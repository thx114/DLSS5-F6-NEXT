#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// CPU provenance only. This neither owns GPU resources nor substitutes for their fences.
namespace dlssg {
struct InputOwner
{
    uintptr_t device = 0, queue = 0;
    uint64_t epoch = 0;
    uint32_t width = 0, height = 0, frames = 0, dynamicMode = 0, dynamicTarget = 0;
    uint32_t displayWidth = 0, displayHeight = 0, guideFlags = 0, flip = 0, depthScale = 0;
    uint32_t flipOffset = 0, depthScaleBits = 0;
    bool operator==(const InputOwner&) const = default;
};

template<size_t Slots> class InputContinuity
{
    struct Guides { uint64_t frame = 0, generation = 0; InputOwner owner {}; uint8_t mask = 0; };
    std::array<Guides, Slots> guides {};
    // CPU source clock only; never reset by Clear/physical GPU retirement.
    // UINT64_MAX is terminal poison, so no generation can wrap or recur.
    uint64_t sourceGeneration = 1;
    bool held = false;
    uint64_t holdFrame = 0;
    InputOwner holdOwner {};
  public:
    bool Held() const { return held; }
    uint64_t HoldFrame() const { return holdFrame; }
    uint64_t SourceGeneration() const { return sourceGeneration; }
    bool SourceClockValid() const { return sourceGeneration != UINT64_MAX; }
    bool RestartSource(uint64_t frame, const InputOwner& owner)
    {
        Clear(); // Old CPU guides/hold are invalid; no GPU resource is touched.
        if (sourceGeneration >= UINT64_MAX - 1)
        {
            sourceGeneration = UINT64_MAX;
            return false; // Saturating, permanent refusal; never ABA after Clear.
        }
        ++sourceGeneration;
        Hold(frame, owner); // Only a subsequent real source may recover/reset.
        return true;
    }
    void Observe(size_t slot, uint64_t frame, const InputOwner& owner, bool depth, uint64_t generation)
    {
        if (!SourceClockValid() || generation != sourceGeneration) return;
        auto& g = guides.at(slot);
        if (g.frame != frame || g.generation != generation || !(g.owner == owner))
            g = {frame, generation, owner, 0};
        g.mask |= depth ? 1 : 2;
    }
    void Hold(uint64_t frame, const InputOwner& owner) { held = true; holdFrame = frame; holdOwner = owner; }
    bool OwnerMatches(const InputOwner& owner) const { return held && holdOwner == owner; }
    // Same-current-frame tag receipt, never GPU completion or a new frame ID.
    bool Current(size_t slot, uint64_t frame, const InputOwner& owner) const
    {
        const auto& g = guides.at(slot);
        return SourceClockValid() && g.generation == sourceGeneration &&
               g.frame == frame && g.owner == owner && g.mask == 3;
    }
    bool Fresh(size_t slot, uint64_t frame, uint64_t dispatched, const InputOwner& owner,
               bool dispatchedInGeneration = true) const
    {
        return held && frame > holdFrame && (!dispatchedInGeneration || frame != dispatched) &&
               holdOwner == owner && Current(slot, frame, owner);
    }
    void Resume() { held = false; }
    void Clear() { held = false; holdFrame = 0; holdOwner = {}; guides = {}; }
};
}
