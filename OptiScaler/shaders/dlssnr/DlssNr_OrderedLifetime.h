#pragma once
#include "DlssNr_RecordingLifetime.h"
#include <dlssnr/DlssNrOrderedReuse.h>
#include <utility>

// Actual product recording lifecycle, shared with the offscreen fixture. Call under
// the NR mutex, AFTER real Execute or successful Reset; no queue/CPU waits here.
namespace DlssNrGpuLifetime
{
template <class Key> struct HistoryRecord
{
    ID3D12CommandList* commands = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandList> commandOwner;
    Use resourceUse;
    Key contract {};
    UINT64 value = 0, ordinal = 0, generation = 0, bytes = 0;
    unsigned write = 0;
    bool pending = false, publish = false, shown = false, submitted = false, quarantined = false;
    bool retirementProven = false, retirementAmbiguous = false;
    bool ordered = false, logicalPublished = false;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> retirementQueue;
};
template <class Record, class Apply>
bool PublishHistoryRecord(Record& slot, bool queueOrdered, UINT64& publishedGeneration, Apply&& apply)
{
    if (!slot.publish || slot.quarantined || !slot.resourceUse || slot.resourceUse->quarantined ||
        slot.logicalPublished || slot.generation <= publishedGeneration) return false;
    publishedGeneration = slot.generation;
    slot.logicalPublished = true;
    apply(slot, queueOrdered);
    return true;
}
template <class Key> class OrderedHistory
{
public:
    using Record = HistoryRecord<Key>;
    Record current;
    std::array<Record, DlssNrOrderedReuse::RetirementCapacity> retained {};
    UINT64 signalValue = 0;
    template <class F> void Each(F&& visit)
    {
        visit(current);
        for (auto& slot : retained) visit(slot);
    }
    unsigned Pending()
    {
        unsigned count = 0;
        Each([&](const Record& slot) { if (slot.pending) ++count; });
        return count;
    }
    static bool Healthy(const Record& slot)
    {
        return !slot.quarantined && !slot.retirementAmbiguous && slot.resourceUse &&
            !slot.resourceUse->quarantined;
    }
    template <class Publish, class Discard> void Collect(Publish&& publish, Discard&& discard)
    {
        Each([&](Record& slot)
        {
            if (!slot.pending || !slot.submitted) return;
            if (!Healthy(slot)) { discard(); return; }
            if (!slot.retirementProven || !slot.fence || !slot.value) return;
            const auto done = slot.fence->GetCompletedValue();
            if (done == UINT64_MAX || slot.resourceUse->DeviceLost())
            {
                slot.quarantined = slot.resourceUse->quarantined = true;
                discard(); return;
            }
            if (done < slot.value || !slot.resourceUse->GpuComplete()) return;
            // GPU completion permits logical publication; it does NOT close an
            // executable generation or free a descriptor/resource owner.
            publish(slot, false);
            if (!slot.resourceUse->Complete()) return;
            slot.resourceUse->RetireOwners();
            auto fence = slot.fence;
            slot = {};
            if (&slot == &current) slot.fence = std::move(fence);
        });
    }
    bool Advance(bool authenticated, ID3D12CommandList* nextList, ID3D12CommandQueue* nextQueue,
                 const Key& nextContract, UINT64 nextOrdinal)
    {
        auto& slot = current;
        DlssNrOrderedReuse::Facts proof;
        proof.localProducer = authenticated && slot.ordered;
        proof.submitted = slot.submitted;
        proof.signalled = slot.retirementProven && slot.fence && slot.value;
        proof.sameQueue = slot.queue == nextQueue && slot.retirementQueue.Get() == nextQueue;
        proof.sameDeviceAndType = SameDevice(slot.commandOwner.Get(), nextQueue) &&
            SameDevice(nextList, nextQueue) && nextQueue && nextList &&
            nextQueue->GetDesc().Type == nextList->GetType();
        proof.healthy = Healthy(slot);
        proof.sameContract = slot.contract == nextContract;
        proof.adjacent = slot.ordinal != UINT64_MAX && slot.ordinal + 1 == nextOrdinal;
        proof.recordingBoundary = slot.commands != nextList;
        Record* free = nullptr;
        for (auto& older : retained)
        {
            if (!older.pending) { if (!free) free = &older; continue; }
            if (!older.ordered || !older.submitted || !older.retirementProven || !Healthy(older) ||
                older.retirementQueue.Get() != nextQueue || older.commands == nextList)
                proof.healthy = false;
        }
        proof.freeRetirement = free != nullptr;
        if (!slot.pending || !DlssNrOrderedReuse::Allows(proof)) return false;
        *free = std::move(slot);
        auto fence = free->fence;
        slot = {};
        slot.fence = std::move(fence);
        return true;
    }
    template <class Publish, class Discard> void Reset(ID3D12CommandList* list,
                                                      Publish&& publish, Discard&& discard)
    {
        Collect(publish, discard);
        Each([&](Record& slot)
        {
            if (!slot.pending || slot.commands != list) return;
            if (!slot.submitted) { slot.pending = false; discard(); }
            slot.commands = nullptr; // old generation can no longer match new Execute
        });
    }
    template <class Publish, class Discard, class Observed>
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
                   Publish&& publish, Discard&& discard, Observed&& observed)
    {
        if (!lists) return;
        Each([&](Record& slot)
        {
            if (!slot.pending) return;
            for (UINT i = 0; i < count; ++i)
            {
                if (lists[i] != slot.commands) continue;
                const bool valid = slot.fence && SameDevice(slot.commands, queue) &&
                    SameDevice(slot.fence.Get(), queue) && queue &&
                    queue->GetDesc().Type == slot.commands->GetType();
                const bool repeated = slot.submitted;
                slot.submitted = true;
                if (repeated && (!slot.retirementProven || slot.retirementQueue.Get() != queue))
                    slot.retirementAmbiguous = true;
                if (!valid || queue != slot.queue || repeated || !Healthy(slot) || signalValue == UINT64_MAX)
                {
                    slot.quarantined = true;
                    if (slot.resourceUse) slot.resourceUse->quarantined = true;
                    discard();
                    observed(slot); break;
                }
                slot.value = ++signalValue;
                slot.retirementQueue = queue;
                HRESULT result = E_FAIL;
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
                if (!(slot.resourceUse->faults && (slot.resourceUse->faults->failSignal ||
                                                  slot.resourceUse->faults->failHistorySignal)))
#endif
                    result = queue->Signal(slot.fence.Get(), slot.value);
                slot.retirementProven = SUCCEEDED(result);
                if (!slot.retirementProven)
                {
                    slot.quarantined = slot.resourceUse->quarantined = true;
                    discard();
                }
                else if (slot.ordered) publish(slot, true); // post-Execute; CPU fence may be pending
                observed(slot); break;
            }
        });
    }
};

// Used by TickNrRetired and the fixture, including the exact shared Uses snapshot.
template <class Entries, class Release> void CollectRetired(Entries& entries, Release&& release)
{
    for (size_t i = 0; i < entries.size();)
    {
        if (!Complete(entries[i].uses)) { ++i; continue; }
        for (const auto& use : entries[i].uses) use->RetireOwners();
        release(entries[i]);
        entries.erase(entries.begin() + i);
    }
}
}
