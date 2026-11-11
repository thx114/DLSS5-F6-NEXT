#pragma once
#include "IFeature_Dx11.h"
#include "Dx11BridgeInitState.h"
#include <runtime/GpuWorkDomain.h>
#include <runtime/GuideSourceMetadata.h>
#include <atomic>
struct Dx11BridgeInitRetention;

#include "IFeature_Dx12.h"

#include <with_dx12/dx11_with_dx12.h>
#include <genshin/GenshinDx12Transport.h>

#include <d3d12.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#define DX11WDX12_COMMAND_BUFFER_COUNT 3

class IFeature_Dx11wDx12 : public virtual IFeature_Dx11
{
  private:
    template <typename F, typename Default> auto CallFeature(F&& f, Default&& def)
    {
        if (auto feature = dx12Feature.get(); feature)
            return f(feature);
        return def;
    }

  protected:
    std::unique_ptr<IFeature_Dx12> dx12Feature = nullptr;
    GuideSourceMetadata::Ptr currentInputMetadata;
    // Count real input attempts, including failures and inputs that skip NR.
    // The successful-output clock is unsuitable for one-frame MV adjacency.
    std::atomic<uint64_t> nrSourceAttempts { 0 };
    Dx11BridgeInit::State bridgeInitState;
    Dx11BridgeInitRetention* initRetention = nullptr;
    void RetainFailedInitialization();
    // Preallocated before bridge submission, including external NGX sources.
    std::unique_ptr<Dx11BridgeInitRetention> genshinRuntimeRetention;
    void RetainGenshinRuntime() noexcept;

    // D3D11
    ID3D11Device5* Dx11Device = nullptr;
    ID3D11DeviceContext4* Dx11DeviceContext = nullptr;

    D3D12_COMMAND_LIST_TYPE Dx12CommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ID3D12CommandQueue* Dx12CommandQueue = nullptr;
    ID3D12CommandAllocator* Dx12CommandAllocator[DX11WDX12_COMMAND_BUFFER_COUNT] {};
    ID3D12GraphicsCommandList* Dx12CommandList[DX11WDX12_COMMAND_BUFFER_COUNT] {};
    ID3D12Fence* Dx12Fence = nullptr;
    HANDLE Dx12FenceEvent = nullptr;
    UINT64 Dx12FenceValue = 0;
    UINT64 genshinLastSubmittedFence = 0; // Set only after a successful queue Signal.
    bool genshinLastSubmissionIsolated = false; // Same successful Signal; transitions drain legacy query/pass aliases.
    bool genshinSubmissionUnfenced = false;
    HRESULT CheckRecordingAvailable(UINT commandFrame, bool isolatedPassResources);
    bool genshinCommandPathFailed = false; // Refuse unsafe allocator reuse after a private transport failure.
    UINT64 Dx12CommandAllocatorFenceValue[DX11WDX12_COMMAND_BUFFER_COUNT] = {};
    // Last recording of each real slot, not an admission/retirement receipt.
    // Weak leases add no GPU ownership and never prevent surface retirement.
    struct GenshinRecordingObservation {
        GenshinTransport::Owner owner{};
        std::weak_ptr<GenshinTransport::Lease> lease;
        uint64_t frame=0;
        const char* phase="unused";
        bool valid=false,providerReturned=false,providerOk=false;
        bool executed=false,executeReturned=false,submitted=false,copyback=false,isolated=false;
        HRESULT resetHr=S_FALSE,closeHr=S_FALSE,submitHr=S_FALSE,signalHr=S_FALSE;
    };
    std::array<GenshinRecordingObservation,DX11WDX12_COMMAND_BUFFER_COUNT> genshinRecordings{};
    bool genshinCommandFailureReported=false,genshinDeviceFailureReported=false;
    std::atomic<bool> interopFenceFaultReported{false},interopDeviceFaultReported{false};
    void ReportInteropFenceFault(const char* phase,UINT slot,UINT64 target,UINT64 completed,DWORD waitResult) noexcept;
    void LogGenshinFailure(const char* phase,HRESULT reason,UINT commandFrame,
                           UINT64 waitTarget=0,bool waitTargetKnown=false) noexcept;

    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C dx11Color = {};
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C dx11Mv = {};
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C dx11Depth = {};
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C dx11Reactive = {};
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C dx11Exp = {};
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C dx11Out = {};

    ID3D11Resource* paramOutput[DX11_WITH_DX12_CACHED_FRAMES] = {};

    // Per-feature Genshin domain; never replaces IFeature's shared static device.
    FramePipeline::GpuWorkDomain genshinWorkDomain;
    ID3D12Device* BridgeWorkDevice() const noexcept {
        return genshinWorkDomain.Creator()?genshinWorkDomain.Creator():_dx11on12Device;
    }
    bool CreateD3D12Objects();
    bool ProcessDx11Textures(const NVSDK_NGX_Parameter* InParameters);
    bool CopyBackOutput();

