#include "Harness.h"
#include "stubs/proxies/Ntdll_Proxy.h"
#include "stubs/proxies/KernelBase_Proxy.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>

#define F6_PRIVATE_FG_TEST_MODULE 1
#include "ProductionSeams.h"
#include "../../OptiScaler/runtime/LoaderStartupContext.h"
#include "../../OptiScaler/framegen/dlssg/PrivateFgTestModule.cpp"
#include "EndProductionSeams.h"
#undef F6_PRIVATE_FG_TEST_MODULE

namespace PrivateFgFixture
{
namespace fs = std::filesystem;
namespace Module = framegen::PrivateFgTestModule;
State state;
thread_local LoaderProof loaderProof = LoaderProof::Ready;
namespace
{
unsigned checks = 0;
constexpr DWORD timeoutMs = 10000;
constexpr std::wstring_view scenarios[] {
    L"startup", L"aliases", L"missing", L"hash-mismatch", L"hash-api-failure",
    L"duplicate-name", L"duplicate-alias", L"duplicate-marker", L"snapshot-failure",
    L"enumeration-failure", L"enumeration-mid-failure", L"load-failure", L"entry-missing",
    L"initialize-once", L"concurrent", L"wait-graphics", L"wait-pending",
    L"wait-loader", L"wait-worker", L"wait-unknown", L"wait-timeout", L"wait-terminal-failure",
    L"worker-failure", L"waiting-worker-failure", L"worker-failure-initializing",
    L"probe-context", L"wait-tls", L"wait-probe-unavailable",
    L"wait-process-init", L"wait-process-init-read-failure", L"wait-process-init-short-read" };
// Only state-machine cases replace a real completed digest with this value.
constexpr std::array<unsigned char, 32> controlledDigest {
    0x17,0x1A,0xC1,0x8D,0x4D,0x63,0x2D,0x12,0xDE,0xE2,0x2D,0x5C,0xAF,0x20,0x85,0xF3,
    0x01,0xBA,0xCF,0xDB,0x9A,0x46,0x25,0x61,0xD9,0x3D,0xBC,0xBB,0x61,0xD6,0xA4,0xF8 };
constexpr std::array<unsigned char, 32> abcDigest {
    0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,
    0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD };

HMODULE LoadedModule() { return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x70000000)); }
HMODULE NtdllModule() { return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x71000000)); }
HMODULE EnumeratedModule(size_t index) { return reinterpret_cast<HMODULE>(static_cast<uintptr_t>(0x1000 + index)); }
thread_local bool insideLoaderCallout = false;

struct SyntheticLoaderLock
{
    CRITICAL_SECTION lock {};
    RTL_CRITICAL_SECTION_DEBUG debug {};
    bool lockQueried = false, debugQueried = false;
    bool flagsRead = false;

    void Reset()
    {
        lock = {};
        debug = {};
        lockQueried = debugQueried = false;
        flagsRead = false;
        lock.DebugInfo = &debug;
        debug.CriticalSection = &lock;
        if (loaderProof == LoaderProof::CurrentThreadOwner || loaderProof == LoaderProof::OwnerWithoutRecursion)
        {
            lock.OwningThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId()));
            lock.RecursionCount = loaderProof == LoaderProof::OwnerWithoutRecursion ? 0 : 1;
        }
        else if (loaderProof == LoaderProof::OtherThreadOwner)
        {
            lock.OwningThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId()) + 1);
            lock.RecursionCount = 1;
        }
        else if (loaderProof == LoaderProof::NegativeRecursion) lock.RecursionCount = -1;
        else if (loaderProof == LoaderProof::RecursionWithoutOwner) lock.RecursionCount = 1;
        if (loaderProof == LoaderProof::BadBacklink) debug.CriticalSection = nullptr;
    }
};
thread_local SyntheticLoaderLock synthetic;

// Order: RTL calls, ReadProcessMemory calls, VirtualQuery calls, process-flags
// reads, PEB-lock-slot reads, critical-section reads, debug-record reads.
// Only expected counts live here;
// all validation and status selection remain in the production header.
using ProbeCounts = std::array<unsigned, 7>;
ProbeCounts ProbeWork()
{
    return { state.rtlCalls.load(), state.memoryReadCalls.load(), state.memoryQueryCalls.load(),
             state.processFlagsReadCalls.load(), state.pebReadCalls.load(),
             state.lockReadCalls.load(), state.debugReadCalls.load() };
}

void ProbeDelta(const ProbeCounts& before, const ProbeCounts& expected)
{
    auto observed = ProbeWork();
    for (size_t i = 0; i < observed.size(); ++i) observed[i] -= before[i];
    Check(observed == expected, "probe performs exactly the expected RTL/query/bounded-read operations");
    Check(!state.badMemoryArguments, "memory proof only queries known records and reads exact permitted ranges");
}

