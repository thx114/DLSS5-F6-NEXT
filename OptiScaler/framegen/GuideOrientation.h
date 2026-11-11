#pragma once

#include <cstdint>
#include <optional>

namespace framegen
{
// Texture row order and vector component convention are independent. This
// describes only FG inputs; NR has its own model-coordinate contract.
struct GuideOrientation
{
    bool depthFlipY = false;
    bool motionFlipY = false;
    bool motionYInverted = false;
    bool flipOffset = false;

    constexpr bool DepthCopy() const { return depthFlipY; }
    constexpr bool MotionCopy() const { return motionFlipY || motionYInverted; }
    constexpr uint32_t Key() const
    {
        return uint32_t(depthFlipY) | (uint32_t(motionFlipY) << 1) | (uint32_t(motionYInverted) << 2);
    }
    constexpr bool UsesOffset() const { return flipOffset && (depthFlipY || motionFlipY); }
};

constexpr GuideOrientation ResolveGuideOrientation(bool legacyFlip, std::optional<bool> depthFlipY,
    std::optional<bool> motionFlipY, std::optional<bool> motionYInverted, bool offset)
{
    // Missing/auto overrides retain each part of the old ResourceFlip behavior.
    // Explicit false must survive config save/load even if the legacy switch is on.
    return {depthFlipY.value_or(legacyFlip), motionFlipY.value_or(legacyFlip),
            motionYInverted.value_or(legacyFlip), offset};
}

template<class Config> GuideOrientation ResolveGuideOrientation(const Config& config)
{
    return ResolveGuideOrientation(config.FGResourceFlip.value_or_default(), config.FGDepthTextureFlipY,
        config.FGMotionTextureFlipY, config.FGMotionYInverted, config.FGResourceFlipOffset.value_or_default());
}
}