    void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                         D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState);

    // Returns false when the last submitted bridge work cannot be proven
    // complete. In that case resources stay retained until process teardown.
    // Close the old executable generations through successful Reset after the
    // real queue drain, before releasing the provider and command dependencies.
    // Resource recreation leaves the live provider intact.
    bool ReleaseSharedResources(bool releaseProvider = false);

    bool BaseInit(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters);

  public:
    GuideSourceMetadata::Ptr CurrentInputMetadata() const noexcept { return currentInputMetadata; }
    // dx11 w/dx12 can't be wrapped but the IFeature_Dx11, needs its own Init and Evaluate
    bool InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) final { return false; };
    bool EvaluateInternal(ID3D11DeviceContext* DeviceContext, NVSDK_NGX_Parameter* InParameters) final
    {
        return false;
    };

    bool Init(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) final;
    bool Evaluate(ID3D11DeviceContext* DeviceContext, NVSDK_NGX_Parameter* InParameters) final;
    bool IsWithDx12() final { return true; }
    API Api() const override { return API::DX12; }

    std::optional<double> ReadUpscalerTime(void* deviceContextVoid) override
    {
        if (auto feature = dx12Feature.get(); feature && Dx12CommandQueue)
            return feature->ReadUpscalerTime(Dx12CommandQueue);

        return std::nullopt;
    };
    void ReadDetailedGpuTimes(void* deviceContextVoid, std::vector<DetailedGpuTime>& detailedGpuTimes) override
    {
        if (auto feature = dx12Feature.get(); feature && Dx12CommandQueue)
            return feature->ReadDetailedGpuTimes(Dx12CommandQueue, detailedGpuTimes);
    };

    feature_version Version() final
    {
        return CallFeature([](auto f) { return f->Version(); }, feature_version {});
    }

    size_t JitterCount() override
    {
        return CallFeature([](auto f) { return f->JitterCount(); }, size_t {});
    }

    void TickFrozenCheck(uint32_t presentPerEval = 1) override
    {
        if (auto feature = dx12Feature.get(); feature)
            return feature->TickFrozenCheck(presentPerEval);
    };

    bool IsFrozen() override
    {
        return CallFeature([](auto f) { return f->IsFrozen(); }, bool {});
    };
    bool UpdateOutputResolution(const NVSDK_NGX_Parameter* InParameters) override
    {
        return CallFeature([&](auto f) { return f->UpdateOutputResolution(InParameters); }, bool {});
    };
    uint32_t DisplayWidth() override
    {
        return CallFeature([](auto f) { return f->DisplayWidth(); }, uint32_t {});
    };
    uint32_t DisplayHeight() override
    {
        return CallFeature([](auto f) { return f->DisplayHeight(); }, uint32_t {});
    };
    uint32_t TargetWidth() override
    {
        return CallFeature([](auto f) { return f->TargetWidth(); }, uint32_t {});
    };
    uint32_t TargetHeight() override
    {
        return CallFeature([](auto f) { return f->TargetHeight(); }, uint32_t {});
    };
    uint32_t RenderWidth() override
    {
        return CallFeature([](auto f) { return f->RenderWidth(); }, uint32_t {});
    };
    uint32_t RenderHeight() override
    {
        return CallFeature([](auto f) { return f->RenderHeight(); }, uint32_t {});
    };
    NVSDK_NGX_PerfQuality_Value PerfQualityValue() override
    {
        return CallFeature([](auto f) { return f->PerfQualityValue(); }, NVSDK_NGX_PerfQuality_Value {});
    }
    bool IsInitParameters() override
    {
        return CallFeature([](auto f) { return f->IsInitParameters(); }, bool {});
    };
    bool IsInited() override
    {
        return CallFeature([](auto f) { return f->IsInited(); }, bool {});
    }
    float Sharpness() override
    {
        return CallFeature([](auto f) { return f->Sharpness(); }, float {});
    }
    bool HasColor() override
    {
        return CallFeature([](auto f) { return f->HasColor(); }, bool {});
    }
    bool HasDepth() override
    {
        return CallFeature([](auto f) { return f->HasDepth(); }, bool {});
    }
    bool HasMV() override
    {
        return CallFeature([](auto f) { return f->HasMV(); }, bool {});
    }
    bool HasTM() override
    {
        return CallFeature([](auto f) { return f->HasTM(); }, bool {});
    }
    bool AccessToReactiveMask() override
    {
        return CallFeature([](auto f) { return f->AccessToReactiveMask(); }, bool {});
    }
    bool HasExposure() override
    {
        return CallFeature([](auto f) { return f->HasExposure(); }, bool {});
    }
    bool HasOutput() override
    {
        return CallFeature([](auto f) { return f->HasOutput(); }, bool {});
    }
    bool ModuleLoaded() override
    {
        return CallFeature([](auto f) { return f->ModuleLoaded(); }, bool {});
    }
    long FrameCount() override
    {
        return CallFeature([](auto f) { return f->FrameCount(); }, long {});
    }
    bool DepthLinear() override
    {
        return CallFeature([](auto f) { return f->DepthLinear(); }, bool {});
    }
    bool AutoExposure() override
    {
        return CallFeature([](auto f) { return f->AutoExposure(); }, bool {});
    }
    bool DepthInverted() override
    {
        return CallFeature([](auto f) { return f->DepthInverted(); }, bool {});
    }
    bool IsHdr() override
    {
        return CallFeature([](auto f) { return f->IsHdr(); }, bool {});
    }
    bool JitteredMV() override
    {
        return CallFeature([](auto f) { return f->JitteredMV(); }, bool {});
    }
    bool LowResMV() override
    {
        return CallFeature([](auto f) { return f->LowResMV(); }, bool {});
    }
    bool SharpenEnabled() override
    {
        return CallFeature([](auto f) { return f->SharpenEnabled(); }, bool {});
    }

    IFeature_Dx11wDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~IFeature_Dx11wDx12();
};
