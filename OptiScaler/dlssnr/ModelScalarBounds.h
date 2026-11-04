#pragma once
#include <algorithm>
#include <cmath>

namespace DlssNr::Profiles
{
inline constexpr float ScalarDefault = 1.0f;
inline constexpr float ScalarMaximum(unsigned int preset) noexcept
{
    return preset == 2u ? 1.0f : 2.0f; // 0 default, 1 preset A, 2 preset B
}
inline float BoundedScalar(float value, unsigned int preset, float minimum = 0.0f) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, ScalarMaximum(preset)) : ScalarDefault;
}
}
