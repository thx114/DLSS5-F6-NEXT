#include "pch.h"
#include "RF_Dx12.h"

#include "RF_Common.h"

#include <Config.h>
#include <State.h>

#include "precompiled/RF_Shader.h"

#ifndef RF_SOURCE_FNV1A
#error Regenerate resource_flip/precompiled/RF_Shader.h with Generate-RFShader.ps1 before building.
#else
static_assert(RFSourceHash() == RF_SOURCE_FNV1A, "RF bytecode is stale; run Generate-RFShader.ps1");
#endif

bool RF_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                       UINT64 width, UINT height, bool velocity, bool flipY, bool invertY)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    const auto inDesc = InResource->GetDesc();
    const auto outDesc = OutResource->GetDesc();
    RFRegion region {};
    if (InResource == OutResource || inDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        outDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || inDesc.DepthOrArraySize != 1 ||
        outDesc.DepthOrArraySize != 1 || inDesc.SampleDesc.Count != 1 || outDesc.SampleDesc.Count != 1 ||
        (inDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        (outDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 ||
        !TryRFRegion(width, height, inDesc.Width, inDesc.Height, outDesc.Width, outDesc.Height,
                     flipY && Config::Instance()->FGResourceFlipOffset.value_or_default(), region))
    {
        LOG_WARN("[{}] Invalid resource flip region: {}x{}", _name, width, height);
        return false;
    }

    LOG_DEBUG("[{0}] Start!", _name);

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList);

    _counter++;
    _counter = _counter % RF_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    RFConstants constants {};

    constants.height = region.height;
    constants.width = region.width;
    constants.offset = region.offset;
    constants.velocity = velocity ? 1 : 0;
    constants.rowFlip = flipY ? 1 : 0;
    constants.negateMotionY = velocity && invertY ? 1 : 0;

    LOG_DEBUG("Width: {}, Height: {}, Offset: {}, RowFlip: {}, NegateMotionY: {}", constants.width,
              constants.height, constants.offset, constants.rowFlip, constants.negateMotionY);

    if (!CreateConstantsBuffer(_device, _constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    InCmdList->Dispatch(region.groupsX, region.groupsY, 1);

    return true;
}

RF_Dx12::RF_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
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

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RFConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, RF_cso, sizeof(RF_cso), rfCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, RF_NUM_OF_HEAPS);
}

RF_Dx12::~RF_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < RF_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }
}
