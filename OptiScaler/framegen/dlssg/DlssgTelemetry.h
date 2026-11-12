#pragma once

#include <atomic>

namespace framegen::DlssgTelemetry
{
struct Counters
{
    std::atomic_bool optionsSeen { false };
    std::atomic_bool active { false };
    std::atomic_uint requested { 1 };
    std::atomic_uint sent { 1 };
    std::atomic_uint result { 0 };
    std::atomic_bool stateSeen { false };
    std::atomic_uint presented { 0 };
    std::atomic_uint maxPresented { 0 };
};

inline Counters counters;
inline const Counters& Get() { return counters; }

inline void RecordSetOptions(unsigned requested, unsigned sent, bool active, unsigned result)
{
    counters.requested.store(requested, std::memory_order_relaxed);
    counters.sent.store(sent, std::memory_order_relaxed);
    counters.result.store(result, std::memory_order_relaxed);
    counters.active.store(active, std::memory_order_relaxed);
    counters.optionsSeen.store(true, std::memory_order_release);
}

// Presented counts cover the interval consumed by GetState, not one source frame.
inline void RecordState(unsigned presented)
{
    counters.presented.store(presented, std::memory_order_relaxed);
    auto maximum = counters.maxPresented.load(std::memory_order_relaxed);
    while (presented > maximum &&
           !counters.maxPresented.compare_exchange_weak(maximum, presented, std::memory_order_relaxed)) {}
    counters.stateSeen.store(true, std::memory_order_release);
}
}
