# Security and safety policy

Memwatcher is privileged, experimental kernel software whose purpose is to write
destructive patterns to physical RAM. Do not use it on a machine that contains
valuable data or performs important work.

## Supported versions

Only the latest tagged release receives fixes. Version 1.0.0 is a research
baseline compiled against Linux 6.8, not a claim of production readiness.

## Reporting a vulnerability

Do not include hostnames, crash dumps, memory contents, credentials, private
ledger data, or other secrets in a public issue. Contact the maintainer privately
through the security-reporting mechanism on the GitHub repository. Include a
minimal reproducer, kernel release/configuration, architecture, and relevant
sanitized kernel messages. Please allow a reasonable coordinated-disclosure
period before publishing a working exploit.

## Threat model

The `/dev/memwatcher` interface is restricted to `CAP_SYS_RAWIO` and mode 0600.
Only one session is allowed. The mapped VMA cannot be executed, expanded, dumped,
or inherited across fork. Test workers drop identity and set `no_new_privs` only
after the privileged claim and mapping are complete.

These measures reduce accidental and post-open misuse; they do not make a buggy
kernel module safe against a hostile root user. Root can already compromise the
host. The main security goals are preventing unprivileged physical-memory access,
maintaining exclusive ownership, and never returning uncertain pages after a
crash or malformed completion.

The durable bad-PFN file is a denial-of-service control surface: adding arbitrary
PFNs can cause usable memory to be retained. Memwatcher accepts only a regular
file owned by its effective UID with no group/other write bits, refuses symlinks,
caps its record count and size, and binds it to `/etc/machine-id`. Keep
`/var/lib/memwatcher` writable only by root. Automatic replay is disabled in VMs
and containers because their PFNs are not stable hardware identities.
