// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>

namespace FramePipeline
{
enum class FenceCompletion { Pending, Complete, Invalid };

// Call only after binding the actual queue/fence and its successfully signalled
// target. This classifies the raw observation; it does not establish ownership.
// D3D12's device-loss sentinel must never authorize Reset or resource retirement.
constexpr FenceCompletion ObserveFence(std::uint64_t completed, std::uint64_t target) noexcept
{
    constexpr auto invalid = (std::numeric_limits<std::uint64_t>::max)();
    if (completed == invalid || target == invalid) return FenceCompletion::Invalid;
    return completed >= target ? FenceCompletion::Complete : FenceCompletion::Pending;
}
}
