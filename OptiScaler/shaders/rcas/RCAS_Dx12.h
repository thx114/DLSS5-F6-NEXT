#pragma once
#include "RCAS_Common.h"

#include <d3d12.h>
#include <optional>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#define RCAS_NUM_OF_HEAPS 2

class RCAS_Dx12 : public Shader_Dx12, public RCAS_Common
{
  public:
    static constexpr UINT RecordingSlotCount = 3;

  private:
    // Un-slotted callers retain their two-heap rotation and inherited single CB.
    FrameDescriptorHeap _frameHeaps[RCAS_NUM_OF_HEAPS];

    struct RecordingSlotResources
    {
        FrameDescriptorHeap heap;
        ID3D12Resource* constantBuffer = nullptr;
        ID3D12Resource* buffer = nullptr;
        D3D12_RESOURCE_STATES bufferState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES unsubmittedState = D3D12_RESOURCE_STATE_COMMON;
        bool recording = false;
    };
    RecordingSlotResources _recordingSlots[RecordingSlotCount];

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    ID3D12PipelineState* _pipelineStateDA = nullptr;
    ID3D12PipelineState* _pipelineStateDASDA = nullptr;

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

    bool DispatchRCAS(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* InMotionVectors,
                      RcasConstants InConstants, ID3D12Resource* OutResource, FrameDescriptorHeap& currentHeap,
                      ID3D12Resource* constantBuffer);
    bool DispatchDepthAdaptive(ID3D12PipelineState* pipelineState, ID3D12GraphicsCommandList* InCmdList,
                               ID3D12Resource* InResource, ID3D12Resource* InMotionVectors, ID3D12Resource* InDepth,
                               RcasConstants InConstants, ID3D12Resource* OutResource,
                               FrameDescriptorHeap& currentHeap, ID3D12Resource* constantBuffer);

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState, std::optional<UINT> recordingSlot = std::nullopt);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState, std::optional<UINT> recordingSlot = std::nullopt);
    // An explicit slot is an identity supplied by the recording owner, never a
    // dispatch counter. The owner must complete its existing fence wait first.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* InMotionVectors,
                  RcasConstants InConstants, ID3D12Resource* OutResource, ID3D12Resource* InDepth = nullptr,
                  std::optional<UINT> recordingSlot = std::nullopt);

    // Called by the Bridge owner after slot admission. Failed/unsubmitted
    // recordings must not publish transitions that the GPU never executed.
    void BeginRecordingSlot(UINT index) noexcept
    {
        if (index >= RecordingSlotCount) return;
        auto& slot = _recordingSlots[index];
        slot.unsubmittedState = slot.bufferState;
        slot.recording = true;
    }
    void FinishRecordingSlot(UINT index, bool executed) noexcept
    {
        if (index >= RecordingSlotCount) return;
        auto& slot = _recordingSlots[index];
        if (slot.recording && !executed) slot.bufferState = slot.unsubmittedState;
        slot.recording = false;
    }

    ID3D12Resource* Buffer(std::optional<UINT> recordingSlot = std::nullopt) const
    {
        return recordingSlot ? (*recordingSlot < RecordingSlotCount ? _recordingSlots[*recordingSlot].buffer : nullptr)
                             : _buffer;
    }
    bool CanRender(std::optional<UINT> recordingSlot = std::nullopt) const { return _init && Buffer(recordingSlot); }

    RCAS_Dx12(std::string InName, ID3D12Device* InDevice);

    ~RCAS_Dx12();
};
