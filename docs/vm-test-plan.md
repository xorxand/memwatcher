# Disposable VM test plan

Do this only in a throwaway guest whose disk can be discarded.

## Preparation

1. Give the guest at least 2 GiB RAM and two vCPUs.
2. Install a compiler, make, and headers matching the guest's running kernel.
3. Build with `make clean all test` and snapshot or clone the guest.
4. Confirm there is no valuable data and no passthrough/DMA device assigned.
5. Keep a second console open so kernel logs remain visible after daemon failure.

## Protocol checks before testing RAM

1. Insert without `enabled=1`; confirm `memwatcher info` works but claims return
   permission denied.
2. Confirm an unprivileged user cannot open `/dev/memwatcher`.
3. Confirm the scanner refuses to run without `--yes-i-understand`.
4. Confirm `memwatcher preload --yes-i-understand` refuses the VM unless the
   explicit `--allow-virtualized-preload` laboratory override is supplied.
5. Remove the module, insert it with `enabled=1`, and select one aligned
   pageblock-sized PFN range from an online sysfs memory block.

## One-block smoke test

Run one quick restricted scan with a zero interval. Confirm exactly one ledger
record appears and the guest remains responsive. A skipped claim is valid; try
several aligned ranges until one can migrate. Confirm successful good pages are
returned by checking `memwatcher info` counters and general free-memory behavior.

## Crash containment

While a block is actively being tested, send `SIGKILL` to the child rather than
the parent. Confirm the daemon reports a crashed tester, the entire pageblock is
listed as quarantined, module removal fails, and the guest can no longer suspend.
Reboot the guest to clear the deliberate quarantine.

## Longer soak

Restore the snapshot, enable the normal ten-second interval, and run a quick
sweep before the full pattern set. Monitor kernel logs for allocator warnings,
lockups, RCU stalls, memory-hotplug issues, and unexpected OOM behavior. Exercise
ordinary workload and moderate memory pressure concurrently. Never promote the
build beyond lab use based solely on a clean smoke test.

## Evidence to capture

- exact kernel release and configuration;
- compiler and Memwatcher commit/tag;
- module load parameters;
- daemon command line and state ledger;
- kernel journal from load through reboot; and
- attempted, acquired, skipped, quarantined, and orphaned page counts.
