// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <mutex>

namespace dlssg
{
// A CPU identity filter, not a recording or GPU completion authority. The
// provider retains both COM owners until its existing Reset/fence retirement.
// Never take continuity or call COM/SDK code while holding this short lock.
template<std::size_t Slots> class NativeGuideListInterest
{
    struct Pair
    {
        const void* list = nullptr;
        const void* nativeList = nullptr;
        bool published = false;
    };
    std::mutex _mutex;
    std::array<Pair, Slots> _pairs {};

  public:
    // Called under provider continuity, after the old slot physically retires
    // and before any SDK tag or conversion can expose the new recording.
    void Publish(std::size_t slot, const void* list, const void* nativeList)
    {
        std::lock_guard lock(_mutex);
        _pairs.at(slot) = {list, nativeList, true};
    }

    template<class List> bool MayContain(std::size_t count, List* const* lists)
    {
        if (!count) return false;
        if (!lists) return true; // An unknown identity is never proven unrelated.
        std::lock_guard lock(_mutex);
        for (const auto& pair : _pairs)
        {
            if (!pair.published) continue;
            if (!pair.list || !pair.nativeList) return true;
            for (std::size_t i = 0; i < count; ++i)
                if (!lists[i] || pair.list == lists[i] || pair.nativeList == lists[i]) return true;
        }
        return false;
        // The lock is released before the caller can acquire continuity. A
        // positive match must still be rechecked against the live slot there.
    }

    template<class List> bool MayContain(List* list) { return MayContain(1, &list); }

    // Only the provider's already-proven drain may discard all interests.
    void Clear()
    {
        std::lock_guard lock(_mutex);
        _pairs = {};
    }
};
}