struct InvalidProof
{
    LoaderProof mode;
    ProbeCounts calls;
};
constexpr InvalidProof invalidProofs[] {
    { LoaderProof::ProcessFlagsReadFailure, { 1, 1, 0, 1, 0, 0, 0 } },
    { LoaderProof::ProcessFlagsShortRead, { 1, 1, 0, 1, 0, 0, 0 } },
    { LoaderProof::PebReadFailure, { 1, 2, 0, 1, 1, 0, 0 } },
    { LoaderProof::PebShortRead, { 1, 2, 0, 1, 1, 0, 0 } },
    { LoaderProof::NullLock, { 1, 2, 0, 1, 1, 0, 0 } },
    { LoaderProof::LockQueryFailure, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockUncommitted, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockPrivate, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockWrongModule, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockReadOnly, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockExecutable, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockGuard, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockNoAccess, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockShortRange, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockOutsideRange, { 1, 2, 1, 1, 1, 0, 0 } },
    { LoaderProof::LockReadFailure, { 1, 3, 1, 1, 1, 1, 0 } },
    { LoaderProof::LockShortRead, { 1, 3, 1, 1, 1, 1, 0 } },
    { LoaderProof::DebugQueryFailure, { 1, 3, 2, 1, 1, 1, 0 } },
    { LoaderProof::DebugWrongModule, { 1, 3, 2, 1, 1, 1, 0 } },
    { LoaderProof::DebugShortRange, { 1, 3, 2, 1, 1, 1, 0 } },
    { LoaderProof::DebugReadFailure, { 1, 4, 2, 1, 1, 1, 1 } },
    { LoaderProof::DebugShortRead, { 1, 4, 2, 1, 1, 1, 1 } },
    { LoaderProof::BadBacklink, { 1, 4, 2, 1, 1, 1, 1 } },
    { LoaderProof::NegativeRecursion, { 1, 4, 2, 1, 1, 1, 1 } },
    { LoaderProof::OwnerWithoutRecursion, { 1, 4, 2, 1, 1, 1, 1 } },
    { LoaderProof::RecursionWithoutOwner, { 1, 4, 2, 1, 1, 1, 1 } }
};

BOOLEAN NTAPI MockLoaderCallout()
{
    ++state.rtlCalls;
    if (state.loaderProbed) SetEvent(state.loaderProbed);
    return static_cast<BOOLEAN>(insideLoaderCallout);
}

