// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <genshin/GenshinFsrUiObservation.h>
#include <cmath>
#include <cstdint>

namespace CriticalUi
{
enum class Notice : unsigned char { None, GenshinLoading, GenshinHooking, GenshinFsrNotDetected };

// A missing Render is not a game-settings reader. Both error causes use the
// same troubleshooting instructions and never assert an actual SMAA/scale setting.
class GenshinFsrHintPolicy
{
    GenshinLegacy::Ui::Session session_ {};
    GenshinLegacy::Ui::HookPhase phase_ = GenshinLegacy::Ui::HookPhase::Unknown;
    std::uint64_t attempt_ = 0, previousTick_ = 0, previousFrame_ = 0;
    std::uint64_t quietSince_ = 0, quietFrames_ = 0, lastCalls_ = 0;
    std::uint64_t candidateSince_ = 0;
    unsigned candidateSamples_ = 0;
    Notice candidate_ = Notice::None;

    void ClearCandidate() noexcept { candidate_ = Notice::None; candidateSince_ = 0; candidateSamples_ = 0; }
public:
    static constexpr std::uint64_t SampleIntervalMs = 500, FreshMs = 1500, MissingMs = 15000;
    static constexpr std::uint64_t ErrorDebounceMs = 1500, MinimumPresents = 120;

    void Reset() noexcept { *this = {}; }
    Notice Update(const GenshinLegacy::Ui::Observation& value, std::uint32_t currentPid,
                  std::uint64_t processStart, std::uint64_t now, std::uint64_t presentFrame,
                  std::uint32_t displayWidth, std::uint32_t displayHeight, bool currentRouteEnabled) noexcept
    {
        using GenshinLegacy::Ui::HookPhase;
        if (!value.session.Known() || !value.attempt || value.session.processId != currentPid ||
            value.session.processStart != processStart || !value.enabled || !currentRouteEnabled ||
            value.phase == HookPhase::Unknown || !value.observedTick || value.observedTick > now ||
            now - value.observedTick > FreshMs)
        { Reset(); return Notice::None; }

        const bool boundary = session_ != value.session || attempt_ != value.attempt || phase_ != value.phase ||
            now < previousTick_ || (previousTick_ && now - previousTick_ > FreshMs) ||
            presentFrame < previousFrame_ || value.render.calls < lastCalls_;
        if (boundary)
        {
            session_ = value.session; attempt_ = value.attempt; phase_ = value.phase;
            quietSince_ = now; quietFrames_ = 0; lastCalls_ = value.render.calls; ClearCandidate();
            previousFrame_ = presentFrame;
        }
        const auto frames = presentFrame - previousFrame_;
        previousFrame_ = presentFrame;
        previousTick_ = now;

        if (value.phase == HookPhase::Initializing)
        { ClearCandidate(); return Notice::GenshinHooking; }

        bool missing = value.phase == HookPhase::Failed;
        if (value.phase == HookPhase::Installed)
        {
            if (value.render.instanceCount > 1)
            { quietSince_ = now; quietFrames_ = 0; lastCalls_ = value.render.calls; ClearCandidate(); return Notice::None; }
            if (value.render.calls != lastCalls_ || !frames)
            { quietSince_ = now; quietFrames_ = 0; }
            else
                quietFrames_ += frames;
            lastCalls_ = value.render.calls;
            missing = now - quietSince_ >= MissingMs && quietFrames_ >= MinimumPresents;

            // Only one fresh instance whose display matches the current viewport
            // may supply a dimension-ratio observation. Ambiguous instances and
            // stale/unknown dimensions cannot diagnose the game's precision.
            const auto& r = value.render;
            if (r.instanceCount == 1 && r.generation && r.captureTick && r.captureTick <= now &&
                now - r.captureTick <= FreshMs && r.displayWidth == displayWidth &&
                r.displayHeight == displayHeight && r.renderWidth && r.renderHeight &&
                displayWidth && displayHeight)
            {
                const double x = double(r.renderWidth) / displayWidth;
                const double y = double(r.renderHeight) / displayHeight;
                if (x > 0.0 && x <= 1.0 && y > 0.0 && y <= 1.0 && std::abs(x - y) <= 0.015)
                    missing = std::abs(x - 0.6) > 0.015 || std::abs(y - 0.6) > 0.015;
            }
        }
        if (!missing) { ClearCandidate(); return Notice::None; }
        if (candidate_ != Notice::GenshinFsrNotDetected)
        { candidate_ = Notice::GenshinFsrNotDetected; candidateSince_ = now; candidateSamples_ = 1; }
        else
            ++candidateSamples_;
        return now - candidateSince_ >= ErrorDebounceMs && candidateSamples_ >= 3
            ? candidate_ : Notice::None;
    }
};
}
