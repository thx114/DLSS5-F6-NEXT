#pragma once
#include <Windows.h>
#include <cstdint>

// Passive AV evidence only. No stack walk: even a bounded unwind may acquire loader locks.
// The on-disk format is fixed-width little endian, version 1; no pointers are dereferenced
// other than the OS-supplied exception structures and the pre-created private mapped view.
namespace startupav
{
enum class Phase : LONG64
{
    Unknown = 0, WorkerEntry = 1, EvidenceReady = 2,
    InputEnter = 10, InputLockAcquired = 11, InputTargetIdentity = 12, InputOtherIdentity = 13,
    InputStateChecked = 14, InputSetTarget = 15, InputSetOther = 16, InputInstallHooks = 17,
    InputHooksReturned = 18, InputOptionalIntegrations = 19, InputReturned = 20
};
enum class Kind : DWORD { ScopedSehSearchObserved = 1, UnhandledFilterEntry = 2 };
constexpr LONG kPerKind = 8;
constexpr DWORD kFileBytes = 4096;
struct Record
{
    volatile LONG committed; DWORD kind; DWORD threadId; DWORD code;
    DWORD exceptionFlags; DWORD parameterCount;
    std::uint64_t instruction; std::uint64_t contextInstruction;
    std::uint64_t optOffset; std::uint64_t accessType; std::uint64_t accessAddress;
    std::uint64_t phaseAndThread;
};
struct Header
{
    char magic[8]; DWORD version; DWORD headerBytes; DWORD recordBytes; DWORD recordsPerKind;
    DWORD processId; DWORD peTimestamp; std::uint64_t processStartFileTime;
    std::uint64_t optBase; std::uint64_t optSize;
    // Stage setters update private state only; this header is immutable after initialization.
    DWORD noStackWalk; DWORD reserved;
};
struct File
{
    Header header;
    Record scopedSearch[kPerKind];
    Record unhandledEntry[kPerKind];
};
static_assert(sizeof(File) <= kFileBytes);
static_assert(sizeof(Record) == 72 && sizeof(Header) == 64);

inline File* g_view = nullptr;
inline HANDLE g_file = INVALID_HANDLE_VALUE;
inline HANDLE g_mapping = nullptr;
inline volatile LONG g_enabled = 0;
inline volatile LONG g_busy = 0;
inline volatile LONG g_scopedCount = 0;
inline volatile LONG g_unhandledCount = 0;
alignas(8) inline volatile LONG64 g_phaseAndThread = 0;
inline volatile LONG g_initialization = 0;

inline void SetPhase(Phase phase) noexcept
{
    const auto value = (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32) |
                       static_cast<DWORD>(phase);
    InterlockedExchange64(&g_phaseAndThread, static_cast<LONG64>(value));
}

inline void Observe(EXCEPTION_POINTERS* info, Kind kind) noexcept
{
    // A global try-guard drops nested/concurrent samples. Never spin or wait on a crashed thread.
    if (InterlockedCompareExchange(&g_enabled, 0, 0) != 1 || info == nullptr ||
        info->ExceptionRecord == nullptr || info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return;

    // Shutdown disables first; recheck after taking the guard before touching a possibly unmapped view.
    if (InterlockedCompareExchange(&g_enabled, 0, 0) != 1)
    {
        InterlockedExchange(&g_busy, 0);
        return;
    }
    auto* view = g_view;
    volatile LONG* count = kind == Kind::ScopedSehSearchObserved ? &g_scopedCount : &g_unhandledCount;
    const LONG index = *count;
    if (view != nullptr && index < kPerKind)
    {
        *count = index + 1; // serialized by g_busy; separate budgets preserve UEF evidence
        auto& record = kind == Kind::ScopedSehSearchObserved ? view->scopedSearch[index] : view->unhandledEntry[index];
        const auto* exception = info->ExceptionRecord;
        record.kind = static_cast<DWORD>(kind);
        record.threadId = GetCurrentThreadId();
        record.code = exception->ExceptionCode;
        record.exceptionFlags = exception->ExceptionFlags;
        record.parameterCount = exception->NumberParameters;
        record.instruction = reinterpret_cast<std::uint64_t>(exception->ExceptionAddress);
#if defined(_M_X64)
        record.contextInstruction = info->ContextRecord != nullptr ? info->ContextRecord->Rip : 0;
#else
        record.contextInstruction = 0;
#endif
        record.optOffset = record.instruction >= view->header.optBase &&
                           record.instruction - view->header.optBase < view->header.optSize
                               ? record.instruction - view->header.optBase : UINT64_MAX;
        record.accessType = exception->NumberParameters >= 2 ? exception->ExceptionInformation[0] : UINT64_MAX;
        record.accessAddress = exception->NumberParameters >= 2 ? exception->ExceptionInformation[1] : 0;
        record.phaseAndThread = static_cast<std::uint64_t>(InterlockedCompareExchange64(&g_phaseAndThread, 0, 0));
        // Readers accept only committed records. No FlushViewOfFile/WriteFile in either exception entry.
        InterlockedExchange(&record.committed, 1);
    }
    InterlockedExchange(&g_busy, 0);
}

// This SEH search filter is reached only when inner handlers did not already handle
// the exception on this call stack. It is not a global first-chance or fatal signal.
inline LONG ScopedSearchFilter(EXCEPTION_POINTERS* info) noexcept
{
    Observe(info, Kind::ScopedSehSearchObserved);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Implemented by Logger.cpp using the already configured log directory.
void Prepare(HMODULE optModule) noexcept;

// Call once on the existing post-DllMain worker. Caller provides an absolute, PID-specific
// path. All file/mapping/PE/loader operations occur here, never in an exception callback.
inline DWORD Initialize(const wchar_t* path, HMODULE optModule) noexcept
{
    if (InterlockedCompareExchange(&g_initialization, 1, 0) != 0)
        return ERROR_ALREADY_INITIALIZED;
    // Serialize setup against nonwaiting cleanup; stopped (-1) can never be re-enabled.
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return ERROR_BUSY;
    struct InitGuard { ~InitGuard() { InterlockedExchange(&g_busy, 0); } } guard;
    if (InterlockedCompareExchange(&g_enabled, 0, 0) == -1)
        return ERROR_OPERATION_ABORTED;
    g_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE)
        return GetLastError();
    g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READWRITE, 0, kFileBytes, nullptr);
    if (g_mapping == nullptr)
    {
        const DWORD error = GetLastError(); CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; return error;
    }
    g_view = static_cast<File*>(MapViewOfFile(g_mapping, FILE_MAP_WRITE, 0, 0, kFileBytes));
    if (g_view == nullptr)
    {
        const DWORD error = GetLastError(); CloseHandle(g_mapping); CloseHandle(g_file);
        g_mapping = nullptr; g_file = INVALID_HANDLE_VALUE; return error;
    }
    // Zero/dirty every page during safe init, reducing (not eliminating) later mapped-page faults.
    ZeroMemory(g_view, kFileBytes);
    auto& header = g_view->header;
    const char magic[8] = { 'O','P','T','A','V','0','1',0 };
    for (unsigned i = 0; i < 8; ++i) header.magic[i] = magic[i];
    header.version = 1; header.headerBytes = sizeof(Header); header.recordBytes = sizeof(Record);
    header.recordsPerKind = kPerKind; header.processId = GetCurrentProcessId(); header.noStackWalk = 1;
    FILETIME start {}, exit {}, kernel {}, user {};
    if (GetProcessTimes(GetCurrentProcess(), &start, &exit, &kernel, &user))
        header.processStartFileTime = (static_cast<std::uint64_t>(start.dwHighDateTime) << 32) | start.dwLowDateTime;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(optModule);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(optModule) + dos->e_lfanew);
    header.optBase = reinterpret_cast<std::uint64_t>(optModule);
    header.optSize = nt->OptionalHeader.SizeOfImage; header.peTimestamp = nt->FileHeader.TimeDateStamp;
    SetPhase(Phase::EvidenceReady);
    InterlockedCompareExchange(&g_enabled, 1, 0);
    return ERROR_SUCCESS;
}

// Idempotent nonwaiting disable. No registered callback or module reference exists.
// If an observation is already writing, retain its view/handles for OS teardown rather
// than race unmapping it or wait under the loader lock. An observer delayed before the
// guard rechecks enabled and never reads a retired view.
inline void Shutdown() noexcept
{
    InterlockedExchange(&g_enabled, -1);
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return;
    if (g_view != nullptr) { UnmapViewOfFile(g_view); g_view = nullptr; }
    if (g_mapping != nullptr) { CloseHandle(g_mapping); g_mapping = nullptr; }
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
    InterlockedExchange(&g_busy, 0);
}
} // namespace startupav
