#include "pch.h"
#include <native/OpticalFgNrPolicy.h>
#include "Upscaler_Inputs_Dx11wDx12.h"

#include "UpscalerFrameParameters.h"
#include <runtime/F6InputPolicy.h>
#include <framegen/dlssg/FgDepthContract.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
#include <upscalers/IFeature_Dx11wDx12.h>
#include <Util.h>

#include <with_dx12/with_dx12.h>
#include "shaders/depth_scale/DS_Dx12.h"

static DS_Dx12* DepthScaleDx11wDx12 = nullptr;

static bool PrepareFgResourceCache(NVSDK_NGX_Parameter* parameters, UINT64 frameKey)
{
    if (parameters == nullptr)
        return false;

    const auto mask = Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth;
    const auto dontUseNtShared = Config::Instance()->DontUseNTShared.value_or_default();
    const auto frameIndex = Dx11WithDx12::GetUpscalerFrameIndex();

    const auto result =
        Dx11WithDx12::PrepareUpscalerResources(parameters, mask, frameIndex, frameKey, dontUseNtShared, false, true);

    if (!result.Success)
    {
        LOG_ERROR("Dx11wDx12 FG input cache preparation failed");
        return false;
    }

    return true;
}

static bool ReusePreparedUpscalerCacheForFg(UINT64 frameKey)
{
    const auto mask = Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth;
    const auto resolvedFrameKey = frameKey != 0 ? frameKey : Dx11WithDx12::GetLastPreparedUpscalerFrameId();

    if (!Dx11WithDx12::HasPreparedUpscalerResources(mask, resolvedFrameKey))
    {
        LOG_WARN("Dx11wDx12 FG input cache miss");
        return false;
    }

    return true;
}

