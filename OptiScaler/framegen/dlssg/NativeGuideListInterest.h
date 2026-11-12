// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

namespace dlssg
{
// A CPU identity filter, not a recording or GPU completion authority. The
// provider retains both COM owners until its existing Reset/fence retirement.
// Never take continuity or call COM/SDK code while holding this short lock.
class NativeGuideListInterest
{
    struct Pair
    {
        const void* recording = nullptr;
        const void* list = nullptr;
        const void* nativeList = nullptr;
    };
    std::mutex _mutex;
    std::vector<Pair> _pairs;

  public:
    // Recording identities outlive presentation-slot wrap. Called under
    // continuity before any SDK tag or conversion can expose this recording.
    void Publish(const void* recording, const void* list, const void* nativeList)
    {
        std::lock_guard lock(_mutex);
        for (auto& pair : _pairs)
            if (pair.recording == recording) { pair = {recording, list, nativeList}; return; }
        _pairs.push_back({recording, list, nativeList});
    }

    // Only successful Reset plus the exact producer fence permits removal.
    void Erase(const void* recording)
    {
        std::lock_guard lock(_mutex);
        std::erase_if(_pairs, [=](const auto& pair) { return pair.recording == recording; });
    }

    template<class List> bool MayContain(std::size_t count, List* const* lists)
    {
        if (!count) return false;
        if (!lists) return true; // An unknown identity is never proven unrelated.
        std::lock_guard lock(_mutex);
        for (const auto& pair : _pairs)
        {
            if (!pair.list || !pair.nativeList) return true;
            for (std::size_t i = 0; i < count; ++i)
                if (!lists[i] || pair.list == lists[i] || pair.nativeList == lists[i]) return true;
        }
        return false;
        // The lock is released before the caller can acquire continuity. A
        // positive match must still be rechecked against live recordings there.
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
