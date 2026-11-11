// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFsrProfiles.h"
#include <cstdint>
#include "legacy/Fsr2FamilyTakeover.h"

namespace GenshinLegacy::DrawAdmission {
// Geometry describes work, never shader identity. In particular an indexed
// quad is not a negative proof of an upscaler pass. Unknown GPU arguments stay
// unknown; this policy never reads them or supplies a surrogate count.
struct Call {
    uint32_t count=0,kind=0;
    bool countKnown=false;
    constexpr bool KnownWork() const noexcept {
        return kind>=1 && kind<=4 && countKnown && count>0;
    }
    constexpr bool LegacyReplacement() const noexcept {
        return KnownWork() && count==3;
    }
};
constexpr bool FamilyShader(uint64_t hash) noexcept {
    return GenshinFsrProfiles::Accumulate(hash) ||
        hash==fsr2_family_takeover::k_pre_hash_1 || hash==fsr2_family_takeover::k_pre_hash_2 ||
        hash==fsr2_family_takeover::k_pre_hash_3 || hash==fsr2_family_takeover::k_pre_hash_4;
}
// Only a current authenticated shader observation can prove the negative.
// KnownWork is separate: zero/indirect geometry grants no source capability.
constexpr bool NonFamilyShader(bool known,uint64_t hash) noexcept {
    return known && !FamilyShader(hash);
}
}
