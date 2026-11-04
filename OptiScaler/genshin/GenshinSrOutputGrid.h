// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinOutputContract.h"
#include <bit>
#include <cstdint>
namespace GenshinGuides {
// Issued by the same-call DLSS12 observer only after successful vendor return,
// successful coordinate-preserving postpasses and the outer Evaluate receipt.
// Canonical copyback/Present correspondence and GPU completion are separate.
struct SrOutputGrid {
    Owner owner{};
    uint64_t attempt=0;
    uint32_t renderWidth=0,renderHeight=0,outputWidth=0,outputHeight=0;
    float jitterPixelsX=0,jitterPixelsY=0;
    bool Matches(const Owner& expected) const noexcept {
        auto finite=[](float f){return (std::bit_cast<uint32_t>(f)&0x7f800000u)!=0x7f800000u;};
        return SameOwner(owner,expected) && attempt && renderWidth && renderHeight && outputWidth && outputHeight &&
            finite(jitterPixelsX) && finite(jitterPixelsY);
    }
};
}
