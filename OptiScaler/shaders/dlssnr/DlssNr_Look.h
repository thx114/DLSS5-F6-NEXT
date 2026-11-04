#pragma once

#include "DlssNr_Common.h"
#include <dlssnr/LayerLookProfile.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// CPU-side mapping for the reviewed independent F5 final-edit shader. This does
// not change model parameters, history, feature lifetime, or provider selection.
namespace DlssNrLook
{
inline float Bounded(float value, float low, float high, float neutral)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : neutral;
}

template <typename Configuration>
void Fill(DlssNrConstants& params, const Configuration& config)
{
    // Global Look is always active, including legacy configurations with LookMode=false.
    params.LookMode = 1u;
    params.LayerLookUiMask = 0u;
    params.LookStrength = Bounded(config.DlssNrLookStrength.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookBrighten = Bounded(config.DlssNrLookBrighten.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookDarken = Bounded(config.DlssNrLookDarken.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookColour = Bounded(config.DlssNrLookColour.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookHue = Bounded(config.DlssNrLookHue.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookShadows = Bounded(config.DlssNrLookShadows.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookMidtones = Bounded(config.DlssNrLookMidtones.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookHighlights = Bounded(config.DlssNrLookHighlights.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookMaxBrighten = Bounded(config.DlssNrLookMaxBrighten.value_or_default(), 0.0f, 4.0f, 0.0f);
    params.LookMaxDarken = Bounded(config.DlssNrLookMaxDarken.value_or_default(), 0.0f, 4.0f, 0.0f);
    params.LookMaxColour = Bounded(config.DlssNrLookMaxColour.value_or_default(), 0.0f, 2.0f, 0.0f);
    params.LookTone = Bounded(config.DlssNrLookTone.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookDetail = Bounded(config.DlssNrLookDetail.value_or_default(), 0.0f, 2.0f, 1.0f);
    params.LookDetailRadius = Bounded(config.DlssNrLookDetailRadius.value_or_default(), 0.25f, 4.0f, 1.0f);
    params.LookHalo = Bounded(config.DlssNrLookHalo.value_or_default(), 0.0f, 1.0f, 0.0f);
    params.LookHaloBrightSide = Bounded(config.DlssNrLookHaloBrightSide.value_or_default(), 0.0f, 1.0f, 0.0f);
}

inline void Fill(DlssNrConstants& params, const DlssNr::LayerLook::Profile& profile)
{
    params.LookMode = profile.enabled ? 1u : 0u;
    params.LayerLookUiMask = profile.protectUi ? 1u : 0u;
    params.LookStrength = profile.values[0];
    params.LookBrighten = profile.values[1];
    params.LookDarken = profile.values[2];
    params.LookColour = profile.values[3];
    params.LookHue = profile.values[4];
    params.LookShadows = profile.values[5];
    params.LookMidtones = profile.values[6];
    params.LookHighlights = profile.values[7];
    params.LookMaxBrighten = profile.values[8];
    params.LookMaxDarken = profile.values[9];
    params.LookMaxColour = profile.values[10];
    params.LookTone = profile.values[11];
    params.LookDetail = profile.values[12];
    params.LookDetailRadius = profile.values[13];
    params.LookHalo = profile.values[14];
    params.LookHaloBrightSide = profile.values[15];
}
template<class Configuration>
void FillFinal(DlssNrConstants& params, const Configuration& config, bool independentLookConsumed)
{
    // The caller supplies an actual post-Look answer (fresh or contract-matched reuse),
    // not merely an enabled setting. Only the legacy Look is suppressed: all other
    // final-composition constants belong to the caller and must remain intact.
    if (independentLookConsumed)
        params.LookMode = 0u;
    else
        Fill(params, config);
}

// A content revision for the values actually copied into this dispatch's CBV.
// It is diagnostic identity, not a completion fence or proof of display. Hash
// only explicit scalar ranges: never the alignas(256) tail padding or exposure
// samples, whose per-frame drift would flood the log during ordinary gameplay.
inline uint64_t Revision(const DlssNrConstants& params)
{
    uint64_t revision = 14695981039346656037ull;
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            revision ^= bytes[i];
            revision *= 1099511628211ull;
        }
    };
    constexpr size_t lookBytes = offsetof(DlssNrConstants, LookHaloBrightSide) + sizeof(float) -
                                 offsetof(DlssNrConstants, LookMode);
    static_assert(lookBytes == 17 * sizeof(uint32_t));
    // Inspecting an object's representation through unsigned char is permitted;
    // avoid pointer arithmetic from one scalar member into a different member.
    append(reinterpret_cast<const unsigned char*>(&params) + offsetof(DlssNrConstants, LookMode), lookBytes);
    append(&params.TransferStrength, sizeof(params.TransferStrength));
    append(&params.ColourStrength, sizeof(params.ColourStrength));
    append(&params.SkinProtection, sizeof(params.SkinProtection));
    append(&params.ShowSkinMask, sizeof(params.ShowSkinMask));
    append(&params.SkinDetail, sizeof(params.SkinDetail));
    append(&params.SkinColour, sizeof(params.SkinColour));
    append(&params.EnvironmentDetail, sizeof(params.EnvironmentDetail));
    append(&params.EnvironmentColour, sizeof(params.EnvironmentColour));
    append(&params.ReplaceDetailStrength, sizeof(params.ReplaceDetailStrength));
    append(&params.ApplyModel, sizeof(params.ApplyModel));
    append(&params.DebugView, sizeof(params.DebugView));
    append(&params.CompareMode, sizeof(params.CompareMode));
    return revision;
}
} // namespace DlssNrLook
