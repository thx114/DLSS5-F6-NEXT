#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace framegen
{
enum class DlssgStateQueryReason : uint8_t { Provider, CapabilityCold, CompletionFence, Game, Other };

// CPU observation of ONE real, consumptive GetState invocation. No fence or
// resource pointer is cached here: this is never a resource-retirement receipt.
struct DlssgStateSample
{
    uintptr_t api = 0;
    uint32_t viewport = 0;
    uint64_t querySequence = 0;
    DlssgStateQueryReason reason = DlssgStateQueryReason::Other;
    uint64_t queryBeginNs = 0, queryEndNs = 0;
    uint64_t previousQuerySequence = 0, previousQueryBeginNs = 0, previousQueryEndNs = 0;
    uintptr_t previousApi = 0;
    uint32_t previousViewport = 0;
    int32_t result = -1;
    uint32_t stateVersion = 0, status = 0, actuallyPresented = 0;
    uint32_t maxGenerated = 0, minWidthOrHeight = 0;
    uint64_t estimatedVramBytes = 0;
    bool succeeded = false, dynamicSupported = false, vsyncSupported = false;
    // Counts retain the SDK's query-interval meaning, never request+1, output
    // identity, generated count, displayed FPS or a completion proof. A cold
    // first read, failed predecessor or changed API/viewport leaves it unknown.
    bool counterIntervalKnown = false;
};

// One instance for all actual SDK queries. Query lock is always the innermost
// lock: invoke must never obtain a provider/FG/continuity lock. Same-thread
// recursion fails before locking, rather than recursively consuming a counter.
// Readers use a separate lock, so menu/cache reads inside SDK callbacks cannot
// enter the SDK again or deadlock against this serialization gate.
class DlssgStateQueries final
{
    std::mutex queryMutex_, sampleMutex_;
    std::array<DlssgStateSample, 32> samples_ {};
    std::size_t next_ = 0;
    uint64_t sequence_ = 0;
    DlssgStateSample previous_ {};
    inline static thread_local bool querying_ = false;

    static uint64_t Now() noexcept
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void Publish(const DlssgStateSample& sample)
    {
        std::lock_guard sampleLock(sampleMutex_);
        for (auto& cached : samples_)
            if (cached.querySequence && cached.api == sample.api && cached.viewport == sample.viewport)
            {
                cached = sample;
                return;
            }
        samples_[next_] = sample;
        next_ = (next_ + 1) % samples_.size();
    }
public:
    template<class Invoke>
    bool SubmitOptions(Invoke&& invoke)
    {
        if (querying_) return false;
        std::lock_guard queryLock(queryMutex_);
        querying_ = true;
        struct Leave { ~Leave() { querying_ = false; } } leave;
        // SDK mode/options can reset counters. Preserve the preceding query's
        // coordinates, but do not claim an uninterrupted known interval.
        previous_.succeeded = false;
        std::forward<Invoke>(invoke)();
        return true;
    }

    template<class Invoke>
    bool Query(uintptr_t api, uint32_t viewport, DlssgStateQueryReason reason,
               DlssgStateSample& sample, Invoke&& invoke)
    {
        sample = {};
        if (!api || querying_) return false;
        std::lock_guard queryLock(queryMutex_);
        querying_ = true;
        struct Leave { ~Leave() { querying_ = false; } } leave;
        sample.api = api;
        sample.viewport = viewport;
        sample.reason = reason;
        sample.querySequence = ++sequence_;
        sample.previousQuerySequence = previous_.querySequence;
        sample.previousQueryBeginNs = previous_.queryBeginNs;
        sample.previousQueryEndNs = previous_.queryEndNs;
        sample.previousApi = previous_.api;
        sample.previousViewport = previous_.viewport;
        sample.queryBeginNs = Now();
        try { std::forward<Invoke>(invoke)(sample); }
        catch (...) {
            // A throwing SDK call might already have consumed the counter.
            // Never silently stretch the next interval back to an older read.
            sample.succeeded = false;
            sample.queryEndNs = Now();
            previous_ = sample;
            Publish(sample);
            throw;
        }
        sample.queryEndNs = Now();
        sample.counterIntervalKnown = sample.succeeded && previous_.succeeded &&
            previous_.api == api && previous_.viewport == viewport &&
            previous_.querySequence != 0 && sample.queryBeginNs >= previous_.queryEndNs;
        previous_ = sample;
        Publish(sample);
        return true;
    }

    bool Read(uintptr_t api, uint32_t viewport, DlssgStateSample& sample)
    {
        std::lock_guard lock(sampleMutex_);
        for (const auto& cached : samples_)
            if (cached.querySequence && cached.api == api && cached.viewport == viewport)
            {
                sample = cached;
                return true;
            }
        sample = {};
        return false;
    }
};
}
