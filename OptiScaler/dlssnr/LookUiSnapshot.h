#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace DlssNr
{

// Process-local UI state for the 16 final-look controls. This has no Config,
// provider, D3D, queue, fence, hook, or file-I/O dependency.
struct LookUiSnapshot
{
    static constexpr std::size_t Count = 16;

    bool stored = false;
    std::array<float, Count> values {};

    bool Valid(const float* lows, const float* highs, std::size_t count = Count) const noexcept
    {
        if (!stored || lows == nullptr || highs == nullptr || count != Count)
            return false;

        for (std::size_t i = 0; i < Count; ++i)
        {
            const float value = values[i];
            if (!std::isfinite(value) || value < lows[i] || value > highs[i])
                return false;
        }
        return true;
    }
};

static_assert(LookUiSnapshot::Count == 16, "Look snapshot must cover the complete v10.7 look allowlist");

} // namespace DlssNr
