# Contributing

Small, auditable changes are preferred. Keep memory ownership, quarantine, and
failure behavior in the kernel; keep scheduling and test policy in user space.
Never add a path that writes a PFN unless ownership was first established by the
kernel.

Before submitting a change:

```sh
make clean
make
make test
```

Kernel-path changes should also include evidence from a disposable VM and note
the tested kernel configurations. Never test a development module on a machine
with valuable data. Do not attach unsanitized dumps or credentials to issues.

Contributions are accepted under GPL-2.0-only, matching the project license.
