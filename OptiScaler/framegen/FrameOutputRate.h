#pragma once
#include <cstdint>

namespace framegen
{
// The provider supplies the number of outputs over each observed interval.
// This is not a scanout counter or a generated-frame identity.
struct OutputRate
{
    uintptr_t api = 0;
    uint32_t viewport = 0;
    uint64_t firstNs = 0, lastNs = 0, frames = 0, sequence = 0;
    double hz = 0;
    bool known = false;
};

class OutputRateWindow
{
    static constexpr uint64_t minWindowNs = 250000000ull;
    static constexpr uint64_t targetWindowNs = 1000000000ull;
    static constexpr uint64_t maxAgeNs = 3000000000ull;
    OutputRate accumulating_ {}, published_ {};
    uint64_t previousSequence_ = 0, previousEndNs_ = 0;
    uintptr_t previousApi_ = 0;
    uint32_t previousViewport_ = 0;

public:
    void Reset() noexcept { *this = {}; }
    void Observe(uintptr_t api, uint32_t viewport, uint64_t sequence, uint64_t previousSequence,
                 uint64_t beginNs, uint64_t endNs, uint32_t frames, bool valid) noexcept
    {
        if (!valid || !api || !sequence || !beginNs || endNs <= beginNs || endNs - beginNs > maxAgeNs)
        {
            Reset();
            return;
        }
        if (previousSequence_ && (api != previousApi_ || viewport != previousViewport_ ||
            previousSequence != previousSequence_ || sequence != previousSequence_ + 1 || beginNs != previousEndNs_))
            Reset();
        if (!accumulating_.firstNs)
        {
            accumulating_.api = api;
            accumulating_.viewport = viewport;
            accumulating_.firstNs = beginNs;
        }
        accumulating_.lastNs = endNs;
        accumulating_.sequence = sequence;
        accumulating_.frames += frames;
        const auto elapsed = endNs - accumulating_.firstNs;
        if (elapsed >= minWindowNs)
        {
            accumulating_.hz = static_cast<double>(accumulating_.frames) * 1e9 / static_cast<double>(elapsed);
            accumulating_.known = true;
            published_ = accumulating_;
        }
        if (elapsed >= targetWindowNs) accumulating_ = {};
        previousApi_ = api;
        previousViewport_ = viewport;
        previousSequence_ = sequence;
        previousEndNs_ = endNs;
    }
    OutputRate Read(uint64_t nowNs) const noexcept
    {
        auto result = published_;
        if (!result.lastNs || nowNs < result.lastNs || nowNs - result.lastNs > maxAgeNs) result.known = false;
        return result;
    }
};
}
