# Architecture

## Safety boundary

The one non-negotiable invariant is: **user space may write a physical page
only after the kernel has successfully made that page private to the claim.**
Memwatcher therefore does not scan `/dev/mem`, guess whether a page is free, or
overwrite pages still owned by another subsystem.

Linux's `alloc_contig_range(start, end, ...)` is the ownership primitive. It
isolates the containing pageblocks, migrates movable occupants, removes the
requested pages from the allocator, and returns them to the caller. A failed
call produces no mapping and the candidate is skipped. This is conservative:
coverage is lower, but failure to acquire is never treated as permission to
test.

```text
online-memory sysfs              /dev/memwatcher
        |                               |
        v                               v
  parent scheduler  --fork-->  disposable test child
        |                         |  claim(pageblock)
        |                         |  mmap(private pages)
        |                         |  drop privilege + idle priority
        |                         |  patterns / March / address test
        |                         |  munmap
        |                         |  complete(good/bad bitmap)
        |                         v
        +<-- shared report -- kernel frees good pages
        |        |            and retains suspect pages
        |        v
        |   durable bad-PFN set
        v
 append-only TSV ledger
```

## Components

### Kernel module

`kernel/memwatcher.c` is deliberately narrow. It validates a pageblock-sized,
pageblock-aligned PFN range in one zone; refuses reserved, offline, huge, or
already poisoned pages; acquires it through `alloc_contig_range()`; exposes one
non-executable, non-copyable shared mapping; and accepts a bounded bad-page
report only after every VMA is gone.

The module permits one open session globally. That simplifies ownership and
makes concurrent claims impossible in version 1. It blocks memory-hotplug state
changes during an active claim and rejects suspend/hibernate while a claim or
quarantine exists.

The module starts disarmed. `enabled=1` must be explicitly supplied and opening
the device requires `CAP_SYS_RAWIO`. The device is mode `0600`.

### Scanner and test child

`src/main.c` owns policy: discovering online ranges, ordering PFNs, waiting
between attempts, repeating sweeps, recording outcomes, and containing faults.
Each candidate is handled by a newly forked child. After opening and mapping the
privileged device, the child clears supplementary groups, changes to the nobody
UID/GID, sets `no_new_privs`, and requests the lowest practical CPU priority.

If the parent disappears, `PDEATHSIG` kills the child. If either process dies,
the file-release path in the module notices an unfinished claim and retains the
entire block. This can consume memory after operational failures, but avoids
returning potentially half-tested pages to Linux.

### Test engine

`src/memtest.c` currently runs:

- fixed all-zero, all-one, alternating, paired-bit, and nibble patterns;
- a March C- style ascending/descending read-write sequence; and
- normal and inverted values derived from each physical word address.

Before reads, the x86 implementation flushes each cache line and executes a
fence. This makes the test more likely to reach memory rather than merely
verifying a cached copy. It is not equivalent to a platform-aware offline DRAM
tester and does not control refresh rates, memory-controller features, row
mapping, temperature, or voltage.

## Claim state machine

```text
IDLE
  | CLAIM succeeds
  v
CLAIMED -- mmap --> MAPPED -- munmap --> CLAIMED
  |                                  |
  | COMPLETE(good)                   | COMPLETE(bad offsets)
  v                                  v
IDLE (all freed)              IDLE (good freed, bad retained)

CLAIMED/MAPPED -- crash, close, protocol failure --> whole block retained
```

The kernel rejects completion while a mapping exists. Duplicate or out-of-range
bad-page offsets reject the report and leave the claim owned by the session; a
later close then invokes whole-claim quarantine.

## Quarantine semantics

Suspect pages remain allocated to the module; they are not marked `HWPoison` and
are not returned to the buddy allocator. On first quarantine the module takes a
permanent reference to itself, making accidental unload impossible. The list is
queryable via `memwatcher info`. This quarantine is intentionally volatile and
ends only at reboot.

If metadata allocation fails during an abandoned-session cleanup, safety wins:
the pages are leaked deliberately, the aggregate orphan count is increased, and
their individual PFNs may no longer be queryable.

## ABI

`include/memwatcher_uapi.h` defines fixed-width, explicitly padded ioctl records:

- `GET_INFO`: ABI, page and pageblock sizes, counters;
- `CLAIM`: starting PFN and exact page count;
- `COMPLETE`: up to 64 bad offsets or whole-claim quarantine;
- `QUARANTINE`: explicitly abandon the whole claim; and
- `GET_QUARANTINE`: enumerate retained PFNs.

The ABI version is 2. A preload flag lets the kernel reject a supposed preload
after any mapping occurred, and separate counters distinguish completed tests
from restored known-bad pages. An incomplete-test flag prevents crash/error
quarantine from inflating completed coverage. Kernel build assertions protect
record sizes across 32- and 64-bit callers.

## Persistent quarantine

Only completed tests with actual mismatches enter the durable bad-PFN set.
Crash, protocol, and internal-test-error quarantines remain fail-closed for the
current boot but are not labeled as defective hardware for future boots. If the
mismatch report overflows its bounded list, the entire pageblock is persisted.

At the next bare-metal service start, a separate oneshot unit loads the module,
groups recorded PFNs by pageblock, claims each block without mapping it, retains
the known-bad offsets, and frees the remaining pages. A failure to reacquire any
recorded page causes preload to fail, which prevents the scanner service from
starting and makes the loss of protection visible.

The file is tied to `/etc/machine-id` and securely validated. This reduces
accidental replay from a copied state file, but cloned installations can share a
machine ID. It is not a DIMM serial-number database; moving a disk or changing
memory hardware can invalidate the physical-address assumption.

Automatic preload is conditioned on bare metal. Guest PFNs do not stably name
host pages across a VM lifecycle, and a container shares host PFNs without a
container-specific physical boundary.

## Coverage and scheduling

The daemon enumerates sysfs memory blocks marked online, aligns within each to
the kernel's pageblock size, then makes one acquisition attempt per pageblock.
At a common 2 MiB pageblock size, 128 GiB contains 65,536 candidates; a ten-
second interval yields a theoretical minimum sweep time of about 7.6 days, plus
test and migration time. Failed acquisitions reduce actual coverage.

Version 1 does not persist a sophisticated per-PFN priority database. Its ledger
provides enough information to build one later. It also does not pressure Linux
to evict unmovable pages, modify allocator internals, or reserve a tested pool.

## Prior art and deliberate departures

RAMpage demonstrated a kernel/userspace split and reported roughly 70% online
coverage in its era. Its implementation targeted Linux 2.6.34 and copied or
directly manipulated allocator internals; those mechanisms are not reused.
Memwatcher instead accepts lower coverage in exchange for an exported modern
ownership primitive and a much smaller kernel surface.

COMeT integrated tested/untested pools and migration into a Linux 2.6 allocator
to bound the interval between tests. Memwatcher adopts only its background sweep
and low-priority philosophy. It is an out-of-tree diagnostic, not an allocator,
and therefore makes no bounded-coverage or performance guarantee.

## Known gaps

- No destructive VM integration result is claimed for the current code.
- No ARM cache-maintenance implementation; non-x86 requires the weaker opt-out.
- No NUMA-aware ordering or adaptive rate control.
- Persistent quarantine starts after local filesystems become available; it is
  not yet an initramfs-stage reservation.
- No EDAC/RAS event correlation or hardware error injection harness.
- No testing of pages that remain unmovable for an entire sweep.
- No stable-kernel compatibility promise beyond the compiled 6.8 target.

These are reasons to treat Memwatcher as a research-quality baseline, not production
fault containment.
