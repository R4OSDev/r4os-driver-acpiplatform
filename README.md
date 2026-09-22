# R4OS ACPI platform driver

External AML namespace, embedded controller, lid and standard brightness events.
The driver uses pinned MIT-licensed uACPI 6.1.0. It does not implement system sleep.

Build using `./Build.sh` on Linux or `Build.bat` on Windows. Both use the shared
PowerShell 7 SDK build. `Build.sh test` runs the bounded platform integration test.

SCI status is sampled by a dedicated worker; deferred GPE and Notify callbacks
have separate CPU0 workers. This is a polling transport, not an installed IRQ.
Firmware operations never run under the kernel input owner.
