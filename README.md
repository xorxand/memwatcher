# memwatcher

The price of hardware has gotten to the point where it's not cheap to replace slightly faulty hardware like memory or disk so it occurred to me, I wonder if memory has the same thing that spinning disks do where you can find a bad block of memory and then just not use it and get good reliability out of a stick of memory that has errors on it.

I ran into this problem where I had a zfs pool where both halves of a mirror had a disk error on the same data and it was unrecoverable. I thought to myself "self, what are the chances of that actually happening, that two disks go bad at the same time at the same place."
Well, it turned out to be a memory problem. memtest86 found a handful of bits that were consistently unable to produce reliable results. Seemed a shame to throw out a 32gig stick of memory because of a few bad bits, so I had the klanker make this:


Memwatcher is an experimental Linux kernel module and user-space daemon that
slowly tests *physical* RAM while the machine remains online. It asks Linux to
isolate and migrate one pageblock, maps the now-private pages into a disposable
test process, runs destructive Memtest-style patterns, then either returns good
pages to the allocator or retains suspect pages until reboot.

> [!CAUTION]
> This is low-level, destructive, experimental software. A kernel or hardware
> bug can crash the machine or corrupt data. Version 1.0.0 has been compiled and
> its user-space engine tested on Ubuntu's 6.8 kernel headers; it has **not** had
> destructive in-kernel testing on production hardware. Start in a disposable
> VM with no valuable data. Memwatcher does not replace ECC, EDAC monitoring,
> backups, or an offline tester such as Memtest86+.

## What it does

1. The daemon enumerates online physical-memory ranges from sysfs.
2. A fresh child process asks `/dev/memwatcher` for one pageblock.
3. The module uses Linux's `alloc_contig_range()` path to isolate the range and
   migrate movable contents. If anything unmovable remains, the claim fails and
   is skipped without touching that memory.
4. The module maps only the successfully claimed pages to the child.
5. The child drops to UID/GID 65534, switches to idle scheduling, and runs fixed
   patterns, a March C- sequence, and physical-address-derived patterns. On x86,
   cache lines are flushed before verification by default.
6. The mapping is removed before results are submitted. Good pages are freed;
   pages reported bad remain allocated and the module pins itself until reboot.

If the child crashes, is killed, loses its report, or closes the device before a
successful completion, the module fails closed and retains the entire claim.
The daemon writes an append-only TSV ledger after every attempt.

See [the architecture](docs/architecture.md), [the threat model](SECURITY.md),
and [the VM test plan](docs/vm-test-plan.md) before enabling it.

## Requirements and compatibility

- Linux with `CONFIG_CONTIG_ALLOC`, `CONFIG_MEMORY_ISOLATION`, migration, and
  compaction enabled
- matching kernel headers, a C11 compiler, GNU make, and root for installation
- x86/x86-64 for the default cache-bypassing test mode
- Linux 6.8 is the currently compiled target; out-of-tree kernel APIs are not
  stable, so other releases may require small compatibility changes

The kernel must be able to migrate every page in a candidate pageblock. Kernel
allocations, pinned DMA pages, some huge pages, device memory, reserved ranges,
and other unmovable contents are intentionally skipped. Consequently, an online
sweep cannot promise 100% coverage.

## Build and non-destructive checks

```sh
make
make test
./build/memwatcher selftest --quick --mib 16
./build/memwatcher scan --dry-run --interval 10
```

`selftest` exercises ordinary process memory only. `--dry-run` enumerates the
sweep without opening the kernel device. Neither command claims physical pages.

## First destructive test: disposable VM only

Build inside the guest against its running kernel, snapshot the guest, and make
sure it contains no important data. Then:

```sh
sudo insmod kernel/memwatcher.ko enabled=1
sudo ./build/memwatcher info
sudo ./build/memwatcher scan --quick --passes 1 --interval 10 \
  --yes-i-understand
```

The acknowledgement is deliberately mandatory. To limit the first run, use a
pageblock-aligned half-open range reported in PFNs:

```sh
sudo ./build/memwatcher scan --quick --start-pfn 0xSTART \
  --end-pfn 0xEND --interval 10 --yes-i-understand
```

Failed claims (`EBUSY` or `EINVAL`) are expected as the daemon walks ranges that
contain unmovable or unavailable pages. Stop with SIGINT/SIGTERM. Inspect
`/var/lib/memwatcher/state.tsv` and `memwatcher info` afterward.

Do not unload the module while using the daemon. Once anything is quarantined,
the module intentionally cannot be unloaded and suspend/hibernation is rejected
until the next reboot. Quarantine state is not persistent across reboot.

## Install

```sh
sudo make install
sudo modprobe memwatcher enabled=1
sudo systemctl enable --now memwatcher.service
```

Installing does not load or enable the module. The systemd service starts only
when `/dev/memwatcher` exists. Review its command line before enabling it. DKMS
metadata is included, but distro-specific DKMS registration is left to package
maintainers.

## Commands

```text
memwatcher info
memwatcher selftest [--quick] [--no-cache-flush] [--mib N]
memwatcher scan --yes-i-understand [--forever] [--passes N]
                [--interval SECONDS] [--quick]
                [--start-pfn PFN --end-pfn PFN]
```

Run `memwatcher --help` for the complete option list. `--no-cache-flush` allows
non-x86 testing, but may verify cache rather than DRAM and is therefore weaker.

## Design lineage

Memwatcher borrows the userspace/kernel split and per-PFN scheduling idea from
[RAMpage](https://github.com/neuhalje/kernel-memtest), while replacing its Linux
2.6-era allocator internals with the exported contiguous-allocation path. It
also borrows the slow background sweep concept from
[COMeT](https://people.cs.pitt.edu/~childers/papers/Rahman-PRDC2011.pdf), without
COMeT's invasive allocator patch or claim of a bounded vulnerability window.
Linux's own [early boot memory test](https://github.com/torvalds/linux/blob/master/mm/memtest.c)
is a useful complement because it can cover memory unavailable to a live system.

## License

GPL-2.0-only. See [LICENSE](LICENSE).
