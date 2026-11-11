#pragma once
#include <Config.h>
#include "ModelScalarBounds.h"
#include <algorithm>
#include <cmath>

namespace DlssNr::Profiles
{
enum class ProfileSource : unsigned char
{
    Default,
    InheritedPass1,
    PassOverride,
};

inline const char* SourceName(ProfileSource source)
{
    switch (source)
    {
    case ProfileSource::PassOverride: return "PassOverride";
    case ProfileSource::InheritedPass1: return "InheritedPass1";
    default: return "Default";
    }
}

struct NrPassTuning
{
    float intensity = 1.0f;
    float structure = 1.0f;
    float tone = 1.0f;
    float skin = 1.0f;
    bool autoMask = true;
    bool operator==(const NrPassTuning&) const = default;
};

struct EffectivePassProfile
{
    unsigned int pass = 0;
    unsigned int preset = 0;
    unsigned int style = 0;
    NrPassTuning tuning {};
    ProfileSource source = ProfileSource::Default;
};

inline EffectivePassProfile EffectivePassProfileFor(const Config& cfg, unsigned int pass)
{
    EffectivePassProfile result {};
    result.pass = pass;
    result.preset = std::min(cfg.DlssNrPreset.value_or_default(), 3u);
    result.style = std::min(cfg.DlssNrStyle.value_or_default(), 2u);
    result.tuning = { cfg.DlssNrIntensity.value_or_default(),
                      cfg.DlssNrLocalStructure.value_or_default(),
                      cfg.DlssNrLocalTone.value_or_default(),
                      cfg.DlssNrSkinStructure.value_or_default(),
                      cfg.DlssNrAutoMask.value_or_default() };

    bool overridden = false;

    const auto apply = [&](const auto& preset, const auto& style, const auto& intensity, const auto& structure,
                            const auto& tone, const auto& skin, const auto& autoMask) {
        if (preset.has_value())
        {
            result.preset = std::min(preset.value(), 3u);
            overridden = true;
        }
        if (style.has_value())
        {
            result.style = std::min(style.value(), 2u);
            overridden = true;
        }
        if (intensity.has_value())
        {
            result.tuning.intensity = intensity.value();
            overridden = true;
        }
        if (structure.has_value())
        {
            result.tuning.structure = structure.value();
            overridden = true;
        }
        if (tone.has_value())
        {
            result.tuning.tone = tone.value();
            overridden = true;
        }
        if (skin.has_value())
        {
            result.tuning.skin = skin.value();
            overridden = true;
        }
        if (autoMask.has_value())
        {
            result.tuning.autoMask = autoMask.value();
            overridden = true;
        }
    };

    if (pass == 1)
        apply(cfg.DlssNrPass2Preset, cfg.DlssNrPass2Style, cfg.DlssNrPass2Intensity,
              cfg.DlssNrPass2LocalStructure, cfg.DlssNrPass2LocalTone, cfg.DlssNrPass2SkinStructure,
              cfg.DlssNrPass2AutoMask);
    else if (pass == 2)
        apply(cfg.DlssNrPass3Preset, cfg.DlssNrPass3Style, cfg.DlssNrPass3Intensity,
              cfg.DlssNrPass3LocalStructure, cfg.DlssNrPass3LocalTone, cfg.DlssNrPass3SkinStructure,
              cfg.DlssNrPass3AutoMask);
    else if (pass >= 3 && pass < DlssNr::MaxPassCount)
    {
        const auto& extra = cfg.DlssNrExtraPasses[pass - 3];
        apply(extra.preset, extra.style, extra.intensity, extra.structure, extra.tone, extra.skin, extra.autoMask);
    }

    // Resolve inheritance and this pass's preset before enforcing its scalar contract.
    result.tuning.intensity = BoundedScalar(result.tuning.intensity, result.preset);
    result.tuning.structure = BoundedScalar(result.tuning.structure, result.preset);
    result.tuning.tone = BoundedScalar(result.tuning.tone, result.preset);
    result.tuning.skin = BoundedScalar(result.tuning.skin, result.preset, -1.0f);
    result.source = pass == 0 ? ProfileSource::Default
                              : overridden ? ProfileSource::PassOverride : ProfileSource::InheritedPass1;
    return result;
}

// Only explicit scalars are clamped. Absent overrides continue to inherit pass 1.
// The effective profile independently enforces this policy even with the menu closed.
inline void ClampConfiguredModelScalars(Config& cfg)
{
    const auto clamp = [](auto& option, unsigned int preset, float minimum = 0.0f) {
        if (option.has_value())
        {
            const float bounded = BoundedScalar(option.value(), preset, minimum);
            if (option.value() != bounded)
                option = bounded;
        }
    };
    clamp(cfg.DlssNrIntensity, cfg.DlssNrPreset.value_or_default());
    clamp(cfg.DlssNrLocalStructure, cfg.DlssNrPreset.value_or_default());
    clamp(cfg.DlssNrLocalTone, cfg.DlssNrPreset.value_or_default());
    clamp(cfg.DlssNrSkinStructure, cfg.DlssNrPreset.value_or_default(), -1.0f);
    const auto later = [&](unsigned int pass, auto& intensity, auto& structure, auto& tone, auto& skin) {
        const auto preset = EffectivePassProfileFor(cfg, pass).preset;
        clamp(intensity, preset); clamp(structure, preset); clamp(tone, preset); clamp(skin, preset, -1.0f);
    };
    later(1, cfg.DlssNrPass2Intensity, cfg.DlssNrPass2LocalStructure, cfg.DlssNrPass2LocalTone,
          cfg.DlssNrPass2SkinStructure);
    later(2, cfg.DlssNrPass3Intensity, cfg.DlssNrPass3LocalStructure, cfg.DlssNrPass3LocalTone,
          cfg.DlssNrPass3SkinStructure);
    for (unsigned int pass = 3; pass < DlssNr::MaxPassCount; ++pass)
    {
        auto& extra = cfg.DlssNrExtraPasses[pass - 3];
        later(pass, extra.intensity, extra.structure, extra.tone, extra.skin);
    }
}

inline EffectivePassProfile EffectivePassProfile(const Config& cfg, unsigned int pass)
{
    return EffectivePassProfileFor(cfg, pass);
}

inline unsigned int PassPreset(const Config& cfg, unsigned int pass)
{
    return EffectivePassProfileFor(cfg, pass).preset;
}

inline unsigned int PassStyle(const Config& cfg, unsigned int pass)
{
    return EffectivePassProfileFor(cfg, pass).style;
}

inline NrPassTuning PassTuning(const Config& cfg, unsigned int pass)
{
    return EffectivePassProfileFor(cfg, pass).tuning;
}
}
