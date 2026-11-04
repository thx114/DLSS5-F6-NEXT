#pragma once

#include "LogAsyncRule.h"
#include <Windows.h>

// Out-of-band drain-failure evidence. Initialized on the existing startup worker,
// never by the failure path. No spdlog, heap allocation, filesystem call, debug
// output, sink invocation, queue query, wait or retry in Record(). Mapped-page
// faults remain possible: this is best effort, not a hard real-time guarantee.
namespace logasync::evidence
{
enum class Context : uint32_t { Close = 1, Exit = 2, CrashBeforeDump = 3, CrashAfterDump = 4 };
inline constexpr DWORD kSlots = 64;
inline constexpr DWORD kBytes = 8192;
struct Record
{
    volatile LONG committed;
    DWORD status, context, mode, threadId, waitBudgetMs;
    DWORD sampleKnown, capacityCheckKnown;
    uint64_t sequence, tickMs, queueCapacity, sampledItems, sampledOverruns, sampleAgeMs, itemsAtCapacityCheck;
};
struct Header
{
    char magic[8];
    DWORD version, headerBytes, recordBytes, slots, processId, reserved;
    uint64_t processStartFileTime;
    volatile LONG attempts, omitted;
};
struct File { Header header; Record records[kSlots]; };
static_assert(sizeof(Header) == 48 && sizeof(Record) == 88 && sizeof(File) <= kBytes);

class Writer
{
  public:
    // Before publication, on a safe initialization thread only. File is already zeroed.
    static void PrepareFile(File& file) noexcept
    {
        constexpr char magic[8] = { 'O','P','T','D','R','N','1',0 };
        for (unsigned i = 0; i < 8; ++i) file.header.magic[i] = magic[i];
        file.header.version = 1; file.header.headerBytes = sizeof(Header);
        file.header.recordBytes = sizeof(Record); file.header.slots = kSlots;
        file.header.processId = GetCurrentProcessId();
        FILETIME start{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &start, &exit, &kernel, &user))
            file.header.processStartFileTime = (uint64_t(start.dwHighDateTime) << 32) | start.dwLowDateTime;
    }

    void Publish(File* file) noexcept { _file.store(file, std::memory_order_release); }

    void RecordFailure(const DrainResult& result, Context context, Mode mode, size_t capacity,
                       const QueueSample& sample, uint32_t waitBudgetMs) noexcept
    {
        if (result.status == DrainStatus::Complete) return;
        auto* file = _file.load(std::memory_order_acquire);
        if (file == nullptr) return; // initialization unavailable/too early: no emergency I/O fallback
        const LONG sequence = InterlockedIncrement(&file->header.attempts);
        if (_busy.test_and_set(std::memory_order_acquire))
        {
            InterlockedIncrement(&file->header.omitted);
            return;
        }
        if (_used == kSlots)
            InterlockedIncrement(&file->header.omitted);
        else
        {
            auto& record = file->records[_used++];
            record.status = static_cast<DWORD>(result.status);
            record.context = static_cast<DWORD>(context); record.mode = static_cast<DWORD>(mode);
            record.threadId = GetCurrentThreadId(); record.waitBudgetMs = waitBudgetMs;
            record.sampleKnown = sample.known;
            record.capacityCheckKnown = result.status == DrainStatus::QueueCapacity;
            record.sequence = static_cast<uint64_t>(sequence); record.tickMs = GetTickCount64();
            record.queueCapacity = capacity; record.sampledItems = sample.items;
            record.sampledOverruns = sample.overruns;
            const auto age = DiagnosticSteadyMs() - sample.steadyMs;
            record.sampleAgeMs = sample.known && age > 0 ? static_cast<uint64_t>(age) : 0;
            record.itemsAtCapacityCheck = result.itemsAtCapacityCheck;
            InterlockedExchange(&record.committed, 1); // readers ignore incomplete records
        }
        _busy.clear(std::memory_order_release);
    }

  private:
    std::atomic<File*> _file { nullptr };
    std::atomic_flag _busy = ATOMIC_FLAG_INIT;
    DWORD _used = 0; // under the nonwaiting try-guard
};
inline Writer writer;
inline std::atomic_flag initialized = ATOMIC_FLAG_INIT;

inline DWORD Initialize(const wchar_t* path) noexcept
{
    if (initialized.test_and_set(std::memory_order_acq_rel)) return ERROR_ALREADY_INITIALIZED;
    const auto file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, kBytes, nullptr);
    if (mapping == nullptr) { const auto error = GetLastError(); CloseHandle(file); return error; }
    auto* view = static_cast<File*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kBytes));
    if (view == nullptr) { const auto error = GetLastError(); CloseHandle(mapping); CloseHandle(file); return error; }
    ZeroMemory(view, kBytes); // touch/dirty all pages before publication
    Writer::PrepareFile(*view);
    writer.Publish(view);
    // Retain view and handles for OS process teardown, like the logger setups.
    // Never unmap under a crash/exit writer or wait/join under the loader lock.
    return ERROR_SUCCESS;
}

inline bool FlushAndObserve(Setup& setup, std::chrono::milliseconds timeout, Context context)
{
    DrainResult result;
    const bool complete = FlushAndDrain(setup, timeout, &result); // exceptions propagate unchanged
    if (!complete)
        writer.RecordFailure(result, context, setup.mode, setup.queueSize,
            setup.marker ? setup.marker->Sample() : QueueSample{}, static_cast<uint32_t>(timeout.count()));
    return complete;
}
} // namespace logasync::evidence
