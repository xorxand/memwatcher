# Memwatcher 1.1.0

This release adds durable, bare-metal quarantine for physical pages that fail a
completed memory test. Confirmed bad PFNs can now be retained again early on the
next boot, allowing the machine to remember known failures across restarts.

## Highlights

- Persist confirmed bad PFNs in a securely opened, root-owned, machine-bound
  file at `/var/lib/memwatcher/bad-pages.tsv`.
- Add `memwatcher preload --yes-i-understand` and a dedicated systemd oneshot
  service that loads the module and retains known-bad pages before scanning.
- Refuse automatic persistent-PFN replay in virtual machines and containers,
  where guest PFNs are not stable physical hardware identities.
- Skip already quarantined pageblocks during later scans.
- Introduce ioctl ABI version 2 with separate completed-test and preloaded-page
  accounting, plus explicit preload and incomplete-result flags.
- Document the persistence design, boot ordering, threat model, and operational
  limitations.

## Included artifacts

- Prebuilt x86-64 userspace application.
- **Unsigned, kernel-specific** module built for Ubuntu
  `6.8.0-138-generic`.
- Reproducible source archive and SHA-256 checksum manifest.

## Validation performed

- Kernel-module compilation against Ubuntu `6.8.0-138-generic` headers.
- Userspace compilation with strict warnings enabled.
- Unit tests for the memory patterns and persistent record handling.
- Userspace self-test, non-destructive scan dry run, Linux kernel checkpatch,
  compiler static analysis, sanitizer runs, artifact inspection, and secret
  scanning.

## Important warning

This release was not loaded for destructive testing on the build workstation.
Use the documented protocol in a disposable VM with no valuable data before any
bare-metal trial. Online testing cannot cover pages the kernel cannot migrate,
and persistent PFNs are meaningful only on stable bare-metal hardware. This is
experimental software, not a substitute for ECC, EDAC monitoring, backups, or
offline Memtest86+ testing. The prebuilt `.ko` requires a compatible
`6.8.0-138-generic` kernel and may require local Secure Boot signing.
