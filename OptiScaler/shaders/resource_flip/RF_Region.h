#pragma once

#include <cstdint>
#include <limits>

inline constexpr std::uint32_t RFThreadsX = 16;
inline constexpr std::uint32_t RFThreadsY = 16;

struct RFRegion
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t offset = 0;
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
};

// Validate before narrowing/subtracting. Output is top-left anchored. The caller
// gates bottomAligned on rowFlip; vector-only conversion must use source row zero.
inline bool TryRFRegion(std::uint64_t width, std::uint64_t height,
                        std::uint64_t inputWidth, std::uint64_t inputHeight,
                        std::uint64_t outputWidth, std::uint64_t outputHeight,
                        bool bottomAligned, RFRegion& result)
{
    result = {};
    constexpr auto maxUint = (std::numeric_limits<std::uint32_t>::max)();
    if (width == 0 || height == 0 || inputWidth > maxUint || inputHeight > maxUint ||
        outputWidth > maxUint || outputHeight > maxUint ||
        width > inputWidth || height > inputHeight || width > outputWidth || height > outputHeight ||
        width > 65535ull * RFThreadsX || height > 65535ull * RFThreadsY)
        return false;

    result.width = static_cast<std::uint32_t>(width);
    result.height = static_cast<std::uint32_t>(height);
    result.offset = bottomAligned ? static_cast<std::uint32_t>(inputHeight - height) : 0;
    result.groupsX = (result.width + RFThreadsX - 1) / RFThreadsX;
    result.groupsY = (result.height + RFThreadsY - 1) / RFThreadsY;
    return true;
}
