// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// CPU identity policy only. Queue ordering and GPU retirement stay with the
// actual producer/NR/provider receipts; these values cannot manufacture either.
namespace dlssg::colour
{
enum class Route { NativePresent, Dx11CopiedPresent };
enum class Mutation { Original, NrWritten, Uncertain };
struct Source
{
    uint64_t serial=0, frame=0, generation=0, epoch=0, feature=0, configuration=0;
    uintptr_t provider=0, swapchain=0, output=0;
    bool operator==(const Source&) const = default;
};
struct Endpoint
{
    uintptr_t swapchain=0, colour=0, queue=0, device=0;
    uint64_t present=0;
    bool operator==(const Endpoint&) const = default;
};
inline bool SameEndpoint(const Endpoint& actual, const Endpoint& observed) noexcept
{
    return actual.swapchain && actual.colour && actual.queue && actual.device &&
        actual == observed;
}
inline bool CanUseHudless(const Source& source, const Source& captured, Mutation mutation) noexcept
{
    // A pre-NR HUDless copy is never relabelled as processed NR colour. Missing
    // or mismatched optional colour says nothing about depth/MV FG admission.
    return source.serial && source.output && source == captured && mutation == Mutation::Original;
}
}
