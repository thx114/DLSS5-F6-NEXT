// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

namespace FramePipeline {
namespace detail {
template<class Changed>
void ChangedByteRangesScalar(const unsigned char* current,const unsigned char* previous,
                            size_t size,Changed&& changed,size_t i=0) {
    for(;i<size;){
        if(current[i]==previous[i]){++i;continue;}
        const auto begin=i++;
        while(i<size && current[i]!=previous[i])++i;
        changed(begin,i);
    }
}

// Bit n describes byte offset+n. Carry the final state across blocks so a run
// crossing a SIMD/block boundary is emitted once, including the scalar tail.
template<unsigned Width,class Changed>
void ChangedByteMaskRanges(std::uint64_t mask,size_t offset,std::uint64_t& previousBit,
                          size_t& runBegin,Changed& changed) {
    static_assert(Width>0 && Width<=64);
    constexpr auto valid=~std::uint64_t{0}>>(64-Width);
    auto transitions=(mask^((mask<<1)|previousBit))&valid;
    previousBit=mask>>(Width-1);
    while(transitions) {
        const auto bit=std::countr_zero(transitions);
        if(mask&(std::uint64_t{1}<<bit))runBegin=offset+bit;
        else changed(runBegin,offset+bit);
        transitions&=transitions-1;
    }
}

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
inline std::uint64_t EqualByteMask16(const unsigned char* current,const unsigned char* previous) {
    const auto a=_mm_loadu_si128(reinterpret_cast<const __m128i*>(current));
    const auto b=_mm_loadu_si128(reinterpret_cast<const __m128i*>(previous));
    return static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(a,b)));
}
#endif
}
// Both inputs must be ordinary CPU snapshots of exactly size bytes. Never use
// this with a driver mapping (which may be write-combined or GPU-owned).
// Keep the snapshots unchanged until return. Report only maximal actually
// changed runs: a NO_OVERWRITE write may not touch an equal byte. SIMD loads
// stay inside the extent; other architectures retain the scalar fallback.
template<class Changed>
void ForEachChangedByteRange(const unsigned char* current,const unsigned char* previous,
                            size_t size,Changed&& changed) {
    if(!size)return;
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    size_t i=0,runBegin=0;
    std::uint64_t previousBit=0;
    while(size-i>=64) {
        const auto equal=detail::EqualByteMask16(current+i,previous+i)|
            (detail::EqualByteMask16(current+i+16,previous+i+16)<<16)|
            (detail::EqualByteMask16(current+i+32,previous+i+32)<<32)|
            (detail::EqualByteMask16(current+i+48,previous+i+48)<<48);
        const auto mask=~equal;
        const auto transitions=mask^((mask<<1)|previousBit);
        // Many short runs make mask enumeration more expensive than a byte
        // scan. Resume scalar once per call, retaining an already-open run.
        if(transitions && std::popcount(transitions)>8) {
            if(previousBit) {
                while(i<size && current[i]!=previous[i])++i;
                changed(runBegin,i);
            }
            detail::ChangedByteRangesScalar(current,previous,size,changed,i);
            return;
        }
        detail::ChangedByteMaskRanges<64>(mask,i,previousBit,runBegin,changed);
        i+=64;
    }
    while(size-i>=16) {
        const auto mask=detail::EqualByteMask16(current+i,previous+i)^std::uint64_t{0xFFFF};
        detail::ChangedByteMaskRanges<16>(mask,i,previousBit,runBegin,changed);
        i+=16;
    }
    for(;i<size;++i) {
        const std::uint64_t bit=current[i]!=previous[i];
        if(bit!=previousBit) {
            if(bit)runBegin=i;
            else changed(runBegin,i);
            previousBit=bit;
        }
    }
    if(previousBit)changed(runBegin,size);
#else
    detail::ChangedByteRangesScalar(current,previous,size,changed);
#endif
}
} // namespace FramePipeline
