#pragma once

// How the log reaches its sinks. A log line written on the game's thread must never wait for a disk: with the file on a
// slow (USB, spinning) drive a synchronous flush held a present for hundreds of ms. Header-only and free of Config, State
// and the game: a host test includes it. Logger.cpp builds the real logger from these pieces.

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace logasync
{
enum class Mode
{
    Sync,            // every line is written and flushed on the calling thread
    AsyncDropOldest, // a worker writes; when the queue is full the oldest lines are dropped (and counted)
    AsyncBlock       // a worker writes; when the queue is full the caller waits (an explicit LogAsync=true)
};

// 32k lines of about 400 bytes each is about 13 MB, minutes of room at 1,000 lines a second for a drive that hiccups
inline constexpr size_t kDropOldestQueueSize = 32768;
inline constexpr size_t kBlockQueueSize = 8192;

// `logAsync` is the ini value (nullopt = auto). Auto writes on a worker when logging to a file and stays synchronous
// otherwise (the console and the debugger are fast enough, and a crash then keeps every line).
inline Mode ChooseMode(std::optional<bool> logAsync, bool logToFile)
{
    if (logAsync.has_value())
        return *logAsync ? Mode::AsyncBlock : Mode::Sync;

    return logToFile ? Mode::AsyncDropOldest : Mode::Sync;
}

// Approximate periodic samples, never a claim about the backlog at crash time.
struct QueueSample
{
    bool known = false;
    size_t items = 0;
    size_t overruns = 0; // queue items, including control messages, not necessarily log lines
    int64_t steadyMs = 0;
};

inline int64_t DiagnosticSteadyMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Added as the LAST sink. FlushAndDrain posts a numbered marker line through the logger; this sink sees it after every
// sink before it handled it, and the flush that follows reaches it after theirs. This acknowledges a marker, not durable
// disk storage or no dropped messages. With multiple workers, other messages may still be processing in another sink.
class FlushMarkerSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    static constexpr const char* kPrefix = "Log flush ";

    // The last marker whose flush came through
    uint64_t Flushed() const
    {
        return _flushed.load(std::memory_order_acquire);
    }

    // Single try, no spinning/queue mutex/sink callback on a failure path.
    void SampleQueue(size_t items, size_t overruns) noexcept
    {
        if (_sampleBusy.test_and_set(std::memory_order_acquire)) return;
        _sample = { true, items, overruns, DiagnosticSteadyMs() };
        _sampleBusy.clear(std::memory_order_release);
    }

    QueueSample Sample() noexcept
    {
        if (_sampleBusy.test_and_set(std::memory_order_acquire)) return {};
        const auto sample = _sample;
        _sampleBusy.clear(std::memory_order_release);
        return sample;
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        const std::string_view text(msg.payload.data(), msg.payload.size());

        if (text.starts_with(kPrefix))
            _seen = std::strtoull(std::string(text.substr(std::strlen(kPrefix))).c_str(), nullptr, 10);
    }

    void flush_() override
    {
        _flushed.store(_seen, std::memory_order_release);
    }

  private:
    uint64_t _seen = 0; // under the sink's mutex
    std::atomic<uint64_t> _flushed { 0 };
    std::atomic_flag _sampleBusy = ATOMIC_FLAG_INIT;
    QueueSample _sample;
};
struct Setup
{
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<spdlog::details::thread_pool> pool; // empty for Sync
    std::shared_ptr<FlushMarkerSink> marker;            // empty for Sync
    Mode mode = Mode::Sync;
    size_t queueSize = 0;
};

// `pool` is only used for AsyncBlock (the caller's, shared with spdlog); the drop-oldest mode makes its own with one
// worker, so lines stay in order.
inline Setup MakeLogger(const std::string& name, std::vector<spdlog::sink_ptr> sinks, Mode mode,
                        std::shared_ptr<spdlog::details::thread_pool> pool = nullptr)
{
    Setup setup;
    setup.mode = mode;

    if (mode == Mode::Sync)
    {
        setup.logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
        return setup;
    }

    if (mode == Mode::AsyncDropOldest)
    {
        setup.queueSize = kDropOldestQueueSize;
        pool = std::make_shared<spdlog::details::thread_pool>(setup.queueSize, 1);
    }
    else
    {
        setup.queueSize = kBlockQueueSize;
    }

    setup.marker = std::make_shared<FlushMarkerSink>();
    sinks.push_back(setup.marker);
    setup.pool = pool;
    setup.logger = std::make_shared<spdlog::async_logger>(name, sinks.begin(), sinks.end(), pool,
                                                          mode == Mode::AsyncDropOldest
                                                              ? spdlog::async_overflow_policy::overrun_oldest
                                                              : spdlog::async_overflow_policy::block);
    return setup;
}

enum class DrainStatus : uint32_t { Complete = 0, QueueCapacity = 1, DeadlineElapsed = 2 };
struct DrainResult
{
    DrainStatus status = DrainStatus::Complete;
    size_t itemsAtCapacityCheck = 0;
};

