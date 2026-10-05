# Memwatcher 1.1.1

This maintenance release fixes correctness, supervision, denial-of-service,
coverage-progress, and file-safety issues found during a full review of 1.1.0.
The ioctl ABI remains version 2.

## Safety and security fixes

- Close the suspend/hibernate race by gating new claims from PM prepare until
  the corresponding post/cancel notification.
- Recheck `CAP_SYS_RAWIO` on every claim and make each successfully claiming
  file session one-shot.
- Require a prior mapping before accepting an ordinary completed-test result.
- Run destructive workers under a dedicated non-login `memwatcher` account
  instead of the shared `nobody` identity.
- Restore `PR_SET_PDEATHSIG` after changing credentials and verify that the
  scheduler parent survived both setup windows.
- Open the root result ledger without following symlinks and validate its file
  and parent-directory ownership, type, and write permissions.
- Prefer `systemd-detect-virt` and add Podman, device-tree, and DMI fallbacks to
  the persistent-PFN virtualization guard.

## Correctness and scalability fixes

- Record parent-side `mmap`, `fork`, and `waitpid` failures as runner errors
  instead of successful memory tests.
- Flush every ledger record and resume the next launch after the last attempted
  PFN, wrapping at the end so repeated restarts converge on full candidate
  coverage.
- Cache sequential quarantine enumeration state in the kernel session, reducing
  normal full enumeration from quadratic to linear work.

## Included artifacts

- Prebuilt x86-64 userspace application.
- **Unsigned, kernel-specific** module built for Ubuntu
  `6.8.0-138-generic`.
- Reproducible source archive and SHA-256 checksum manifest.

## Important warning

The module was compiled and the non-destructive test suite was run, but this
release was not loaded for destructive testing on the build workstation. Test
first in a disposable VM with no valuable data. Memwatcher remains experimental
and is not a substitute for ECC, EDAC monitoring, backups, or offline
Memtest86+ testing. The prebuilt module requires a compatible kernel and may
require local Secure Boot signing.
