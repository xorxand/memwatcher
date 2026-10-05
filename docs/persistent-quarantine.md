# Persistent known-bad page quarantine

Kernel quarantine is intentionally volatile: retained pages disappear from the
allocator only until reboot. The durable layer records confirmed physical PFNs
so they can be acquired again before continuous scanning begins.

## Record policy

`/var/lib/memwatcher/bad-pages.tsv` contains a version header, the installation's
machine ID, and hexadecimal PFNs with observation timestamps. The scanner writes
only results that completed the kernel protocol and contained memory-test
mismatches. It does not persist blocks retained because a worker crashed, an
internal test failed, or a report was malformed.

If more than the ABI's bounded number of pages fail in one pageblock, the whole
pageblock is recorded. This trades capacity for certainty.

Every update is flushed with `fsync()` along with the parent directory before
the scanner continues. The parser deduplicates PFNs and refuses malformed,
oversized, symlinked, wrongly owned, group/world writable, or foreign-machine
files. The cap is 65,536 recorded PFNs.

The separate scan ledger is also opened without following symlinks and only
inside an effective-UID-owned directory without group/other write access. Each
record is flushed before the next attempt and doubles as the restart cursor;
bad-page persistence remains in the independently validated file above.

## Boot sequence

```text
local filesystems available
          |
          v
memwatcher-preload.service (bare metal only)
          |
          +-- modprobe memwatcher enabled=1
          |
          +-- memwatcher preload --yes-i-understand
                    |
                    +-- group known PFNs by pageblock
                    +-- claim/migrate each pageblock
                    +-- retain bad offsets without mmap or testing
                    +-- fail startup if any pageblock is unavailable
          |
          v
memwatcher.service continuous scanner
```

The preload ioctl path is not a new privileged primitive. It uses the same
pageblock claim and bounded result ABI as testing, but the kernel rejects the
preload flag if that session ever created a mapping.

If preload protects some pageblocks before a later one fails, those earlier
blocks remain quarantined and pin the module. Reboot before retrying; repeatedly
starting the unit in that partial state cannot reacquire pages already retained.

## Limits

This occurs after the root filesystem is mounted, not during early kernel boot.
A known-bad PFN could already contain an unmovable allocation by then, causing
preload to fail visibly. Moving the durable set and reservation helper into the
initramfs would narrow that window and is the next logical hardening step.

Physical addresses are meaningful only while the hardware memory map remains
stable. Replacing/rearranging DIMMs, moving the system disk, changing firmware
memory layout, or cloning the installation requires discarding or explicitly
revalidating the file. Clones can retain the same machine ID, so that binding is
a guard against accidents rather than a hardware identity guarantee.

VM replay is disabled because a guest PFN identifies only a guest address; its
host page may change after reboot, ballooning, host swap, or live migration.
Container replay is disabled because containers share the host kernel and have
no private physical PFN namespace.
