#pragma once

#include "GimiPlan.h"
#include <algorithm>
#include <cstring>

namespace gimi
{
inline std::uint32_t IniParameterBits(float value) noexcept
{
    std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
inline float IniParameterFloat(std::uint32_t bits) noexcept
{
    float value; std::memcpy(&value, &bits, sizeof(value)); return value;
}

// Fresh private Opt state starts at +0 plus the Plan's ordered static Constants
// assignments. No external loader's state, dynamic Constants commands, saved
// parameters or live GPU page is imported. Those need an explicit contract.
// Each complete page is an immutable Texture1D<float4>, including signed zero.
// Domain proof and GPU creation are load/Present-only; draw never uploads.
// The full core's finite flags, shader classifications and restored overlay
// images can exceed the old 512-page product. Keep scalar-domain proof at 512;
// complete pages have a separate limit and remain charged to PreloadLimits.
inline constexpr std::uint32_t IniParameterPageLimit = 4096;

struct IniParameterSlot
{
    Id scalar = 0;
    std::uint32_t stride = 0; // reserved legacy radix field; layout retained
    std::vector<std::uint32_t> values; // float bits indexed by complete image ID
};
struct IniParameterSpec
{
    SourceLocation source;
    bool required = false;
    std::uint32_t states = 0, initial = 0;
    // Ascending scalar IDs, including singleton written domains. Complete
    // images are lexicographically sorted by these columns, bounded above.
    // Key table and immutable pages share the existing preload budget.
    std::vector<IniParameterSlot> slots;
    std::array<std::uint32_t, IniParameterScalars> defaults{};

    bool Read(std::uint32_t state, Id scalar, float& value) const noexcept
    {
        if (!required || state >= states || scalar >= IniParameterScalars) return false;
        value = IniParameterFloat(defaults[scalar]);
        const auto slot = std::lower_bound(slots.begin(), slots.end(), scalar,
            [](const IniParameterSlot& entry, Id id) { return entry.scalar < id; });
        if (slot != slots.end() && slot->scalar == scalar)
            value = IniParameterFloat(slot->values[state]);
        return true;
    }
    bool Write(std::uint32_t state, Id scalar, float value, std::uint32_t& next) const noexcept
    {
        if (!required || state >= states || scalar >= IniParameterScalars) return false;
        const auto bits = IniParameterBits(value);
        const auto slot = std::lower_bound(slots.begin(), slots.end(), scalar,
            [](const IniParameterSlot& entry, Id id) { return entry.scalar < id; });
        if (slot == slots.end() || slot->scalar != scalar) return false;
        // Search the WHOLE next key. No allocation, hash, upload, partial-page
        // match or independent scalar acceptance can authorize a missing page.
        std::uint32_t lo = 0, hi = states;
        while (lo < hi)
        {
            const auto mid = lo + (hi - lo) / 2;
            int order = 0;
            for (const auto& entry : slots)
            {
                const auto wanted = entry.scalar == scalar ? bits : entry.values[state];
                if (entry.values[mid] != wanted) { order = entry.values[mid] < wanted ? -1 : 1; break; }
            }
            if (!order) { next = mid; return true; }
            if (order < 0) lo = mid + 1; else hi = mid;
        }
        return false;
    }
    bool Find(const std::vector<float>& image, std::uint32_t& state) const noexcept
    {
        if (!required || !states || image.size() != IniParameterScalars) return false;
        std::uint32_t lo = 0, hi = states;
        while (lo < hi)
        {
            const auto mid = lo + (hi - lo) / 2;
            int order = 0;
            for (const auto& slot : slots)
            {
                const auto wanted = IniParameterBits(image[slot.scalar]);
                if (slot.values[mid] != wanted) { order = slot.values[mid] < wanted ? -1 : 1; break; }
            }
            if (!order)
            {
                // Also compare EVERY unwritten default. Native text's dynamic
                // x/y/w cannot hide in a sparse key or leak into overlay t120.
                auto slot = slots.begin();
                for (Id scalar = 0; scalar < IniParameterScalars; ++scalar)
                {
                    auto expected = defaults[scalar];
                    if (slot != slots.end() && slot->scalar == scalar) { expected = slot->values[mid]; ++slot; }
                    if (IniParameterBits(image[scalar]) != expected) return false;
                }
                state = mid; return true;
            }
            if (order < 0) lo = mid + 1; else hi = mid;
        }
        return false;
    }
};
} // namespace gimi
