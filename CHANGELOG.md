# Changelog

## 1.0.0 - 2026-09-13

- Initial research-quality release.
- Add a Linux 6.8 kernel module using pageblock-aligned `alloc_contig_range()`
  claims, restricted mapping, fail-closed session cleanup, and volatile bad-page
  quarantine.
- Add a low-priority user-space scanner with disposable workers, sysfs physical
  range discovery, fixed/March/address patterns, x86 cache flushing, and a TSV
  result ledger.
- Add DKMS metadata, a hardened systemd unit, architecture and VM test docs,
  unit/self tests, release packaging, and CI.
