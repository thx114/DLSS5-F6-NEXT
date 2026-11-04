#pragma once

#include <Windows.h>
#include <cmath>
#include <cstddef>

// Measures accepted output Present-entry call cadence, before the real Present result is known.
// This is not displayed-frame or monitor timing. The overlay calls Record exactly once per eligible entry.
namespace MenuPhysicalTiming
{
namespace Detail
{
constexpr std::size_t WindowSize = 8;
constexpr double StaleMs = 2000.0;

struct History
{
    const void* Swapchain = nullptr;
    const void* Queue = nullptr;
    LONGLONG Last = 0;
    LONGLONG Frequency = 0;
    double Intervals[WindowSize] {};
    double Sum = 0.0;
    std::size_t Count = 0;
    std::size_t Next = 0;
    bool InPresent = false;
    bool ReadEnabled = false;
};

inline thread_local History history;

inline void Reset(const void* swapchain, const void* queue, LONGLONG now)
{
    auto& h = history;
    h.Swapchain = swapchain;
    h.Queue = queue;
    h.Last = now;
    h.Sum = 0.0;
    h.Count = 0;
    h.Next = 0;
}

inline bool RecordSample(const void* swapchain, const void* queue)
{
    auto& h = history;
    LARGE_INTEGER counter {}, frequency {};
    if (swapchain == nullptr || queue == nullptr || !QueryPerformanceCounter(&counter) || counter.QuadPart <= 0)
    {
        Reset(nullptr, nullptr, 0);
        return false;
    }
    if (h.Frequency <= 0)
    {
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        {
            Reset(nullptr, nullptr, 0);
            return false;
        }
        h.Frequency = frequency.QuadPart;
    }
    if (h.Swapchain != swapchain || h.Queue != queue || h.Last <= 0 || counter.QuadPart < h.Last)
    {
        Reset(swapchain, queue, counter.QuadPart);
        return false;
    }
    if (counter.QuadPart == h.Last)
        return h.Count > 0; // A duplicate timestamp adds no interval and does not move the baseline.

    const double interval = static_cast<double>(counter.QuadPart - h.Last) / h.Frequency * 1000.0;
    if (!std::isfinite(interval) || interval <= 0.0 || interval > StaleMs)
    {
        Reset(swapchain, queue, counter.QuadPart);
        return false;
    }
    h.Last = counter.QuadPart;
    if (h.Count == WindowSize)
        h.Sum -= h.Intervals[h.Next];
    else
        ++h.Count;
    h.Intervals[h.Next] = interval;
    h.Sum += interval;
    h.Next = (h.Next + 1) % WindowSize;
    return true;
}
} // namespace Detail

// Keep the returned token alive through the complete Present call, including
// menu rendering, replay and every early return after the overlay access is
// acquired. Scoping read access prevents later generic/legacy menu calls on
// this thread from reusing the route.
class PresentScope
{
  public:
    PresentScope(bool enabled, const void* swapchain, const void* queue)
        : previousInPresent(Detail::history.InPresent), previousReadEnabled(Detail::history.ReadEnabled)
    {
        auto& h = Detail::history;
        h.InPresent = true;
        h.ReadEnabled = !previousInPresent && enabled && Detail::RecordSample(swapchain, queue);
    }

    ~PresentScope()
    {
        Detail::history.InPresent = previousInPresent;
        Detail::history.ReadEnabled = previousReadEnabled;
    }

    PresentScope(const PresentScope&) = delete;
    PresentScope& operator=(const PresentScope&) = delete;

  private:
    bool previousInPresent;
    bool previousReadEnabled;
};

[[nodiscard]] inline PresentScope Record(bool enabled, const void* swapchain, const void* queue)
{
    return PresentScope(enabled, swapchain, queue);
}

inline bool Read(double& intervalMs)
{
    auto& h = Detail::history;
    if (!h.ReadEnabled || h.Count == 0 || h.Frequency <= 0)
        return false;
    LARGE_INTEGER now {};
    if (!QueryPerformanceCounter(&now) || now.QuadPart < h.Last)
        return false;
    const double age = static_cast<double>(now.QuadPart - h.Last) / h.Frequency * 1000.0;
    const double mean = h.Sum / h.Count;
    if (!std::isfinite(age) || age > Detail::StaleMs || !std::isfinite(mean) || mean <= 0.0)
        return false;
    intervalMs = mean;
    return true;
}
} // namespace MenuPhysicalTiming
