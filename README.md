# R4OS ACPI platform driver

External AML namespace, embedded controller, lid and standard brightness events.
The driver uses pinned MIT-licensed uACPI 6.1.0. It does not implement system sleep.

CPU performance uses the separate optional firmware transport added in
0.85.30. Processor devices and legacy Processor objects are matched through
MADT UID/APIC identities to the actual online CPU catalogue. `_CPC` supersedes
`_PSS`/`_PCT`; package sizes, GAS storage widths, ordering, ranges, `_PPC` and
complete `_PSD` domains are checked before control. The initial transport
supports SystemIO and already mapped SystemMemory. PCC and FFH remain
unavailable; no model register or `_OSC` capability grant is guessed.

Requests are copied into a resident bounded admission cell and executed by the
existing platform worker. A callback caller uses ordinary task waits, rather
than dedicated-driver-thread sleep. CPPC requires an already enabled autonomous
interface with reversible min/max/desired fields; no sticky activation is added.
Legacy P-states use declared ordering ranks, with actual control/status receipt.
One leader owns each complete dependency domain; direct CPU tokens cannot own
any member concurrently. SW_ANY additionally requires exactly shared control
descriptors. SW_ALL/HW_ALL require separate nonaliasing member registers.

Original words are captured after exclusive admission and compared in full;
public fingerprints only describe observations. Conflicts, partial writes and
uncertain readbacks retain the original words, exact token, module and CPU
cohort. Stop closes new requests and waits for actual parameter return and
callback completion. Notifications withdraw the catalogue while preserving
sensor-independent return. Restart is required to publish a changed catalogue.

`Build.sh test` / `Build.bat test` extends the existing uACPI integration with
68 synthetic firmware cases covering IO/MMIO CPPC/PSS, nonidentity MADT mapping,
coordination, malformed/unsupported packages, a changed admission baseline,
conflicts, catalogue withdrawal and stop during work. Coordinated P-state
requests/restores reach all members before status waits; the fixture includes
hardware that acknowledges only after the whole group. The private C wire
bridge is checked field by field against the canonical ABI. Regenerate the checked-in
AML headers with `Tests/Compile-PowerFixtures.ps1` and ACPICA `iasl` when changing
the ASL fixture. This is source qualification; real physical actuation and
energy response still require appropriate hardware. The normal Test profile
explicitly includes/loads ACPIPLAT; other profiles keep their existing selection.

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
