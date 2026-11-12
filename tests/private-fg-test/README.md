# Private local FG test fixture

This fixture supports the private local 6x FG test integration. It does not run
an external ASI or establish SDK acceptance, generated frames or display output.

`tools/tests/Run-CommonCoreTests.ps1` compiles both translation units with C++20,
x64 and the normal Streamline include path. The executable checks the default
build, provider paths and telemetry, then launches 31 private scenarios in fresh
processes. Do not add a global `F6_PRIVATE_FG_TEST_MODULE` definition or compile
the production `.cpp` as a separate translation unit. Each fixture translation
unit includes that source directly, with distinct namespaces and switches.

Run the fixture with:

```powershell
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1 -SuiteName PrivateFgTestModule -SkipSharedRegression
```

## Boundaries

Runtime writes require the executable to reside in this repository's
`build/common-core-tests`. Every private case creates an exclusive
`private-fg-<pid>-<tick>-<scenario>/plugins` directory. Cases requiring file input
create `MFGUnlocker.asi` containing only `abc`. An owner-path seam directs real
production file IO to that directory. No existing file is overwritten, no user
ASI is accepted, and small case directories remain for inspection.

File open/read/close and BCrypt SHA256 remain real. The mismatch case asserts
the published digest for `abc`. State-machine cases calculate that same digest,
then explicitly replace it through `BCryptFinishHash` to reach later states.
These positive cases do not demonstrate a matching external file's validation.
The hash API failure case calculates the real digest before reporting failure.

Loader, export and Toolhelp calls are mocked. Loaded modules have fake handles;
the only entry is a local test function. Snapshot stand-ins are real closable
event handles. Module inventory failures include snapshot, first-entry and
mid-enumeration failures, with `ERROR_NO_MORE_FILES` required for exhaustion.
Incomplete inventories cannot map a module or query/call its entry.

`NtdllProxy::Module()` returns a distinct fake identity. `Prepare` caches it and
resolves the RTL callback exactly once without calling it, reading memory or
doing file/hash/load/entry work. The callback has the actual
`BOOLEAN (NTAPI*)()` signature and a thread-local return flag. No system RTL
export is looked up or called.

## Real neutral Probe with synthetic memory

`fixture.cpp` includes the actual `runtime/LoaderStartupContext.h` while the
production `ReadProcessMemory` and `VirtualQuery` seams are active. The private
module uses that same inline Probe definition. Neither Probe nor its status
selection/data-range checks are copied into the fixture.

The x64 intrinsic reads only the current thread's PEB pointer. `ReadMemory`
accepts `PEB + 0x50` for synthetic process flags and the resulting `PEB + 0x110` address solely as a request identity and
returns a pointer to a thread-local synthetic `CRITICAL_SECTION`. It never
dereferences that actual PEB slot. Later reads accept only exact-size synthetic
critical-section and debug records, after those records have been queried.
Unexpected handles, addresses, sizes or query/read ordering fail the stub
contract. The fixture never reads or changes the real loader lock.

`QueryMemory` accepts only those two synthetic records and an exact
`MEMORY_BASIC_INFORMATION` buffer. Positive metadata describes committed,
writable, non-executable image data owned by the fake Ntdll identity. Negative
metadata changes one property at a time. All returned bytes come from synthetic
records; no memory-query/read operation is forwarded to Windows.

Counters distinguish RTL calls, total reads, total queries, process-flags reads, PEB-slot reads,
critical-section reads and debug-record reads. A complete proof must issue
exactly `1 / 4 / 2 / 1 / 1 / 1 / 1`, respectively. RTL callout refusal issues
`1 / 0 / 0 / 0 / 0 / 0 / 0`; unknown module/callback issues all zeros. Every invalid
proof has an explicit expected count through its first rejection, ensuring
invalid metadata is rejected before a forbidden record is read. Terminal and
default-build paths issue no proof operations.

The `probe-context` scenario exercises the real neutral Probe directly: null
module/callback, immediate RTL callout, valid read/write and write-copy regions,
another thread's owner, and current-thread ownership with RTL false. Its 26
negative proofs cover failed/short reads, a null lock, failed queries,
uncommitted/private/wrong-module/read-only/executable/guard/no-access regions,
undersized/out-of-range regions, a broken debug backlink and inconsistent
owner/recursion fields. These are controlled proof inputs, not an actual TLS
callback or a measurement of this Windows build's RTL behavior.

## Startup state and waiting

The 32-caller initialization case uses event gates and a blocked local entry.
It verifies one admitted initializer and persistent `Initializing` until that
entry returns. Every terminal case calls both `StartupWorkerUnavailable` and
`Initialize` repeatedly, preserving the original terminal status and operation
counts. No production state is reset between assertions.

`worker-failure` verifies `Pending -> WorkerUnavailable` without initialization
or loader/file/proof work, including subsequent no-op retries.
`waiting-worker-failure` first observes ordinary graphics blocked in a pending
five-second wait, then requires worker failure to publish a terminal state and
wake graphics within one second. It does not create a production worker or
pretend to inject a real `CreateThread` failure. `worker-failure-initializing`
holds the admitted worker inside its entry and sends 128 failure notifications;
these cannot replace `Initializing`, and the original worker must still finish.

Normal graphics waits use a complete synthetic proof in both `Pending` and
`Initializing`. Success and missing-file completion must wake them before the
five-second deadline. Readiness means startup is terminal, including failure.
Timeouts preserve pending status and perform no initialization or export lookup.

RTL-true loader calls refuse without reading the PEB. The worker's own reentrant
wait uses one RTL call and a complete synthetic proof before refusing to wait
for itself. Unknown RTL refuses before `Prepare`, after a null resolution, and
during initialization. `wait-tls` uses RTL false plus the current thread as the
synthetic PEB loader-lock owner, and must refuse promptly in both unfinished
states. `wait-probe-unavailable` repeats all 24 negative proofs in both states;
each must refuse the five-second wait within one second, retain pending status
and avoid loading or initialization side effects.

Every process also tests concurrent default-build `Prepare`, `Initialize`,
`StartupWorkerUnavailable` and `AwaitGraphicsStartup` calls, including zero and
default timeouts. All external-operation and memory-proof counters stay zero.
Provider identity and arbitrary SDK interval telemetry tests remain independent
of capability and options acknowledgement.

This fixture does not compile `Config`, exercise FGHooks wiring, create a real
TLS callout or validate real `CreateThread` failure handling. Product selection
preservation, lock placement, real loader-context behavior and runtime output
remain separate checks. `LoaderStartupContextTests.cpp` exercises the real
Windows executable TLS, main and worker contexts; its runner suite is
`LoaderStartupContext`. Current execution receipts are in
`build/external-fg-{module,loader}-tests.log`.
