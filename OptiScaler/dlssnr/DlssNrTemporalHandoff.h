#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace DlssNrTemporalDetail
{
// Two independent full-model feedback cohorts. Each slot is replaced every L,
// staggered by L/2; only their display is blended. A displayed blend must never
// be captured into either slot, or the older cohort could acquire a new birth.
class Handoff
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point epoch {};
    std::array<uint64_t, 2> captured { ~0ull, ~0ull }, planned {};

public:
    struct Plan { uint32_t resetMask = 3, incoming = 0; float blend = 1.0f; };

    Plan Next(Clock::time_point now, uint32_t lifetimeMs, bool usable)
    {
        if (!usable) Reset();
        Plan p;
        if (epoch.time_since_epoch().count() == 0)
        {
            planned = { 0, 0 };
            return p;
        }
        const double periodMs = (std::max)(1u, lifetimeMs) * 0.5;
        const double age = (std::max)(0.0,
            std::chrono::duration<double, std::milli>(now - epoch).count());
        const uint64_t step = static_cast<uint64_t>(age / periodMs);
        p.incoming = static_cast<uint32_t>(step & 1ull);
        planned[p.incoming] = step;
        planned[p.incoming ^ 1u] = step ? step - 1 : 0;
        p.resetMask = 0;
        for (unsigned i = 0; i < 2; ++i)
            if (captured[i] != planned[i]) p.resetMask |= 1u << i;
        // At a boundary the previous incoming cohort has weight one, while
        // the expired one is reseeded with weight zero. Skipped boundaries
        // reseed every mismatched generation; no catch-up replay or old read.
        const float phase = static_cast<float>(std::clamp((age - step * periodMs) / periodMs, 0.0, 1.0));
        p.blend = step ? phase * phase * (3.0f - 2.0f * phase) : 1.0f;
        return p;
    }

    // Recording success only. Actual queue publication and GPU retirement
    // remain the backend's responsibility; discarded recordings reset us.
    void Captured(Clock::time_point now)
    {
        if (epoch.time_since_epoch().count() == 0) epoch = now;
        captured = planned;
    }
    void Reset() { *this = {}; }
};
}
