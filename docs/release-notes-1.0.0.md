# Memwatcher 1.0.0

Initial public, research-quality release of a continuous online physical-memory
tester for Linux.

## Included

- Out-of-tree GPL kernel module for exclusive pageblock claims, restricted
  userspace mapping, and fail-closed volatile quarantine.
- Low-priority userspace scanner with disposable workers, fixed/March/address
  test patterns, x86 cache-line flushing, and an append-only result ledger.
- Shared versioned ioctl ABI, DKMS metadata, hardened systemd unit, manual page,
  CI workflow, architecture document, and disposable-VM validation plan.
- Prebuilt x86-64 daemon and an **unsigned, kernel-specific** module for Ubuntu
  `6.8.0-138-generic`, plus source archive and SHA-256 checksums.

## Validation performed

- Clean kernel-module compilation against Ubuntu `6.8.0-138-generic` headers.
- Userspace compilation with `-Wall -Wextra -Wpedantic -Werror`.
- Pattern-engine unit test, cache-flushing self-test, dry-run physical-range
  enumeration, Linux kernel `checkpatch.pl`, artifact inspection, and secret scan.

## Important warning

This release was not loaded for destructive testing on the build workstation.
It must first be exercised using the documented protocol in a disposable VM
with no valuable data. It is not a substitute for ECC, EDAC monitoring, backups,
or offline Memtest86+ testing. The prebuilt `.ko` will load only on a compatible
`6.8.0-138-generic` kernel and may additionally require local Secure Boot signing.
