#pragma once

namespace DlssNr
{
// One bound for configuration, UI, runtime loops and per-pass storage.
inline constexpr unsigned int MaxPassCount = 6;
inline constexpr unsigned int DefaultMaxPassCount = MaxPassCount;
inline constexpr unsigned int ExtraPassCount = MaxPassCount - 3;

inline constexpr unsigned int ClampPassCount(unsigned int count) noexcept
{
    return count < 1 ? 1 : count > MaxPassCount ? MaxPassCount : count;
}
}
