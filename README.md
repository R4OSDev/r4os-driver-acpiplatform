# R4OS ACPI platform driver

External AML namespace, embedded controller, lid and standard brightness events.
The driver uses pinned MIT-licensed uACPI 6.1.0. It does not implement system sleep.

Build using `./Build.sh` on Linux or `Build.bat` on Windows. Both use the shared
PowerShell 7 SDK build. `Build.sh test` runs the bounded platform integration test.

SCI status is sampled by a dedicated worker; deferred GPE and Notify callbacks
have separate CPU0 workers. This is a polling transport, not an installed IRQ.
Firmware operations never run under the kernel input owner.

AML and Notify workers block on resident binary semaphore events while their
queues are empty. Stop wakes the workers, closes enqueue admission and drains
admitted work before join. Events are created before threads and released
only after all callback producers and workers have joined. A wait failure
withdraws the platform through the existing fault path.

The installed SCI status/ack handler retains its 10-ms polling fallback because
the platform contract has no SCI IRQ registration. Without a handler its worker
blocks. EC GPEs wake the platform worker directly; only controllers without
GPE or with outstanding queries retain 10-ms retries. Otherwise a 1-second
safety sample covers firmware that omits its declared event and the lid.
