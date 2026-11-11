// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace GenshinFgLeaseBudget {
// retained includes active AND recycled allocations. reuse is true only for
// an exclusive retired lease with this exact device and normalized extent.
// Reusing those textures has no additional allocation charge.
inline constexpr bool Fits(uint64_t retained,uint64_t requested,bool reuse,uint64_t budget) noexcept {
    return requested<=budget && retained<=budget && (reuse || requested<=budget-retained);
}
enum class Admission { Ready, NoVacantSlot, AdditionalAllocationBudget };
inline constexpr Admission Classify(bool vacant,uint64_t retained,uint64_t requested,bool reuse,uint64_t budget) noexcept {
    if(!vacant)return Admission::NoVacantSlot;
    return Fits(retained,requested,reuse,budget)?Admission::Ready:Admission::AdditionalAllocationBudget;
}
// Only incompatible, fully retired, sole-owned cache entries may be discarded.
// Active slots and unfinished/external holders are never allocation headroom.
inline constexpr bool Discardable(bool exclusive,bool complete,bool compatible) noexcept {
    return exclusive && complete && !compatible;
}
inline constexpr void Increment(uint32_t& count) noexcept {if(count!=UINT32_MAX)++count;}
struct Counters {
    uint32_t noVacantSlot=0,additionalAllocationBudget=0,deviceMismatch=0,extentMismatch=0;
    uint32_t unfinishedHolders=0,externalHolders=0,recycledDiscarded=0,reused=0,allocated=0;
    uint32_t refused=0,reports=0,seenMask=0;
};
}
