#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct TranslationFrameTimeSelection
{
    float dispatch_ms = 16.6667f;
    bool from_game = false;
};

// The caller owns the per-instance start counter. Eviction/end-of-dispatch
// timestamps must never overwrite it. Update even when the game supplies dt
// so legacy calls following a supplied frame still have a useful baseline.
inline TranslationFrameTimeSelection select_translation_frame_time(
    float source_ms, std::int64_t dispatch_start, std::int64_t &previous_start,
    std::int64_t frequency)
{
    TranslationFrameTimeSelection result;
    if (std::isfinite(source_ms) && source_ms >= 1.0f && source_ms <= 100.0f)
    {
        result.dispatch_ms = source_ms;
        result.from_game = true;
    }
    else if (previous_start > 0 && dispatch_start > previous_start && frequency > 0)
    {
        const double elapsed = (static_cast<double>(dispatch_start) -
            static_cast<double>(previous_start)) * 1000.0 / static_cast<double>(frequency);
        result.dispatch_ms = static_cast<float>(std::clamp(elapsed, 1.0, 100.0));
    }
    previous_start = dispatch_start;
    return result;
}
