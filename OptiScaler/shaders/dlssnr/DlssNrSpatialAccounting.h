#pragma once

#include <cstdint>
#include <limits>

namespace DlssNr::Spatial
{
// Bounds library-owned scratch, including the old generation while it is parked.
// This is an admission ceiling, not an estimate of the provider's private VRAM.
// No quality/model setting is changed to satisfy it. A frame that cannot fit is skipped.
inline constexpr uint64_t kScratchByteLimit = 1024ull * 1024ull * 1024ull;
inline constexpr uint32_t kScratchSlotLimit = 64;

struct ScratchUsage
{
    uint64_t bytes = 0;
    uint32_t slots = 0;
    bool known = true;

    void Add(uint64_t allocationBytes, uint32_t allocations = 1)
    {
        if (!known || allocationBytes == 0 || allocationBytes == std::numeric_limits<uint64_t>::max() ||
            bytes > std::numeric_limits<uint64_t>::max() - allocationBytes ||
            slots > std::numeric_limits<uint32_t>::max() - allocations)
        {
            known = false;
            return;
        }
        bytes += allocationBytes;
        slots += allocations;
    }

    void Add(const ScratchUsage& other)
    {
        if (!other.known) known = false;
        if (other.slots != 0) Add(other.bytes, other.slots);
    }

    bool Allowed() const
    {
        return known && bytes <= kScratchByteLimit && slots <= kScratchSlotLimit;
    }
};
}