void UpscalerInputsDx11wDx12::Init(ID3D11Device* dx11Device, ID3D11DeviceContext* dx11Context, ID3D12Device* dx12Device,
                                   ID3D12CommandQueue* dx12CommandQueue)
{
    if (State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    if (dx12Device != nullptr && dx12CommandQueue != nullptr && !WithDx12::IsInited())
        WithDx12::SetD3D12Objects(dx12Device, dx12CommandQueue, D3D12_COMMAND_LIST_TYPE_DIRECT);

    if (!WithDx12::IsInited() && !WithDx12::PrepareD3D12ForD3D11(dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        LOG_ERROR("UpscalerInputsDx11wDx12::Init failed to resolve D3D12 device/queue");
        return;
    }

    _dx12Device = WithDx12::GetD3D12Device();
    _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();

    Dx11WithDx12::Init(dx11Device, dx11Context);
}

void UpscalerInputsDx11wDx12::Reset() {}

void UpscalerInputsDx11wDx12::UpscaleStart(NVSDK_NGX_Parameter* InParameters, IFeature_Dx11* feature)
{
    if (native::OpticalFgNrEnabled()) return;

    if (InParameters == nullptr || feature == nullptr)
        return;
    if (F6InputPolicy::FsrBridgeEnabled && dlssg::depth_contract::Game()==GameRouting::Identity::Genshin &&
        State::Instance().activeFgInput==FGInput::Upscaler &&
        State::Instance().activeFgOutput==FGOutput::DLSSG) return; // Sealed bridge remains independent.

    auto& cfg = *Config::Instance();
    const auto frame = fg_inputs::ReadFrameParameters(
        *InParameters, *feature, cfg, State::Instance().lastFGFrameTime,
        { OptiKeys::FSR_NearPlane, OptiKeys::FSR_FarPlane, OptiKeys::FSR_CameraFovVertical,
          OptiKeys::FSR_ViewSpaceToMetersFactor });
    State::Instance().lastFsrCameraFar = frame.cameraFar.value;
    State::Instance().lastFsrCameraNear = frame.cameraNear.value;

    if (State::Instance().activeFgInput != FGInput::Upscaler || _dx12Device == nullptr)
        return;

    // The interop adapter must prepare/reuse the actual source cache before
    // EvaluateState/NewFrame. Common scalar parameters cannot certify it.
    const auto prepareInputs = [&]() {
        if (feature->IsWithDx12())
            return ReusePreparedUpscalerCacheForFg(Dx11WithDx12::GetLastPreparedUpscalerFrameId());
        if (PrepareFgResourceCache(InParameters, Dx11WithDx12::NextUpscalerFrameId())) return true;
        LOG_ERROR("Dx11wDx12 FG input cache preparation failed");
        return false;
    };
    if (!prepareInputs())
    {
        if (auto* own=dynamic_cast<DLSSG_Dx12*>(State::Instance().currentFG))
            own->RejectUpscalerGuideSource("selected-sr-cache-not-prepared");
        return;
    }

    auto fg = State::Instance().currentFG;

    if (fg == nullptr)
        return;

    auto fgConstants = fg_inputs::MakeFgConstants<FG_Constants, FG_Flags>(frame);

    fg->EvaluateState(_dx12Device, fgConstants);

    if (fg->Mutex.getOwner() == 2)
    {
        LOG_TRACE("Waiting for present!");
        fg->Mutex.lock(4);
        fg->Mutex.unlockThis(4);
    }

    auto inputExecution = fg->GuardExecution(FGExecutionScope::Inputs);
    if (!fg->CanSubmitInputs()) return;
    const auto previousFrame = fg->FrameCount();
    const auto startedFrame = fg->StartNewFrame();
    if (!fg->CanSubmitInputs() || startedFrame == previousFrame) return;

    fg_inputs::ApplyFrameParameters(*fg, frame);

    uintptr_t evaluatedOutput=0;
    if (auto* own=dynamic_cast<DLSSG_Dx12*>(fg); own && own->OwnsUpscalerDepthContract())
    {
        GuideDiagnostics::Resource colour;
        auto* output=fg_inputs::ReadResource<ID3D11Resource>(*InParameters,NVSDK_NGX_Parameter_Output);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (output && SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&texture))))
        {
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            colour={true,static_cast<UINT>(desc.Format),desc.Width,desc.Height,{0,0,desc.Width,desc.Height}};
            evaluatedOutput=reinterpret_cast<uintptr_t>(texture.Get());
        }
        // This callback is after the real successful DX11 Evaluate. The cache
        // preparation ordinal is independent of provider/Reflex/SDK frame IDs.
        auto* bridge=dynamic_cast<IFeature_Dx11wDx12*>(feature);
        auto raw=bridge?bridge->CurrentInputMetadata():GuideSourceMetadata::Ptr{};
        // The existing cache validation below authenticates this live call's
        // resources. Metadata cannot grant the cache or recording admission.
        if(raw) {
            const auto* depth=fg_inputs::ReadResource<ID3D11Resource>(*InParameters,NVSDK_NGX_Parameter_Depth);
            const auto* motion=fg_inputs::ReadResource<ID3D11Resource>(*InParameters,NVSDK_NGX_Parameter_MotionVectors);
            if(raw->inputDepth!=reinterpret_cast<uintptr_t>(depth)||
                raw->inputMotion!=reinterpret_cast<uintptr_t>(motion))raw.reset();
        }
        own->ObserveUpscalerGuideSource(static_cast<IFeature*>(feature),InParameters,
            Dx11WithDx12::GetLastPreparedUpscalerFrameId(),colour,std::move(raw));
    }

    if (State::Instance().isShuttingDown || !fg->IsActive() || !Config::Instance()->FGEnabled.value_or_default() ||
        State::Instance().currentSwapchain == nullptr)
    {
        return;
    }

    auto& cache = Dx11WithDx12::GetUpscalerResourceCache();
    const auto frameIndex = fg->GetIndex();

    LOG_DEBUG("(FG Dx11wDx12) using cached inputs for fgUpscaledImage[{}], frame: {}", frameIndex, fg->FrameCount());

    ID3D12Resource* paramVelocity = cache.Mv.Dx12Resource;
    ID3D12Resource* paramDepth = cache.Depth.Dx12Resource;

    auto cmdList = fg->GetUICommandList();
    if (!cmdList) return; // Never send a null recording to DS or SL.

    if (auto* own=dynamic_cast<DLSSG_Dx12*>(fg); own && own->OwnsUpscalerDepthContract())
    {
        // Resolve only live current NGX resources to the same interface saved
        // by PrepareTextureFrom11To12. Never dereference an old SourceTexture.
        auto* sourceDepth = fg_inputs::ReadResource<ID3D11Resource>(*InParameters, NVSDK_NGX_Parameter_Depth);
        auto* sourceMotion = fg_inputs::ReadResource<ID3D11Resource>(*InParameters, NVSDK_NGX_Parameter_MotionVectors);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceDepthTexture,sourceMotionTexture;
        if (sourceDepth) sourceDepth->QueryInterface(IID_PPV_ARGS(&sourceDepthTexture));
        if (sourceMotion) sourceMotion->QueryInterface(IID_PPV_ARGS(&sourceMotionTexture));
        const auto prepared=Dx11WithDx12::GetLastPreparedUpscalerFrameId();
        if (!prepared || cache.frameId!=prepared || cache.Depth.LastPreparedFrame!=prepared ||
            cache.Mv.LastPreparedFrame!=prepared ||
            !sourceDepthTexture || !sourceMotionTexture ||
            sourceDepthTexture.Get()!=cache.Depth.SourceTexture || sourceMotionTexture.Get()!=cache.Mv.SourceTexture)
        {
            own->RejectUpscalerGuideSource("selected-sr-cache-source-mismatch");
            return;
        }
        const bool tagged=own->SetUpscalerGuidePair(static_cast<IFeature*>(feature),cmdList,InParameters,paramDepth,paramVelocity,
            feature->RenderWidth(),feature->RenderHeight(),
            feature->LowResMV()?feature->RenderWidth():feature->TargetWidth(),
            feature->LowResMV()?feature->RenderHeight():feature->TargetHeight(),
            (D3D12_RESOURCE_STATES)cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            (D3D12_RESOURCE_STATES)cfg.MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            evaluatedOutput);
        if (dlssg::depth_contract::Game()==GameRouting::Identity::Genshin)
            own->SealDx11GuideInputs(tagged,_dx12CommandQueue);
        return;
    }

    if (paramVelocity != nullptr)
    {
        Dx12Resource setResource {};
        setResource.type = FG_ResourceType::Velocity;
        setResource.cmdList = cmdList;
        setResource.resource = paramVelocity;
        setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->MVResourceBarrier.value_or(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        setResource.validity = FG_ResourceValidity::ValidNow;

        if (feature->LowResMV())
        {
            setResource.width = feature->RenderWidth();
            setResource.height = feature->RenderHeight();
        }
        else
        {
            setResource.width = feature->TargetWidth();
            setResource.height = feature->TargetHeight();
        }

        fg->SetResource(&setResource);
    }

    if (paramDepth != nullptr)
    {
        auto done = false;

        if (Config::Instance()->FGEnableDepthScale.value_or_default())
        {
            if (DepthScaleDx11wDx12 == nullptr)
                DepthScaleDx11wDx12 = new DS_Dx12("Depth Scale Dx11wDx12", _dx12Device);

            if (DepthScaleDx11wDx12->CreateBufferResource(_dx12Device, paramDepth, feature->DisplayWidth(),
                                                          feature->DisplayHeight(),
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                DepthScaleDx11wDx12->Buffer() != nullptr)
            {
                DepthScaleDx11wDx12->SetBufferState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (DepthScaleDx11wDx12->Dispatch(cmdList, paramDepth, DepthScaleDx11wDx12->Buffer()))
                {
                    Dx12Resource setResource {};
                    setResource.type = FG_ResourceType::Depth;
                    setResource.cmdList = cmdList;
                    setResource.resource = DepthScaleDx11wDx12->Buffer();
                    setResource.width = feature->RenderWidth();
                    setResource.height = feature->RenderHeight();
                    setResource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    setResource.validity = FG_ResourceValidity::UntilPresent;

                    fg->SetResource(&setResource);
                    done = true;
                }
            }
        }

        if (!done)
        {
            Dx12Resource setResource {};
            setResource.type = FG_ResourceType::Depth;
            setResource.cmdList = cmdList;
            setResource.resource = paramDepth;
            setResource.width = feature->RenderWidth();
            setResource.height = feature->RenderHeight();
            setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->DepthResourceBarrier.value_or(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            setResource.validity = FG_ResourceValidity::ValidNow;

            fg->SetResource(&setResource);
        }
    }

    LOG_DEBUG("(FG Dx11wDx12) D3D11 input preparation done, frame: {}", fg->FrameCount());
}

void UpscalerInputsDx11wDx12::UpscaleEnd(NVSDK_NGX_Parameter* InParameters, IFeature_Dx11* feature)
{
    if (native::OpticalFgNrEnabled()) return;

    if (InParameters == nullptr || feature == nullptr)
        return;

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _dx12Device == nullptr)
        return;

    if (fg->IsActive() && Config::Instance()->FGEnabled.value_or_default() &&
        State::Instance().currentSwapchain != nullptr)
        LOG_DEBUG("(FG Dx11wDx12) running, frame: {}", feature->FrameCount());
}
