// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DlssNr_RecordingLifetime.h"
#include <utility>

struct NVSDK_NGX_Parameter;

// Caller holds the existing NR mutex. Recording closure and actual queue/fence
// completion remain distinct; neither this owner nor its destructor waits.
class NrGpuExecutionLifetime final
{
public:
    using ReleaseFeature = void (*)(void*);
    using DestroyParameters = void (*)(NVSDK_NGX_Parameter*);
#if defined(DLSSNR_LIFETIME_TEST_SEAM)
    explicit NrGpuExecutionLifetime(DlssNrGpuLifetime::TestFaults* faults = nullptr) : recordings_(faults) {}
#endif
    DlssNrGpuLifetime::Use Recording(ID3D12Device* device, ID3D12CommandList* list) { return recordings_.Recording(device,list); }
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) { recordings_.Submitted(queue,count,lists); }
    void ResetRecording(ID3D12CommandList* list) { recordings_.ResetRecording(list); }
    bool Watches(ID3D12CommandList* list) const { return recordings_.Watches(list); }
    bool CanResetRecording(ID3D12CommandList* list) const { return recordings_.CanResetRecording(list); }
    DlssNrGpuLifetime::Uses Snapshot() { return recordings_.Snapshot(); }
    size_t Tracked() const { return recordings_.Tracked(); }
    bool Halted() const { return recordings_.Halted(); }

    void ParkFeature(void*& feature, ReleaseFeature overrideRelease = nullptr, NVSDK_NGX_Parameter** parameters = nullptr);
    void ParkResource(ID3D12Resource*& resource);
    void CollectCompleted(ReleaseFeature currentDefault, DestroyParameters destroyParameters);
    bool Empty() const noexcept { return retired_.empty(); }
    size_t RetiredCount() const noexcept { return retired_.size(); }
    bool ContainsRetiredResource(ID3D12Resource* resource) const noexcept;
    template<class Visit> void VisitRetiredResources(Visit&& visit) const
    {
        for (const auto& entry : retired_) std::forward<Visit>(visit)(entry.resource);
    }

private:
    struct Retired
    {
        void* feature = nullptr;
        ID3D12Resource* resource = nullptr;
        ReleaseFeature release = nullptr;
        NVSDK_NGX_Parameter* parameters = nullptr;
        DlssNrGpuLifetime::Uses uses;
    };
    DlssNrGpuLifetime::Registry recordings_;
    // Raw ownership is deliberately retained on unknown/failed retirement.
    // Destruction must never unconditionally free GPU-visible allocations.
    std::vector<Retired> retired_;
};