// Marker acknowledgement only, not durable-disk or no-dropped-lines proof. False also
// means a block-policy capacity precheck refused enqueueing, not just a timeout.
// The existing timeout covers the marker wait, NOT synchronous sink I/O, queue mutex
// acquisition, or the block-policy enqueue TOCTOU race with other producers.
inline bool FlushAndDrain(Setup& setup, std::chrono::milliseconds timeout, DrainResult* result = nullptr)
{
    if (result != nullptr) *result = {};
    if (setup.logger == nullptr)
        return true;

    if (setup.mode == Mode::Sync || setup.pool == nullptr)
    {
        setup.logger->flush();
        return true;
    }

    // A flush request posted to a full queue under the block policy would wait for the worker: not on this path
    if (setup.mode == Mode::AsyncBlock)
    {
        const auto items = setup.pool->queue_size();
        if (items + 1 >= setup.queueSize)
        {
            if (result != nullptr) *result = { DrainStatus::QueueCapacity, items };
            return false;
        }
    }

    static std::atomic<uint64_t> lastToken { 0 };
    const uint64_t token = lastToken.fetch_add(1, std::memory_order_relaxed) + 1;

    // Level off passes any logger level; the line is the marker the sinks hand back
    setup.logger->log(spdlog::level::off, "{}{}", FlushMarkerSink::kPrefix, token);
    setup.logger->flush();

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (setup.marker->Flushed() != token)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            if (result != nullptr) result->status = DrainStatus::DeadlineElapsed;
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

// Lines dropped by the full queue since the last report, at most one report every `minIntervalMs` while the count rises
// (one line per burst, not per dropped line). `nowMs` is any monotonic clock in ms.
class DropReporter
{
  public:
    // Returns the number of lines dropped since the last report, 0 when there is nothing to say yet
    size_t Poll(size_t droppedTotal, double nowMs, double minIntervalMs = 5000.0)
    {
        if (droppedTotal <= _reported)
            return 0;

        if (_lastReportMs >= 0.0 && nowMs - _lastReportMs < minIntervalMs)
            return 0;

        const size_t fresh = droppedTotal - _reported;
        _reported = droppedTotal;
        _lastReportMs = nowMs;
        return fresh;
    }

    // A new logger starts its overrun count at 0: forget what the old one reported
    void Reset()
    {
        _reported = 0;
        _lastReportMs = -1.0;
    }

  private:
    size_t _reported = 0;
    double _lastReportMs = -1.0;
};

// A rebuilt logger must not join the old one's worker on the calling thread (destroying a thread pool waits for the
// worker to write the whole backlog, seconds on a stalled disk). The old setup is parked instead: its worker finishes the
// backlog on its own, and a thread that still holds the old logger (the log macros use a raw pointer) stays valid.
// A menu toggle is rare, so the idle worker and queue it keeps are not a cost worth a join.
inline void Retire(std::vector<Setup*>& retired, Setup* old)
{
    if (old != nullptr)
        retired.push_back(old);
}

// The process is about to exit (ExitProcess): the worker is still alive now and gone right after, so this is the last
// moment the queued lines can reach the disk. Only a worker-backed logger has anything to drain, once.
inline bool ShouldDrainAtExit(Mode mode, bool alreadyDrained)
{
    return !alreadyDrained && mode != Mode::Sync;
}

// The unhandled-exception filters of a process. Ours (`self`) stays the one Windows calls; the filter the game sets (before
// or after ours) is kept as `next` and called from ours once the log is drained, so a game's own crash handling still runs
// and its answer is honoured. Header-only and templated on the pointer type so a host test needs no windows.h.
template <class Filter> class FilterChain
{
  public:
    explicit FilterChain(Filter self) : _self(self) {}

    // What a game's SetUnhandledExceptionFilter(f) does once ours is installed: remember f, hand back the filter it
    // replaces (as the real API does), leave ours in place. Our own filter handed in again is not "the game's".
    Filter Replace(Filter f)
    {
        if (f == _self)
            return _next.load(std::memory_order_acquire);

        return _next.exchange(f, std::memory_order_acq_rel);
    }

    // The filter that was installed when ours went in (or took over again)
    void Adopt(Filter previous)
    {
        if (previous != _self)
            _next.store(previous, std::memory_order_release);
    }

    // The filter to call after ours, or null (never ours: it would recurse)
    Filter Next() const
    {
        const Filter next = _next.load(std::memory_order_acquire);
        return next == _self ? Filter {} : next;
    }

    // One crash handler per thread at a time: a crash inside ours skips straight to the next filter
    class Entry
    {
      public:
        Entry() : _first(!Active()) { Active() = true; }
        ~Entry()
        {
            if (_first)
                Active() = false;
        }
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

        bool First() const { return _first; }

      private:
        static bool& Active()
        {
            thread_local bool active = false;
            return active;
        }

        bool _first;
    };

  private:
    Filter _self;
    std::atomic<Filter> _next { Filter {} };
};
} // namespace logasync
