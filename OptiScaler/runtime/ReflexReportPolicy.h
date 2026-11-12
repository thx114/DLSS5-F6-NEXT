// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// Monitor policy only. Completion comes from GetLatency's ordered report array;
// neither a report nor a matched timestamp proves queue ownership or retirement.
namespace ReflexReportPolicy
{
inline bool ValidInterval(uint64_t begin, uint64_t end) noexcept
{
    return begin != 0 && end != 0 && begin != UINT64_MAX && end != UINT64_MAX && end >= begin;
}

struct Interval { uint64_t begin, end; };
struct Range { uint64_t begin, end; };

template<class Report>
std::optional<Range> RangeFor(const Report& report) noexcept
{
    if (!report.frameID) return {};
    // Read named fields rather than walking across distinct struct members.
    const std::array<Interval, 6> intervals {{
        {report.simStartTime, report.simEndTime},
        {report.renderSubmitStartTime, report.renderSubmitEndTime},
        {report.presentStartTime, report.presentEndTime},
        {report.driverStartTime, report.driverEndTime},
        {report.osRenderQueueStartTime, report.osRenderQueueEndTime},
        {report.gpuRenderStartTime, report.gpuRenderEndTime}
    }};
    Range range {UINT64_MAX, 0};
    for (const auto& interval : intervals)
    {
        if (!ValidInterval(interval.begin, interval.end)) continue;
        if (interval.begin < range.begin) range.begin = interval.begin;
        if (interval.end > range.end) range.end = interval.end;
    }
    return range.end > range.begin ? std::optional<Range>{range} : std::nullopt;
}

struct Selection
{
    size_t index, newestIndex;
    uint64_t newestFrame;
    Range range;
};

template<class Report, size_t Count>
std::optional<Selection> LatestCompleted(const Report (&reports)[Count]) noexcept
{
    static_assert(Count > 0);
    // NVAPI returns older reports first, newest completed report last. Frame IDs
    // are marker identities, not sortable global ordinals across devices/resets.
    // Optional GPU enrichment deliberately has no input into this decision.
    for (size_t i = Count; i-- > 0;)
        if (const auto range = RangeFor(reports[i]))
            return Selection {i, Count - 1, reports[Count - 1].frameID, *range};
    return {};
}

class Freshness
{
    uint64_t frame_ = 0, begin_ = 0, end_ = 0, changedAt_ = 0;
    uintptr_t device_ = 0;
    bool estimated_ = false, seen_ = false;
public:
    bool Observe(uint64_t frame, Range range, uintptr_t device, bool estimated, uint64_t now) noexcept
    {
        if (!seen_ || now < changedAt_ || frame != frame_ || range.begin != begin_ || range.end != end_ ||
            device != device_ || estimated != estimated_)
        {
            frame_ = frame; begin_ = range.begin; end_ = range.end; device_ = device;
            estimated_ = estimated; changedAt_ = now; seen_ = true;
        }
        return UnchangedMs(now) <= 2000;
    }
    // Time since this selected report changed, NOT the driver's sample age.
    uint64_t UnchangedMs(uint64_t now) const noexcept { return now >= changedAt_ ? now - changedAt_ : 0; }
};

} // namespace ReflexReportPolicy
