#pragma once
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <d3d12.h>
#include <optional>
#include <d3dx/d3dx12.h>

#define OS_NUM_OF_HEAPS 2

// Forward declaration so this header need not pull in Config.h. Scaler is a scoped enum with a fixed
// underlying type, so an opaque declaration is a complete type -- enough for a member and a parameter.
enum class Scaler : uint32_t;

class OS_Dx12 : public Shader_Dx12
{
  public:
    static constexpr UINT RecordingSlotCount = 3;

  private:
    bool _upsample = false;

    // Which downscaler this instance was built for. Scaler::Count means "no override -- read the global
    // OutputScalingDownscaler", so the ordinary Output Scaling constructor behaves exactly as before.
    // Neural Rendering passes its own DlssNrScalingDownscaler here so the two pick filters independently.
    Scaler _scalerOverride;
    Scaler ActiveScaler() const;

    FrameDescriptorHeap _frameHeaps[OS_NUM_OF_HEAPS];

    // CPU-written uploads/descriptors and the private intermediate belong to
    // the caller's command slot. Immutable PSO/root signature remain shared.
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

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, uint32_t InWidth, uint32_t InHeight,
                              D3D12_RESOURCE_STATES InState, std::optional<UINT> recordingSlot = std::nullopt);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState, std::optional<UINT> recordingSlot = std::nullopt);
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
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
    bool IsUpsampling() const { return _upsample; }
    bool CanRender(std::optional<UINT> recordingSlot = std::nullopt) const { return _init && Buffer(recordingSlot); }

    OS_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample);
    OS_Dx12(std::string InName, ID3D12Device* InDevice, bool InUpsample, Scaler InScalerOverride);

    ~OS_Dx12();
};
