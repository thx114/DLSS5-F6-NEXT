#pragma once

// The command lists NR has recorded something on that the submit and reset hooks must hear about: its GPU timestamps
// (DlssNrGpuTime), Reuse's coverage readbacks and the finished-picture copy (DlssNr_Late.inl). Those hooks run on every
// ExecuteCommandLists and command list Reset in the process, on any thread and any queue (the game's workers, frame
// generation's presenter). NR holds g_nrMutex for its whole recording, so a hook that always took it would hold all of
// them up for that long. A hook first looks here, without a lock, and takes g_nrMutex only for a list that is watched.
//
// Lists are identities only (never dereferenced). Add is called while recording, before the list can be submitted; the
// successful Reset closes the tracked executable generation. Mutations are
// serialized by the NR mutex and precede the recording-to-submit handoff.
// Concurrent readers never hold the overflow mutex while entering NR. Overflow keeps
// exact identities, so an occupancy peak never permanently enables all hooks.

#include <atomic>
#include <cstddef>
#include <mutex>
#include <unordered_set>

namespace DlssNrWatchedLists
{
inline constexpr size_t kCount = 64;

inline std::atomic<const void*> g_lists[kCount] {};
inline std::mutex g_overflowMutex;
inline std::unordered_set<const void*> g_overflow;
inline std::atomic<size_t> g_overflowCount = 0;
inline std::atomic<size_t> g_overflowPeak = 0; // observation only; never a lifecycle gate

inline void Add(const void* list)
{
    if (list == nullptr)
        return;

    for (auto& entry : g_lists)
    {
        if (entry.load() == list)
            return;
    }

    // A previously overflowing identity can be recorded again after fast
    // slots drain. Keep exactly one membership, including across those stores.
    if (g_overflowCount.load(std::memory_order_acquire))
    {
        std::lock_guard lock(g_overflowMutex);
        if (g_overflow.contains(list)) return;
    }

    for (auto& entry : g_lists)
    {
        const void* expected = nullptr;

        if (entry.compare_exchange_strong(expected, list))
            return;
    }

    std::lock_guard lock(g_overflowMutex);
    g_overflow.insert(list);
    g_overflowCount.store(g_overflow.size(), std::memory_order_release);
    if (g_overflow.size() > g_overflowPeak.load(std::memory_order_relaxed))
        g_overflowPeak.store(g_overflow.size(), std::memory_order_relaxed);
}

// Only a matching identity asks the caller to enter the NR global lock.
inline bool Any(unsigned count, const void* const* lists)
{
    if (lists == nullptr)
        return false;

    for (auto& entry : g_lists)
    {
        const void* watched = entry.load();

        if (watched == nullptr)
            continue;

        for (unsigned i = 0; i < count; ++i)
        {
            if (lists[i] == watched)
                return true;
        }
    }

    if (g_overflowCount.load(std::memory_order_acquire))
    {
        std::lock_guard lock(g_overflowMutex);
        for (unsigned i = 0; i < count; ++i)
            if (g_overflow.contains(lists[i])) return true;
    }
    return false;
}

inline void Remove(unsigned count, const void* const* lists)
{
    if (lists == nullptr)
        return;

    for (unsigned i = 0; i < count; ++i)
    {
        if (lists[i] == nullptr)
            continue;

        for (auto& entry : g_lists)
        {
            const void* expected = lists[i];
            entry.compare_exchange_strong(expected, nullptr);
        }
    }
    if (g_overflowCount.load(std::memory_order_acquire))
    {
        std::lock_guard lock(g_overflowMutex);
        for (unsigned i = 0; i < count; ++i) g_overflow.erase(lists[i]);
        g_overflowCount.store(g_overflow.size(), std::memory_order_release);
    }
}
} // namespace DlssNrWatchedLists