struct Handle
{
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

void MockInitialize()
{
    ++state.initCalls;
    if (state.awaitInsideEntry)
    {
        const auto started = GetTickCount64();
        state.workerAwaitResult = Module::AwaitGraphicsStartup(5000);
        state.workerAwaitElapsed = GetTickCount64() - started;
        state.workerAwaitPending = Module::CurrentStatus() == Module::Status::Initializing && Module::ProviderPending();
        state.workerAwaitCalled = true;
    }
    if (!state.blockEntry) return;
    if (!SetEvent(state.entryEntered) || WaitForSingleObject(state.releaseEntry, timeoutMs) != WAIT_OBJECT_0)
        state.entryWaitFailed = true;
}

void CopyModule(LPMODULEENTRY32W entry, size_t index)
{
    entry->hModule = EnumeratedModule(index);
    wcsncpy_s(entry->szModule, _countof(entry->szModule), state.modules[index].c_str(), _TRUNCATE);
}

fs::path ExecutablePath()
{
    std::wstring name(32768, L'\0');
    const auto size = ::GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    Check(size && size < name.size(), "read fixture executable path");
    name.resize(size);
    return fs::canonical(name);
}

fs::path OutputRoot(const fs::path& executable)
{
    // Runner owns this output directory. Never derive a writable destination
    // from a supplied ASI path or from the working directory.
    const auto repository = fs::canonical(fs::path(__FILE__)).parent_path().parent_path().parent_path();
    const auto intended = repository / L"build" / L"common-core-tests";
    const auto expected = fs::canonical(intended);
    Check(expected == intended, "Runner output cannot redirect through a junction or symlink");
    Check(executable.parent_path() == expected, "fixture must run from repository build/common-core-tests");
    return expected;
}

void PrepareCase(const fs::path& executable, std::wstring_view name)
{
    const auto output = OutputRoot(executable);
    const auto directory = output / (L"private-fg-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                     std::to_wstring(GetTickCount64()) + L"-" + std::wstring(name));
    Check(fs::create_directory(directory), "create exclusive fresh case output directory");
    Check(fs::canonical(directory).parent_path() == output, "case output remains in Runner directory");
    state.ownerPath = directory / L"fixture-owner.exe";
    Check(fs::create_directory(directory / L"plugins"), "create case plugins directory");
    if (name == L"startup" || name == L"missing" || name == L"aliases" || name == L"wait-terminal-failure" ||
        name == L"worker-failure" || name == L"waiting-worker-failure" || name == L"probe-context") return;
    const auto input = directory / L"plugins" / L"MFGUnlocker.asi";
    Handle file(CreateFileW(input.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(file.value != INVALID_HANDLE_VALUE, "create synthetic fixture file exclusively");
    DWORD written = 0;
    Check(WriteFile(file.value, "abc", 3, &written, nullptr) && written == 3, "seed real SHA256/IO input");
}

void Aliases()
{
    for (const auto* name : { L"MFGUnlocker.asi", L"mfgunlocker.ASI", L"MfGuNlOcKeR-private.aSi",
                             L"MFGUnlocker64.asi", L"MFGUnlocker-copy.asi" })
        Check(Module::OwnsAsiName(name), "private build recognizes reserved ASI alias");
    for (const auto* name : { L"", L".asi", L"MFGUnlocke.asi", L"OtherMFGUnlocker.asi",
                             L"MFGUnlocker.dll", L"MFGUnlocker.asi.bak", L"MFGUnlocker", L"other.asi" })
        Check(!Module::OwnsAsiName(name), "private build rejects unrelated ASI names");
    const std::wstring storage = L"MFGUnlocker.asi.trailing";
    Check(Module::OwnsAsiName(std::wstring_view(storage.data(), std::wstring_view(L"MFGUnlocker.asi").size())),
          "bounded alias view need not be null terminated");
}

void ConcurrentInitialize()
{
    state.blockEntry = true;
    Handle entered(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Handle release(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(entered.value && release.value, "create deterministic entry gates");
    state.entryEntered = entered.value;
    state.releaseEntry = release.value;
    constexpr unsigned callerCount = 32;
    std::atomic_uint ready { 0 };
    std::atomic_bool start { false };
    std::vector<std::thread> callers;
    // Gate all callers before the CAS. The winning entry stays blocked while
    // all other callers compete and while the main thread observes pending.
    try
    {
        for (unsigned i = 0; i < callerCount; ++i)
            callers.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (unsigned repeat = 0; repeat < 32; ++repeat) Module::Initialize();
            });
    }
    catch (...)
    {
        start.store(true, std::memory_order_release);
        SetEvent(release.value);
        for (auto& caller : callers) caller.join();
        throw;
    }
    while (ready.load(std::memory_order_acquire) != callerCount) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    const bool observedEntry = WaitForSingleObject(entered.value, timeoutMs) == WAIT_OBJECT_0;
    const bool initializing = Module::CurrentStatus() == Module::Status::Initializing;
    const bool pending = Module::ProviderPending();
    const bool oneEntry = state.initCalls == 1 && state.loadCalls == 1 && state.entryLookups == 1;
    // An extra burst after the entry is reached exercises the losing CAS while
    // the winner is still inside InitializeASI, without timing-based sleeps.
    for (unsigned i = 0; i < 128; ++i) Module::Initialize();
    SetEvent(release.value);
    for (auto& caller : callers) caller.join();
    Check(observedEntry, "concurrent winner reaches mocked entry");
    Check(initializing && pending, "provider stays pending while actual initialization is unfinished");
    Check(oneEntry && state.initCalls == 1 && !state.entryWaitFailed, "concurrent CAS admits exactly one entry");
}

void Terminal(Module::Status wanted)
{
    Check(Module::CurrentStatus() == wanted, "expected terminal status");
    Check(!Module::ProviderPending(), "terminal status releases provider pending gate");
    Check(std::strcmp(Module::StatusText(), "unknown") != 0, "terminal status has diagnostic text");
    const auto loads = state.loadCalls.load(), entries = state.initCalls.load();
    const auto lookups = state.resolveCalls.load(), hashes = state.finishHashCalls.load();
    const auto paths = state.fileNameCalls.load(), snapshots = state.snapshotCalls.load();
    const auto first = state.firstModuleCalls.load(), next = state.nextModuleCalls.load();
    const auto probes = state.rtlCalls.load(), rtl = state.rtlLookups.load();
    const auto ntdll = state.ntdllModuleCalls.load();
    const auto proof = ProbeWork();
    Check(Module::AwaitGraphicsStartup(0) && Module::AwaitGraphicsStartup(),
          "every terminal status immediately permits graphics startup");
    Check(state.rtlCalls == probes && state.rtlLookups == rtl && state.ntdllModuleCalls == ntdll,
          "terminal graphics startup bypasses loader-callout resolution and probing");
    for (unsigned i = 0; i < 128; ++i)
    {
        Module::StartupWorkerUnavailable();
        Module::Initialize();
    }
    Check(Module::CurrentStatus() == wanted && !Module::ProviderPending(), "terminal status is stable");
    Check(state.loadCalls == loads && state.initCalls == entries && state.resolveCalls == lookups &&
          state.finishHashCalls == hashes && state.fileNameCalls == paths && state.snapshotCalls == snapshots &&
          state.firstModuleCalls == first && state.nextModuleCalls == next,
          "completed Initialize never retries file/hash/load/entry work");
    Check(ProbeWork() == proof, "terminal awaits and repeated worker failures perform no memory proof");
}

auto StartupWork()
{
    return std::array { state.fileNameCalls.load(), state.fileOpenCalls.load(), state.fileReadCalls.load(),
        state.snapshotCalls.load(), state.firstModuleCalls.load(), state.nextModuleCalls.load(),
        state.finishHashCalls.load(), state.loadCalls.load(), state.resolveCalls.load(),
        state.entryLookups.load(), state.initCalls.load(), state.ntdllModuleCalls.load(), state.rtlLookups.load() };
}

void PrepareWait()
{
    Module::Prepare();
    Check(state.ntdllModuleCalls == 1 && state.rtlLookups == 1 && state.resolveCalls == 1 && state.rtlCalls == 0,
          "Prepare caches exactly one mocked Ntdll RTL export without invoking it");
    Check(state.fileNameCalls == 0 && state.fileOpenCalls == 0 && state.fileReadCalls == 0 &&
          state.finishHashCalls == 0 && state.snapshotCalls == 0 && state.loadCalls == 0 && state.initCalls == 0 &&
          state.memoryReadCalls == 0 && state.memoryQueryCalls == 0,
          "private Prepare performs no file IO, hash, inventory, load or initialization");
}

struct StartupWorker
{
    Handle entered { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    Handle release { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    std::jthread thread;

    StartupWorker()
    {
        Check(entered.value && release.value, "create startup-worker gates");
        state.entryEntered = entered.value;
        state.releaseEntry = release.value;
        state.blockEntry = true;
        thread = std::jthread([] { Module::Initialize(); });
    }
    bool Reached() const { return WaitForSingleObject(entered.value, timeoutMs) == WAIT_OBJECT_0; }
    void Finish()
    {
        SetEvent(release.value);
        if (thread.joinable()) thread.join();
    }
    ~StartupWorker() { Finish(); }
};

void GraphicsWait(bool startsPending, bool missing)
{
    using namespace std::chrono_literals;
    PrepareWait();
    Handle probed(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Check(probed.value != nullptr, "create graphics loader-probe observation event");
    state.loaderProbed = probed.value;
    std::unique_ptr<StartupWorker> worker;
    bool reached = true;
    if (!startsPending)
    {
        worker = std::make_unique<StartupWorker>();
        reached = worker->Reached();
    }
    auto graphics = std::async(std::launch::async, [] { return Module::AwaitGraphicsStartup(5000); });
    const bool sawProbe = WaitForSingleObject(probed.value, timeoutMs) == WAIT_OBJECT_0;
    const bool blocked = graphics.wait_for(100ms) == std::future_status::timeout;
    const bool pending = Module::ProviderPending() && Module::CurrentStatus() ==
        (startsPending ? Module::Status::Pending : Module::Status::Initializing);
    bool blockedDuringEntry = true;
    if (missing) Module::Initialize();
    else
    {
        if (!worker)
        {
            worker = std::make_unique<StartupWorker>();
            reached = worker->Reached();
        }
        blockedDuringEntry = graphics.wait_for(100ms) == std::future_status::timeout &&
            Module::CurrentStatus() == Module::Status::Initializing && Module::ProviderPending();
        worker->Finish();
    }
    const bool notified = graphics.wait_for(1s) == std::future_status::ready;
    const bool result = graphics.get();
    state.loaderProbed = nullptr;
    Check(sawProbe && blocked && pending, "ordinary graphics actually waits while startup is unfinished");
    Check(reached && blockedDuringEntry && !state.entryWaitFailed, "graphics cannot pass blocked InitializeASI");
    Check(notified && result, "terminal publication wakes graphics well before its five-second deadline");
    Check(state.rtlCalls == 1 && state.rtlLookups == 1 && state.ntdllModuleCalls == 1,
          "graphics uses cached RTL callback without resolving it again");
    ProbeDelta({}, { 1, 4, 2, 1, 1, 1, 1 });
    Terminal(missing ? Module::Status::Missing : Module::Status::InitReturned);
    Check(state.initCalls == (missing ? 0u : 1u) && state.loadCalls == (missing ? 0u : 1u),
          "waiting leaves actual initialization exclusively to the startup worker");
}

void RefusedWait(bool loader, Module::Status expected = Module::Status::Pending,
                 LoaderProof proof = LoaderProof::Ready)
{
    using namespace std::chrono_literals;
    const auto before = StartupWork();
    auto graphics = std::async(std::launch::async, [loader, proof] {
        insideLoaderCallout = loader;
        loaderProof = proof;
        return Module::AwaitGraphicsStartup(5000);
    });
    const bool immediate = graphics.wait_for(1s) == std::future_status::ready;
    const bool result = graphics.get();
    Check(immediate && !result, "unsafe or unknown loader context refuses the five-second wait immediately");
    Check(Module::CurrentStatus() == expected && Module::ProviderPending(),
          "refused wait cannot complete or fail pending startup");
    Check(StartupWork() == before, "refused wait does not resolve, initialize or access files");
}

void TimeoutWait()
{
    PrepareWait();
    const auto checkTimeout = [](Module::Status expected) {
        const auto before = StartupWork();
        const auto proof = ProbeWork();
        const auto started = GetTickCount64();
        const bool result = Module::AwaitGraphicsStartup(80);
        const auto elapsed = GetTickCount64() - started;
        Check(!result && elapsed >= 40 && elapsed < 1000, "ordinary graphics honors a bounded startup timeout");
        Check(Module::CurrentStatus() == expected && Module::ProviderPending(),
              "timeout preserves Pending or Initializing rather than publishing failure");
        Check(StartupWork() == before, "timeout performs no initialization, file IO or export resolution");
        ProbeDelta(proof, { 1, 4, 2, 1, 1, 1, 1 });
    };
    checkTimeout(Module::Status::Pending);
    StartupWorker worker;
    const bool reached = worker.Reached();
    // Worker RAII always releases the fake entry, including on assertion failure.
    checkTimeout(Module::Status::Initializing);
    worker.Finish();
    Check(reached && !state.entryWaitFailed, "timeout leaves the startup worker free to complete later");
    Terminal(Module::Status::InitReturned);
}

void ProbeContract()
{
    using Context = LoaderStartupContext::Status;
    const auto untouched = StartupWork();
    const auto checkProbe = [](HMODULE module, LoaderStartupContext::ProbeFunction callback,
                               Context expected, const ProbeCounts& calls) {
        const auto before = ProbeWork();
        Check(LoaderStartupContext::Probe(module, callback) == expected,
              "real neutral Probe classifies the supplied loader proof");
        ProbeDelta(before, calls);
    };
    checkProbe(nullptr, &MockLoaderCallout, Context::ProbeUnavailable, {});
    checkProbe(NtdllModule(), nullptr, Context::ProbeUnavailable, {});
    insideLoaderCallout = true;
    loaderProof = LoaderProof::PebReadFailure;
    checkProbe(NtdllModule(), &MockLoaderCallout, Context::LoaderCallout, { 1, 0, 0, 0, 0, 0, 0 });
    insideLoaderCallout = false;
    for (const auto proof : { LoaderProof::Ready, LoaderProof::WriteCopy, LoaderProof::OtherThreadOwner,
                              LoaderProof::OtherProcessFlags })
    {
        loaderProof = proof;
        checkProbe(NtdllModule(), &MockLoaderCallout, Context::Ready, { 1, 4, 2, 1, 1, 1, 1 });
    }
    loaderProof = LoaderProof::ProcessInitializing;
    checkProbe(NtdllModule(), &MockLoaderCallout, Context::LoaderCallout, { 1, 1, 0, 1, 0, 0, 0 });
    loaderProof = LoaderProof::CurrentThreadOwner;
    checkProbe(NtdllModule(), &MockLoaderCallout, Context::LoaderCallout, { 1, 4, 2, 1, 1, 1, 1 });
    for (const auto& invalid : invalidProofs)
    {
        loaderProof = invalid.mode;
        checkProbe(NtdllModule(), &MockLoaderCallout, Context::ProbeUnavailable, invalid.calls);
    }
    loaderProof = LoaderProof::Ready;
    Check(StartupWork() == untouched && Module::CurrentStatus() == Module::Status::Pending,
          "direct neutral proof tests cannot resolve modules, initialize or complete private startup");
}

void ProofWait(std::span<const InvalidProof> proofs)
{
    PrepareWait();
    const auto refuse = [&](Module::Status expected) {
        for (const auto& proof : proofs)
        {
            const auto before = ProbeWork();
            RefusedWait(false, expected, proof.mode);
            ProbeDelta(before, proof.calls);
        }
    };
    refuse(Module::Status::Pending);
    StartupWorker worker;
    const bool reached = worker.Reached();
    refuse(Module::Status::Initializing);
    worker.Finish();
    Check(reached && !state.entryWaitFailed, "rejected loader proofs leave the actual worker free to finish");
    Check(state.ntdllModuleCalls == 1 && state.rtlLookups == 1,
          "refused proofs use the cached Ntdll identity and RTL callback");
    Terminal(Module::Status::InitReturned);
}

void WorkerFailure(bool waiting)
{
    using namespace std::chrono_literals;
    Handle probed;
    std::future<bool> graphics;
    bool observed = true, blocked = true;
    if (waiting)
    {
        PrepareWait();
        probed.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(probed.value != nullptr, "create worker-failure graphics probe observation");
        state.loaderProbed = probed.value;
        graphics = std::async(std::launch::async, [] { return Module::AwaitGraphicsStartup(5000); });
        observed = WaitForSingleObject(probed.value, timeoutMs) == WAIT_OBJECT_0;
        blocked = graphics.wait_for(100ms) == std::future_status::timeout;
    }
    const auto before = StartupWork();
    const bool pending = Module::CurrentStatus() == Module::Status::Pending && Module::ProviderPending();
    Module::StartupWorkerUnavailable();
    bool notified = true, ready = true;
    if (waiting)
    {
        notified = graphics.wait_for(1s) == std::future_status::ready;
        ready = graphics.get();
        state.loaderProbed = nullptr;
        ProbeDelta({}, { 1, 4, 2, 1, 1, 1, 1 });
    }
    Check(pending && observed && blocked, "worker-failure publication starts with actual pending graphics");
    Check(notified && ready, "worker failure wakes graphics before its five-second deadline");
    Check(StartupWork() == before, "worker failure never performs file/hash/inventory/load/export/entry work");
    Terminal(Module::Status::WorkerUnavailable);
    Check(state.fileNameCalls == 0 && state.fileOpenCalls == 0 && state.finishHashCalls == 0 &&
          state.snapshotCalls == 0 && state.loadCalls == 0 && state.initCalls == 0,
          "worker-unavailable terminal never attempts a delayed initialization");
    if (!waiting) ProbeDelta({}, {});
}

void WorkerFailureDuringInitialization()
{
    state.forceDigest = true;
    PrepareWait();
    StartupWorker worker;
    const bool reached = worker.Reached();
    const auto before = StartupWork();
    for (unsigned i = 0; i < 128; ++i) Module::StartupWorkerUnavailable();
    const bool retained = Module::CurrentStatus() == Module::Status::Initializing && Module::ProviderPending();
    Check(StartupWork() == before, "late worker-failure notifications have no initialization side effects");
    worker.Finish();
    Check(reached && retained && !state.entryWaitFailed, "worker failure cannot overwrite an admitted Initializing worker");
    Terminal(Module::Status::InitReturned);
    Check(state.finishHashCalls == 1 && state.realDigest == abcDigest && state.initCalls == 1,
          "the retained worker completes its real abc hash and one controlled entry call");
    ProbeDelta({}, {});
}

void WaitingCase(std::wstring_view name)
{
    using namespace std::chrono_literals;
    if (name == L"wait-graphics") GraphicsWait(false, false);
    else if (name == L"wait-pending") GraphicsWait(true, false);
    else if (name == L"wait-terminal-failure") GraphicsWait(true, true);
    else if (name == L"wait-timeout") TimeoutWait();
    else if (name == L"wait-probe-unavailable") ProofWait(invalidProofs);
    else if (name == L"wait-tls" || name == L"wait-process-init" ||
             name == L"wait-process-init-read-failure" || name == L"wait-process-init-short-read")
    {
        const auto mode = name == L"wait-tls" ? LoaderProof::CurrentThreadOwner :
                          name == L"wait-process-init" ? LoaderProof::ProcessInitializing :
                          name == L"wait-process-init-read-failure" ? LoaderProof::ProcessFlagsReadFailure :
                          LoaderProof::ProcessFlagsShortRead;
        const InvalidProof proofs[] { { mode, name == L"wait-tls" ?
            ProbeCounts { 1, 4, 2, 1, 1, 1, 1 } : ProbeCounts { 1, 1, 0, 1, 0, 0, 0 } } };
        ProofWait(proofs);
    }
    else if (name == L"wait-worker")
    {
        PrepareWait();
        state.awaitInsideEntry = true;
        auto worker = std::async(std::launch::async, [] { Module::Initialize(); });
        const bool immediate = worker.wait_for(1s) == std::future_status::ready;
        worker.get();
        Check(immediate && state.workerAwaitCalled && !state.workerAwaitResult && state.workerAwaitElapsed < 1000,
              "initializing worker refuses to wait for itself");
        Check(state.workerAwaitPending && state.rtlCalls == 1, "worker refusal preserves its own Initializing state");
        ProbeDelta({}, { 1, 4, 2, 1, 1, 1, 1 });
        Terminal(Module::Status::InitReturned);
    }
    else if (name == L"wait-loader")
    {
        PrepareWait();
        RefusedWait(true);
        StartupWorker worker;
        const bool reached = worker.Reached();
        RefusedWait(true, Module::Status::Initializing);
        worker.Finish();
        Check(reached && !state.entryWaitFailed && state.rtlCalls == 2,
              "loader-callout guard refuses both pending states with the exact BOOLEAN NTAPI stub");
        ProbeDelta({}, { 2, 0, 0, 0, 0, 0, 0 });
        Terminal(Module::Status::InitReturned);
    }
    else
    {
        Check(name == L"wait-unknown", "known waiting scenario");
        RefusedWait(false); // No Prepare: cached RTL callback is unknown.
        state.rtlAvailable = false;
        PrepareWait();
        RefusedWait(false); // Prepare could not resolve the callback either.
        StartupWorker worker;
        const bool reached = worker.Reached();
        RefusedWait(false, Module::Status::Initializing);
        worker.Finish();
        Check(reached && !state.entryWaitFailed, "unknown RTL also refuses during initialization");
        Check(state.rtlCalls == 0, "unknown RTL callback is never invoked");
        ProbeDelta({}, {});
        Terminal(Module::Status::InitReturned);
    }
    if (state.finishHashCalls != 0)
        Check(state.finishHashCalls == 1 && state.realDigest == abcDigest,
              "waiting state cases still calculate real SHA256 before controlled digest substitution");
    Check(!state.badLoaderArguments, "waiting uses only the fixture module and known mock exports");
}

void Case(const fs::path& executable, std::wstring_view name)
{
    Check(std::find(std::begin(scenarios), std::end(scenarios), name) != std::end(scenarios), "known scenario");
    Check(Module::Enabled() && Module::CurrentStatus() == Module::Status::Pending && Module::ProviderPending(),
          "private startup is pending before explicit Initialize");
    Check(state.loadCalls == 0 && state.resolveCalls == 0 && state.initCalls == 0,
          "private startup does not load or call entry eagerly");
    PrepareCase(executable, name);
    if (name == L"worker-failure" || name == L"waiting-worker-failure")
    {
        WorkerFailure(name == L"waiting-worker-failure");
        return;
    }
    if (name == L"worker-failure-initializing")
    {
        WorkerFailureDuringInitialization();
        return;
    }
    if (name == L"probe-context")
    {
        ProbeContract();
        return;
    }
    if (name.starts_with(L"wait-"))
    {
        state.forceDigest = true;
        WaitingCase(name);
        return;
    }
    if (name == L"startup" || name == L"aliases")
    {
        if (name == L"aliases") Aliases();
        Check(Module::CurrentStatus() == Module::Status::Pending && Module::ProviderPending(),
              "queries and alias recognition cannot complete pending initialization");
        Check(state.fileNameCalls == 0 && state.snapshotCalls == 0 && state.finishHashCalls == 0 &&
              state.loadCalls == 0 && state.resolveCalls == 0, "startup queries have no loading side effects");
        return;
    }
    auto wanted = Module::Status::InitReturned;
    if (name == L"missing") wanted = Module::Status::Missing;
    else if (name == L"hash-mismatch") wanted = Module::Status::ValidationFailed;
    else
    {
        state.forceDigest = true;
        if (name == L"hash-api-failure") { state.failHash = true; wanted = Module::Status::ValidationFailed; }
        else if (name == L"duplicate-name") { state.modules.push_back(L"MFGUnlocker.asi"); wanted = Module::Status::AlreadyLoaded; }
        else if (name == L"duplicate-alias") { state.modules.push_back(L"mFgUnLoCkEr-copy.ASI"); wanted = Module::Status::AlreadyLoaded; }
        else if (name == L"duplicate-marker") { state.modules.push_back(L"renamed-plugin.asi"); state.marker = true; wanted = Module::Status::AlreadyLoaded; }
        else if (name == L"snapshot-failure") { state.failSnapshot = true; wanted = Module::Status::InventoryFailed; }
        else if (name == L"enumeration-failure") { state.failFirst = true; wanted = Module::Status::InventoryFailed; }
        else if (name == L"enumeration-mid-failure")
        {
            // Two unrelated modules are observed, then enumeration fails before
            // a conflicting alias. An incomplete inventory must prevent mapping.
            state.modules.push_back(L"MFGUnlocker-unvisited.asi");
            state.failNext = true;
            wanted = Module::Status::InventoryFailed;
        }
        else if (name == L"load-failure") { state.failLoad = true; wanted = Module::Status::LoadFailed; }
        else if (name == L"entry-missing") { state.missingEntry = true; wanted = Module::Status::EntryMissing; }
        else Check(name == L"initialize-once" || name == L"concurrent", "known scenario");
    }
    if (name == L"concurrent") ConcurrentInitialize();
    else Module::Initialize();
    Terminal(wanted);
    Check(!state.badLoaderArguments, "all export and loader requests use the fixture contract");
    if (name == L"missing")
        Check(state.finishHashCalls == 0, "missing file never enters SHA256");
    else
        Check(state.finishHashCalls == 1 && state.realDigest == abcDigest, "real file IO and BCrypt SHA256 read abc");
    const bool mapped = wanted == Module::Status::InitReturned || wanted == Module::Status::EntryMissing ||
                        wanted == Module::Status::LoadFailed;
    Check(state.loadCalls == (mapped ? 1u : 0u), "rejected file or duplicate never reaches module loader");
    Check(state.entryLookups == ((mapped && !state.failLoad) ? 1u : 0u), "entry lookup follows mock mapping only");
    Check(state.initCalls == (wanted == Module::Status::InitReturned ? 1u : 0u), "only successful mock entry is called once");
    if (mapped)
        Check(state.writeWasBlocked && state.deleteWasBlocked, "production keeps input pinned against writes/deletion through mapping");
    if (name == L"hash-mismatch" || name == L"hash-api-failure")
        Check(state.snapshotCalls == 0 && state.resolveCalls == 0, "failed validation cannot inspect or query another module");
    if (wanted == Module::Status::InventoryFailed)
    {
        Check(state.snapshotCalls == 1 && state.loadCalls == 0 && state.entryLookups == 0 && state.initCalls == 0,
              "incomplete inventory never maps a module or queries/calls its entry");
        if (state.failSnapshot)
            Check(state.firstModuleCalls == 0 && state.nextModuleCalls == 0 && state.resolveCalls == 0,
                  "snapshot failure cannot enumerate invalid snapshot");
        else if (state.failFirst)
            Check(state.firstModuleCalls == 1 && state.nextModuleCalls == 0 && state.resolveCalls == 0,
                  "first-module failure cannot query modules or advance enumeration");
        else
            Check(state.firstModuleCalls == 1 && state.nextModuleCalls == 2 && state.nextModule == 2 &&
                  state.resolveCalls == 2 && state.nextModule < state.modules.size(),
                  "mid-enumeration failure leaves conflicting alias unvisited and blocks loading");
    }
}

void Spawn(const fs::path& executable, const wchar_t* name)
{
    std::wstring command = L"\"" + executable.wstring() + L"\" --case " + name;
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, executable.parent_path().c_str(), &startup, &process), "start fresh scenario process");
    Handle child(process.hProcess), thread(process.hThread);
    const auto wait = WaitForSingleObject(child.value, 30000);
    if (wait != WAIT_OBJECT_0)
    {
        // This handle is the child created above, never a game or user module.
        TerminateProcess(child.value, 2);
        WaitForSingleObject(child.value, timeoutMs);
        Check(false, "scenario process exceeded bounded timeout");
    }
    DWORD code = 1;
    Check(GetExitCodeProcess(child.value, &code) && code == 0, "fresh scenario process passed");
}
}

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

DWORD WINAPI ModuleFileName(HMODULE, LPWSTR output, DWORD capacity)
{
    ++state.fileNameCalls;
    const auto path = state.ownerPath.wstring();
    if (path.empty() || path.size() >= capacity) return 0;
    std::copy(path.begin(), path.end(), output);
    output[path.size()] = L'\0';
    return static_cast<DWORD>(path.size());
}

HANDLE WINAPI OpenFile(LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
                      DWORD creation, DWORD flags, HANDLE templateFile)
{
    ++state.fileOpenCalls;
    return ::CreateFileW(path, access, sharing, security, creation, flags, templateFile);
}

BOOL WINAPI ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped)
{
    ++state.fileReadCalls;
    return ::ReadFile(file, buffer, bytes, read, overlapped);
}

HANDLE WINAPI Snapshot(DWORD flags, DWORD process)
{
    ++state.snapshotCalls;
    if (flags != TH32CS_SNAPMODULE || process != GetCurrentProcessId()) state.badLoaderArguments = true;
    if (state.failSnapshot) { SetLastError(ERROR_PARTIAL_COPY); return INVALID_HANDLE_VALUE; }
    const auto handle = CreateEventW(nullptr, TRUE, FALSE, nullptr); // Real closable snapshot stand-in.
    return handle ? handle : INVALID_HANDLE_VALUE;
}

BOOL WINAPI FirstModule(HANDLE, LPMODULEENTRY32W entry)
{
    ++state.firstModuleCalls;
    state.nextModule = 0;
    if (state.failFirst || state.modules.empty()) { SetLastError(ERROR_BAD_LENGTH); return FALSE; }
    CopyModule(entry, state.nextModule++);
    return TRUE;
}

BOOL WINAPI NextModule(HANDLE, LPMODULEENTRY32W entry)
{
    ++state.nextModuleCalls;
    if (state.failNext && state.nextModule == 2) { SetLastError(ERROR_PARTIAL_COPY); return FALSE; }
    if (state.nextModule == state.modules.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    CopyModule(entry, state.nextModule++);
    return TRUE;
}

BOOL WINAPI ReadMemory(HANDLE process, LPCVOID address, LPVOID output, SIZE_T size, SIZE_T* read)
{
    ++state.memoryReadCalls;
    if (read) *read = 0;
    if (process != GetCurrentProcess() || !output || !read)
    {
        state.badMemoryArguments = true;
        return FALSE;
    }
    // Compute the allowed field addresses without reading the real PEB. All
    // bytes returned below belong to thread-local synthetic records only.
#if defined(_M_X64)
    const auto peb = static_cast<uintptr_t>(__readgsqword(0x60));
    const auto flagsAddress = reinterpret_cast<LPCVOID>(peb + 0x50);
    const auto slot = reinterpret_cast<LPCVOID>(peb + 0x110);
#else
    const LPCVOID flagsAddress = nullptr;
    const LPCVOID slot = nullptr;
#endif
    if (address == flagsAddress && size == sizeof(std::uint32_t))
    {
        ++state.processFlagsReadCalls;
        synthetic.Reset();
        synthetic.flagsRead = true;
        if (loaderProof == LoaderProof::ProcessFlagsReadFailure) return FALSE;
        const std::uint32_t flags = loaderProof == LoaderProof::ProcessInitializing ? 0x3u :
                                    loaderProof == LoaderProof::OtherProcessFlags ? 0xfffffffdu : 0u;
        std::memcpy(output, &flags, sizeof(flags));
        *read = loaderProof == LoaderProof::ProcessFlagsShortRead ? sizeof(flags) - 1 : sizeof(flags);
        return TRUE;
    }
    if (address == slot && size == sizeof(CRITICAL_SECTION*) && synthetic.flagsRead)
    {
        ++state.pebReadCalls;
        if (loaderProof == LoaderProof::PebReadFailure) return FALSE;
        auto* lock = loaderProof == LoaderProof::NullLock ? nullptr : &synthetic.lock;
        std::memcpy(output, &lock, size);
        *read = loaderProof == LoaderProof::PebShortRead ? size - 1 : size;
        return TRUE;
    }
    const bool lock = address == &synthetic.lock && size == sizeof(synthetic.lock) && synthetic.lockQueried;
    const bool debug = address == &synthetic.debug && size == sizeof(synthetic.debug) && synthetic.debugQueried;
    if (!lock && !debug)
    {
        state.badMemoryArguments = true;
        return FALSE;
    }
    if (lock) ++state.lockReadCalls;
    else ++state.debugReadCalls;
    if ((lock && loaderProof == LoaderProof::LockReadFailure) ||
        (debug && loaderProof == LoaderProof::DebugReadFailure)) return FALSE;
    std::memcpy(output, address, size);
    const bool shortRead = (lock && loaderProof == LoaderProof::LockShortRead) ||
                           (debug && loaderProof == LoaderProof::DebugShortRead);
    *read = shortRead ? size - 1 : size;
    return TRUE;
}

SIZE_T WINAPI QueryMemory(LPCVOID address, PMEMORY_BASIC_INFORMATION output, SIZE_T size)
{
    ++state.memoryQueryCalls;
    const bool lock = address == &synthetic.lock;
    const bool debug = address == &synthetic.debug;
    if ((!lock && !debug) || !output || size != sizeof(*output))
    {
        state.badMemoryArguments = true;
        return 0;
    }
    if (lock) synthetic.lockQueried = true;
    else synthetic.debugQueried = true;
    if ((lock && loaderProof == LoaderProof::LockQueryFailure) ||
        (debug && loaderProof == LoaderProof::DebugQueryFailure)) return 0;
    *output = {};
    output->BaseAddress = const_cast<void*>(address);
    output->AllocationBase = NtdllModule();
    output->RegionSize = lock ? sizeof(synthetic.lock) : sizeof(synthetic.debug);
    output->State = MEM_COMMIT;
    output->Type = MEM_IMAGE;
    output->Protect = loaderProof == LoaderProof::WriteCopy ? PAGE_WRITECOPY : PAGE_READWRITE;
    if (lock)
        switch (loaderProof)
        {
        case LoaderProof::LockUncommitted: output->State = MEM_RESERVE; break;
        case LoaderProof::LockPrivate: output->Type = MEM_PRIVATE; break;
        case LoaderProof::LockWrongModule: output->AllocationBase = LoadedModule(); break;
        case LoaderProof::LockReadOnly: output->Protect = PAGE_READONLY; break;
        case LoaderProof::LockExecutable: output->Protect = PAGE_EXECUTE_READWRITE; break;
        case LoaderProof::LockGuard: output->Protect |= PAGE_GUARD; break;
        case LoaderProof::LockNoAccess: output->Protect = PAGE_NOACCESS; break;
        case LoaderProof::LockShortRange: --output->RegionSize; break;
        case LoaderProof::LockOutsideRange:
            output->BaseAddress = reinterpret_cast<LPVOID>(reinterpret_cast<uintptr_t>(address) + 1);
            break;
        default: break;
        }
    else
        switch (loaderProof)
        {
        case LoaderProof::DebugWrongModule: output->AllocationBase = LoadedModule(); break;
        case LoaderProof::DebugShortRange: --output->RegionSize; break;
        default: break;
        }
    return sizeof(*output);
}

NTSTATUS WINAPI FinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR output, ULONG length, ULONG flags)
{
    ++state.finishHashCalls;
    const auto result = ::BCryptFinishHash(hash, output, length, flags);
    if (result < 0 || length != state.realDigest.size()) return result;
    std::copy_n(output, state.realDigest.size(), state.realDigest.begin());
    if (state.failHash) return static_cast<NTSTATUS>(-1);
    if (state.forceDigest) std::copy(controlledDigest.begin(), controlledDigest.end(), output);
    return result;
}

HMODULE MockLoadLibrary(LPCWSTR path, HANDLE file, DWORD flags)
{
    ++state.loadCalls;
    const auto input = state.ownerPath.parent_path() / L"plugins" / L"MFGUnlocker.asi";
    if (!path || fs::path(path) != input || file || flags) state.badLoaderArguments = true;
    const auto writer = CreateFileW(input.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    state.writeWasBlocked = writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    state.deleteWasBlocked = !DeleteFileW(input.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION;
    return state.failLoad ? nullptr : LoadedModule();
}

HMODULE MockNtdllModule()
{
    ++state.ntdllModuleCalls;
    return NtdllModule();
}

FARPROC WINAPI MockGetProcAddress(HMODULE module, LPCSTR name)
{
    ++state.resolveCalls;
    if (module == NtdllModule() && name && std::strcmp(name, "RtlIsThreadWithinLoaderCallout") == 0)
    {
        ++state.rtlLookups;
        return state.rtlAvailable ? std::bit_cast<FARPROC>(&MockLoaderCallout) : nullptr;
    }
    if (!name) { state.badLoaderArguments = true; return nullptr; }
    if (std::strcmp(name, "InitializeASI") == 0 && module == LoadedModule())
    {
        ++state.entryLookups;
        return state.missingEntry ? nullptr : std::bit_cast<FARPROC>(&MockInitialize);
    }
    if (std::strcmp(name, "MfgUnlockCoreLoaded") == 0)
        return state.marker && module == EnumeratedModule(2) ? std::bit_cast<FARPROC>(&MockInitialize) : nullptr;
    state.badLoaderArguments = true;
    return nullptr;
}

int Run(int argc, wchar_t** argv)
{
    const auto executable = ExecutablePath();
    OutputRoot(executable);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--case")
    {
        Case(executable, argv[2]);
        Check(!state.badMemoryArguments, "all synthetic loader memory operations obey the permitted ranges");
        std::wprintf(L"PrivateFgTestModule %ls: %u checks (%ls)\n", argv[2], checks,
                     state.forceDigest ? L"controlled BCrypt result; NOT matching-file SHA validation" :
                     state.finishHashCalls != 0 ? L"real SHA256/IO" : L"no file hashing or entry execution");
        return 0;
    }
    Check(argc == 1, "usage: PrivateFgTestModule.exe [--case <scenario>]");
    for (const auto name : scenarios) Spawn(executable, name.data());
    std::printf("PrivateFgTestModule: default build, provider paths, telemetry and %zu fresh-process scenarios passed; "
                "loader/export/RTL/memory-proof calls always mocked; real neutral Probe compiled; "
                "positive state cases use controlled BCrypt results.\n",
                std::size(scenarios));
    return 0;
}
}
