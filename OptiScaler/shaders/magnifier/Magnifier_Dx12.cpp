#include "pch.h"
#include "Magnifier_Dx12.h"

#include <Config.h>
#include <State.h>
#include "precompile/Magnifier_Shader.h"

void Magnifier_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                     D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

bool Magnifier_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource,
                              ID3D12Resource* OutResource, std::optional<UINT> recordingSlot)
{
    if ((recordingSlot && *recordingSlot >= RecordingSlotCount) ||
        !_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList, recordingSlot);

    FrameDescriptorHeap* selectedHeap = nullptr;
    ID3D12Resource* constantBuffer = _constantBuffer;
    if (recordingSlot)
    {
        auto& slot = _recordingSlots[*recordingSlot];
        if (!slot.constantBuffer)
        {
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(InternalMagnifierParams));
            const auto props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&slot.constantBuffer)))) return false;
        }
        if (!slot.heap.GetHeapCSU() && !InitHeaps(_device, &slot.heap, 1)) return false;
        selectedHeap = &slot.heap;
        constantBuffer = slot.constantBuffer;
    }
    else
    {
        _counter++;
        _counter = _counter % Magnifier_NUM_OF_HEAPS;
        selectedHeap = &_frameHeaps[_counter];
    }
    FrameDescriptorHeap& currentHeap = *selectedHeap;

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    InternalMagnifierParams constants {};

    auto outDesc = OutResource->GetDesc();
    FilloutStruct((float) outDesc.Width, (float) outDesc.Height, constants);

    if (!CreateConstantsBuffer(_device, constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    auto inDesc = InResource->GetDesc();
    UINT dispatchWidth = static_cast<UINT>((inDesc.Width + InNumThreadsX - 1) / InNumThreadsX);
    UINT dispatchHeight = (inDesc.Height + InNumThreadsY - 1) / InNumThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

void Magnifier_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState,
                                 std::optional<UINT> recordingSlot)
{
    if (recordingSlot)
    {
        if (*recordingSlot >= RecordingSlotCount) return;
        auto& slot = _recordingSlots[*recordingSlot];
        if (slot.buffer) Shader_Dx12::SetBufferState(InCommandList, InState, slot.buffer, &slot.bufferState);
        return;
    }
    return Shader_Dx12::SetBufferState(InCommandList, InState, _buffer, &_bufferState);
}

bool Magnifier_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource,
                                          D3D12_RESOURCE_STATES InState, std::optional<UINT> recordingSlot)
{
    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (recordingSlot)
    {
        if (*recordingSlot >= RecordingSlotCount || !InDevice || !InSource)
            return false;
        auto& slot = _recordingSlots[*recordingSlot];
        auto desc = InSource->GetDesc();
        const auto old = slot.buffer ? slot.buffer->GetDesc() : D3D12_RESOURCE_DESC {};
        const bool reuse = slot.buffer && old.Width == desc.Width && old.Height == desc.Height && old.Format == desc.Format;
        const bool created = Shader_Dx12::CreateBufferResource(InDevice, InSource, InState, &slot.buffer,
                                                               resourceFlags);
        if (created && !reuse)
        {
            slot.buffer->SetName(L"Magnifier_Buffer_RecordingSlot");
            slot.bufferState = InState;
            slot.unsubmittedState = InState; // New resource physically starts here, even if recording is discarded.
        }
        return created;
    }

    auto result = Shader_Dx12::CreateBufferResource(InDevice, InSource, InState, &_buffer, resourceFlags);

    if (result)
    {
        _buffer->SetName(L"Magnifier_Buffer");
        _bufferState = InState;
    }

    return result;
}

Magnifier_Dx12::Magnifier_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, 1, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(InternalMagnifierParams));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, Magnifier_cso, sizeof(Magnifier_cso), shaderCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, Magnifier_NUM_OF_HEAPS);
}

Magnifier_Dx12::~Magnifier_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;
    for (auto& slot : _recordingSlots)
    {
        SAFE_RELEASE(slot.constantBuffer);
        SAFE_RELEASE(slot.buffer);
        slot.heap.ReleaseHeaps();
    }
    if (!_init) return;

    for (int i = 0; i < Magnifier_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }

    SAFE_RELEASE(_buffer);
}
