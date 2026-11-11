// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace GenshinFsrProfiles {
// Reviewed actual creation bytes, not layout guesses. FNV is only an index;
// the creation record must also authenticate size and SHA256 before numerical
// capabilities can be issued. Unknown shaders remain native draws.
enum class Colour : uint8_t { Base, NativeOverlay };
struct Profile {
    uint64_t hash;
    size_t bytes;
    std::array<unsigned char,32> digest;
    Colour colour;
    bool skipPrepasses;
};
inline constexpr std::array<Profile,2> Reviewed={{
    {0x78057A29AF6C2D99ull,15388,
     {0xE2,0xCF,0xA1,0x65,0xB0,0xEB,0xF3,0x6F,0x65,0x5C,0x7E,0x9E,0xF7,0x82,0x3B,0x6A,
      0xFE,0xD1,0x11,0x3E,0x18,0xD6,0xFC,0xB7,0xD8,0x2F,0xE5,0x87,0x7D,0xF5,0xAD,0xBF},Colour::Base,true},
    {0xFEAC669E48FD931Eull,15656,
     {0xF7,0x5F,0xB7,0x26,0xE8,0x66,0x1C,0x5E,0xB5,0xF6,0x39,0xB4,0x7F,0xA9,0x91,0x45,
      0xA7,0xD4,0x7E,0x07,0xB3,0xD2,0xAC,0x0F,0x43,0x57,0x0D,0x42,0xD9,0x3A,0x9A,0xFE},Colour::NativeOverlay,false}
}};
constexpr const Profile* Find(uint64_t hash) noexcept {
    for(const auto& p:Reviewed)if(p.hash==hash)return &p;
    return nullptr;
}
constexpr bool Accumulate(uint64_t hash) noexcept {return Find(hash)!=nullptr;}
constexpr bool Overlay(uint64_t hash) noexcept {
    const auto* p=Find(hash);return p && p->colour==Colour::NativeOverlay;
}
constexpr bool SkipPrepasses(uint64_t hash) noexcept {
    const auto* p=Find(hash);return p && p->skipPrepasses;
}
constexpr const Profile* Authenticate(uint64_t hash,size_t bytes,const std::array<unsigned char,32>& digest) noexcept {
    const auto* p=Find(hash);return p && p->bytes==bytes && p->digest==digest?p:nullptr;
}
}
