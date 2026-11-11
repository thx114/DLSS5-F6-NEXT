#pragma once
#include "PassLimits.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace DlssNr::LayerLook
{
struct Parameter { const char* key; const char* cn; const char* en; float low, high, neutral; };
inline constexpr std::array<Parameter, 16> Parameters {{
    { "Strength", "\xE6\x95\x88\xE6\x9E\x9C\xE5\xBC\xBA\xE5\xBA\xA6", "Effect strength", 0.00f, 2.00f, 1.00f },
    { "Brighten", "\xE6\x8F\x90\xE4\xBA\xAE", "Brightening", 0.00f, 2.00f, 1.00f },
    { "Darken", "\xE5\x8E\x8B\xE6\x9A\x97", "Darkening", 0.00f, 2.00f, 1.00f },
    { "Colour", "\xE8\x89\xB2\xE5\xBD\xA9", "Colour", 0.00f, 2.00f, 1.00f },
    { "Hue", "\xE8\x89\xB2\xE7\x9B\xB8", "Hue", 0.00f, 2.00f, 1.00f },
    { "Shadows", "\xE9\x98\xB4\xE5\xBD\xB1", "Shadows", 0.00f, 2.00f, 1.00f },
    { "Midtones", "\xE4\xB8\xAD\xE9\x97\xB4\xE8\xB0\x83", "Midtones", 0.00f, 2.00f, 1.00f },
    { "Highlights", "\xE9\xAB\x98\xE5\x85\x89", "Highlights", 0.00f, 2.00f, 1.00f },
    { "MaxBrighten", "\xE6\x9C\x80\xE5\xA4\xA7\xE6\x8F\x90\xE4\xBA\xAE", "Brightening ceiling", 0.00f, 4.00f, 0.00f },
    { "MaxDarken", "\xE6\x9C\x80\xE5\xA4\xA7\xE5\x8E\x8B\xE6\x9A\x97", "Darkening floor", 0.00f, 4.00f, 0.00f },
    { "MaxColour", "\xE8\x89\xB2\xE5\xBD\xA9\xE8\xBD\xAF\xE9\x99\x90\xE5\x88\xB6", "Colour soft limit", 0.00f, 2.00f, 0.00f },
    { "Tone", "\xE5\xA4\xA7\xE5\xB0\xBA\xE5\xBA\xA6\xE5\x85\x89\xE7\x85\xA7", "Broad lighting", 0.00f, 2.00f, 1.00f },
    { "Detail", "\xE7\xBB\x86\xE8\x8A\x82", "Detail", 0.00f, 2.00f, 1.00f },
    { "DetailRadius", "\xE5\x88\x86\xE7\xA6\xBB\xE5\x8D\x8A\xE5\xBE\x84", "Separation radius", 0.25f, 4.00f, 1.00f },
    { "Halo", "\xE4\xBA\xAE\xE6\x99\x95\xE6\x8A\x91\xE5\x88\xB6", "Bright halo suppression", 0.00f, 1.00f, 0.00f },
    { "HaloBrightSide", "\xE6\x9A\x97\xE6\x99\x95\xE6\x8A\x91\xE5\x88\xB6", "Dark halo suppression", 0.00f, 1.00f, 0.00f },
}};
// Persisted PassNLookOverrides bits follow this stable parameter order.
// New parameters must not renumber existing entries.
static_assert(Parameters.size() < 32);
inline constexpr uint32_t ParameterMask = (uint32_t{1} << Parameters.size()) - 1u;
template<class Storage> uint32_t Overrides(const Storage& storage)
{
    return storage.overrides.value_or_default() & ParameterMask;
}
// Persisted PassNLookUiMask values. Never renumber these policies.
enum class UiMask : uint32_t { None = 0, Protect = 1, First = 2, Previous = 3 };
inline uint32_t BoundUiMask(uint32_t value) { return value <= 3u ? value : 0u; }
template<class Configuration> bool ResolveUiMask(const Configuration& config, unsigned layer)
{
    if (layer >= MaxPassCount) return false;
    // Every inherited edge goes backwards. Mask inheritance is independent of
    // colour-parameter inheritance and does not enable any processing layer.
    for (unsigned remaining = MaxPassCount; remaining != 0; --remaining)
    {
        const auto policy = static_cast<UiMask>(BoundUiMask(config.DlssNrLayerLooks[layer].uiMask.value_or_default()));
        if (policy == UiMask::Protect) return true;
        if (policy == UiMask::None || layer == 0) return false;
        layer = policy == UiMask::First ? 0u : layer - 1u;
    }
    return false;
}
struct Profile
{
    bool enabled = false;
    bool protectUi = false;
    std::array<float, 16> values {};
};
inline float Bound(float value, const Parameter& p)
{
    return std::isfinite(value) ? std::clamp(value, p.low, p.high) : p.neutral;
}
template<class Storage, class Visitor> void Visit(Storage& storage, Visitor visitor)
{
    visitor(0u, storage.DlssNrLookStrength);
    visitor(1u, storage.DlssNrLookBrighten);
    visitor(2u, storage.DlssNrLookDarken);
    visitor(3u, storage.DlssNrLookColour);
    visitor(4u, storage.DlssNrLookHue);
    visitor(5u, storage.DlssNrLookShadows);
    visitor(6u, storage.DlssNrLookMidtones);
    visitor(7u, storage.DlssNrLookHighlights);
    visitor(8u, storage.DlssNrLookMaxBrighten);
    visitor(9u, storage.DlssNrLookMaxDarken);
    visitor(10u, storage.DlssNrLookMaxColour);
    visitor(11u, storage.DlssNrLookTone);
    visitor(12u, storage.DlssNrLookDetail);
    visitor(13u, storage.DlssNrLookDetailRadius);
    visitor(14u, storage.DlssNrLookHalo);
    visitor(15u, storage.DlssNrLookHaloBrightSide);
}
template<class Storage> Profile Capture(const Storage& storage)
{
    Profile result;
    result.enabled = storage.DlssNrLookMode.value_or_default();
    Visit(storage, [&](unsigned i, const auto& option) { result.values[i] = Bound(option.value_or_default(), Parameters[i]); });
    return result;
}
// Runtime policy must not rewrite the legacy INI value or any Look parameter.
template<class Configuration> void EnforceGlobalEnable(Configuration& config)
{
    config.DlssNrLookMode.set_volatile_value(true);
}
// Inheritance copies resolved parameter values only, including explicit upstream
// overrides. Layer 1 is enabled in independent mode; other layers retain their own
// enable. 0 local; 1 first layer; 2 previous layer. References only go backwards.
// Local override bits never enable a layer or affect the legacy global Look.
template<class Configuration> Profile Resolve(const Configuration& config, unsigned layer)
{
    Profile result;
    for (unsigned i = 0; i < Parameters.size(); ++i) result.values[i] = Parameters[i].neutral;
    if (layer >= MaxPassCount) return result;
    const bool enabled = layer == 0 || config.DlssNrLayerLooks[layer].DlssNrLookMode.value_or_default();
    // Bound traversal by the fixed pass count. No allocation or recursive resolver
    // is needed on the dispatch path, and disabled sources still supply values.
    std::array<unsigned, MaxPassCount> inheritedLayers {};
    unsigned count = 0, source = layer;
    while (source > 0)
    {
        const unsigned inherit = config.DlssNrLayerLooks[source].inherit.value_or_default();
        if (inherit != 1 && inherit != 2) break;
        inheritedLayers[count++] = source;
        source = inherit == 1 ? 0u : source - 1u;
    }
    result = Capture(config.DlssNrLayerLooks[source]);
    // Apply ancestors first so "previous layer" means its actual resolved GPU
    // profile, rather than skipping an intermediate layer's local adjustments.
    while (count > 0)
    {
        const auto& storage = config.DlssNrLayerLooks[inheritedLayers[--count]];
        const uint32_t overrides = Overrides(storage);
        if (overrides == 0) continue;
        Visit(storage, [&](unsigned i, const auto& option) {
            if ((overrides & (uint32_t{1} << i)) != 0)
                result.values[i] = Bound(option.value_or_default(), Parameters[i]);
        });
    }
    result.enabled = config.DlssNrLayerLookMode.value_or_default() && enabled;
    result.protectUi = ResolveUiMask(config, layer);
    return result;
}
template<class Storage> void Reset(Storage& storage)
{
    storage.DlssNrLookMode = false;
    storage.inherit = 0u;
    storage.overrides = 0u;
    storage.uiMask = 0u;
    Visit(storage, [](unsigned i, auto& option) { option = Parameters[i].neutral; });
}
// Exact bounded GPU profile values; explicit scalar bits, never padding.
inline uint64_t Revision(const Profile& profile)
{
    if (!profile.enabled) return 0;
    uint64_t hash = 14695981039346656037ull;
    for (float value : profile.values)
    {
        uint32_t bits = std::bit_cast<uint32_t>(value);
        for (unsigned byte = 0; byte < 4; ++byte) { hash ^= (bits >> (8*byte)) & 255u; hash *= 1099511628211ull; }
    }
    // Preserve legacy identity exactly when no mask is requested. This is a
    // settings identity only: current-frame mask pixels are never cache proof.
    if (profile.protectUi) { hash ^= 1u; hash *= 1099511628211ull; }
    return hash;
}
template<class Configuration> uint64_t ChainRevision(const Configuration& config, unsigned activePassCount)
{
    uint64_t hash = 0;
    for (unsigned pass = 0; pass < std::min(activePassCount, MaxPassCount); ++pass)
    {
        const auto profile = Resolve(config, pass);
        if (profile.enabled) hash = (hash * 1000003ull) ^ Revision(profile) ^ (uint64_t(pass + 1) << 56);
    }
    return hash;
}
}
