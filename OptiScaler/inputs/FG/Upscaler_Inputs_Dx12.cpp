#include "pch.h"
#include <native/OpticalFgNrPolicy.h>
#include "Upscaler_Inputs_Dx12.h"
#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>
#include "shaders/depth_scale/DS_Dx12.h"
#include "MathUtils.h"
#include <framegen/dlssg/FgDepthContract.h>
#include <framegen/dlssg/DLSSG_Dx12.h>

using namespace OptiMath;
static DS_Dx12* DepthScale = nullptr;

void UpscalerInputsDx12::Init(ID3D12Device* device)
{
    if (State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    _device = device;
}

void UpscalerInputsDx12::Reset()
{
    if (State::Instance().currentFG == nullptr || State::Instance().activeFgInput != FGInput::Upscaler ||
        _device == nullptr)
        return;

    if (Config::Instance()->FGHUDFix.value_or_default())
        Hudfix_Dx12::ResetCounters();
}

dlssg::NativeSource UpscalerInputsDx12::UpscaleStart(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                                      IFeature_Dx12* feature)
{
    if (!InCmdList || !InParameters || !feature) return {};
    if (dlssg::depth_contract::Game()==GameRouting::Identity::Genshin &&
        State::Instance().activeFgInput==FGInput::Upscaler &&
        State::Instance().activeFgOutput==FGOutput::DLSSG) return {}; // Sealed Present adapter only.
    if (native::OpticalFgNrEnabled() && feature->GetUpscalerType() != Upscaler::Optical) return {};

    Hudfix_Dx12::SetSkipStatus(true);

    // FSR Camera values
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraVFov = 0.0f;
    float meterFactor = 0.0f;
    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    float tempCameraNear = 0.0f;
    float tempCameraFar = 0.0f;

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    const auto& ngxParams = *InParameters;

    ngxParams.Get(OptiKeys::FSR_NearPlane, &tempCameraNear);
    ngxParams.Get(OptiKeys::FSR_FarPlane, &tempCameraFar);

    if (!cfg.FsrUseFsrInputValues.value_or_default() || (tempCameraNear == 0.0f && tempCameraFar == 0.0f))
    {
        if (feature->DepthInverted())
        {
            cameraFar = cfg.FsrCameraNear.value_or_default();
            cameraNear = cfg.FsrCameraFar.value_or_default();
        }
        else
        {
            cameraFar = cfg.FsrCameraFar.value_or_default();
            cameraNear = cfg.FsrCameraNear.value_or_default();
        }
    }
    else
    {
        cameraNear = tempCameraNear;
        cameraFar = tempCameraFar;
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default() ||
        ngxParams.Get(OptiKeys::FSR_CameraFovVertical, &cameraVFov) != NVSDK_NGX_Result_Success)
    {
        if (cfg.FsrVerticalFov.has_value())
            cameraVFov = GetRadiansFromDeg(cfg.FsrVerticalFov.value());
        else if (cfg.FsrHorizontalFov.value_or_default() > 0.0f)
        {
            const float hFovRad = GetRadiansFromDeg(cfg.FsrHorizontalFov.value());
            cameraVFov =
                GetVerticalFovFromHorizontal(hFovRad, (float) feature->TargetWidth(), (float) feature->TargetHeight());
        }
        else
            cameraVFov = GetRadiansFromDeg(60);
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default())
        ngxParams.Get(OptiKeys::FSR_ViewSpaceToMetersFactor, &meterFactor);

    State::Instance().lastFsrCameraFar = cameraFar;
    State::Instance().lastFsrCameraNear = cameraNear;

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _device == nullptr)
        return {};

    FG_Constants fgConstants {};
    fgConstants.displayWidth = feature->DisplayWidth();
    fgConstants.displayHeight = feature->DisplayHeight();

    if (feature->IsHdr())
        fgConstants.flags |= FG_Flags::Hdr;

    if (feature->DepthInverted())
        fgConstants.flags |= FG_Flags::InvertedDepth;

    if (feature->JitteredMV())
        fgConstants.flags |= FG_Flags::JitteredMVs;

    if (!feature->LowResMV())
        fgConstants.flags |= FG_Flags::DisplayResolutionMVs;

    if (Config::Instance()->FGAsync.value_or_default())
        fgConstants.flags |= FG_Flags::Async;

    auto* own=dynamic_cast<DLSSG_Dx12*>(fg);
    const bool nativeZzz=own && own->OwnsNativeZzzSource();
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> selectedList;
    Microsoft::WRL::ComPtr<IUnknown> selectedIdentity;
    if (nativeZzz)
    {
        selectedList=dlssg::depth_contract::NativeObject(InCmdList);
        if (selectedList) selectedList.As(&selectedIdentity);
    }
    const auto selectedFeatureId=dlssg::zzz::SelectedFeatureIdentity<IFeature>(feature);
    dlssg::zzz::Selection selectedEntry(
        dlssg::zzz::gameApi.load(std::memory_order_acquire),static_cast<uint32_t>(sl::kFeatureDLSS),
        nativeZzz?own->NativeZzzViewport():0,reinterpret_cast<uintptr_t>(selectedIdentity.Get()),
        selectedFeatureId,reinterpret_cast<uintptr_t>(own));
    if (nativeZzz && !dlssg::zzz::Selection::Current(reinterpret_cast<uintptr_t>(selectedIdentity.Get()),
        selectedFeatureId,own->NativeZzzViewport(),reinterpret_cast<uintptr_t>(own)))
    {
        static std::atomic<unsigned> rows { 0 };
        if (rows.fetch_add(1,std::memory_order_relaxed)<8)
            LOG_WARN("F6 ZZZ selected frame refused: reason=no-matching-synchronous-game-DLSS-scope list={} feature={} viewport={} thread={} async_cross_thread=unsupported",
                reinterpret_cast<uintptr_t>(selectedIdentity.Get()),selectedFeatureId,
                own->NativeZzzViewport(),GetCurrentThreadId());
        own->RejectUpscalerGuideSource("native-frame-scope-missing-or-mismatched");
        return {};
    }
    if (!nativeZzz) fg->EvaluateState(_device, fgConstants);

    int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    const bool checkedSource=own && own->OwnsUpscalerDepthContract();
    const bool checkedZzz=checkedSource &&
        dlssg::depth_contract::Game()==GameRouting::Identity::ZenlessZoneZero;
    if (checkedZzz && !nativeZzz && fg->Mutex.getOwner()==2)
    {
        fg->Mutex.lock(4);
        fg->Mutex.unlockThis(4);
    }
    auto inputExecution=nativeZzz ? own->BeginNativeUpscalerInputs(_device,fgConstants) :
        checkedZzz ? fg->GuardExecution(FGExecutionScope::Inputs) : FGExecutionGuard{};
    if (checkedZzz && (!fg->CanSubmitInputs() || (nativeZzz && !own->OwnsNativeZzzSource()))) return {};
    const auto previousFrame=fg->FrameCount();
    const auto startedFrame=fg->StartNewFrame();
    if (checkedZzz && (!fg->CanSubmitInputs() || startedFrame==previousFrame)) return {};

    auto aspectRatio = (float) feature->DisplayWidth() / (float) feature->DisplayHeight();
    fg->SetCameraValues(cameraNear, cameraFar, cameraVFov, aspectRatio, meterFactor);
    fg->SetFrameTimeDelta(State::Instance().lastFGFrameTime);
    fg->SetMVScale(mvScaleX, mvScaleY);
    fg->SetJitter(jitterX, jitterY);
    fg->SetReset(reset);
    fg->SetInterpolationRect(feature->DisplayWidth(), feature->DisplayHeight());

    Hudfix_Dx12::UpscaleStart();

    // FG Prepare
    UINT frameIndex;
    if (!State::Instance().isShuttingDown && fg->IsActive() && Config::Instance()->FGEnabled.value_or_default() &&
        State::Instance().currentSwapchain != nullptr)
    {
        // Wait for present
        if (!checkedZzz && fg->Mutex.getOwner() == 2)
        {
            LOG_TRACE("Waiting for present!");
            fg->Mutex.lock(4);
            fg->Mutex.unlockThis(4);
        }

        bool allocatorReset = false;
        frameIndex = fg->GetIndex();

        ID3D12GraphicsCommandList* commandList = nullptr;
        commandList = InCmdList;

        LOG_DEBUG("(FG) copy buffers for fgUpscaledImage[{}], frame: {}", frameIndex, fg->FrameCount());

        ID3D12Resource* paramVelocity = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramVelocity) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramVelocity);

        if (checkedSource)
        {
            ID3D12Resource* realDepth=nullptr;
            if (InParameters->Get(NVSDK_NGX_Parameter_Depth,&realDepth)!=NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Depth,(void**)&realDepth);
            const bool published=own->SetUpscalerGuidePair(static_cast<IFeature*>(feature),commandList,InParameters,realDepth,paramVelocity,
                feature->RenderWidth(),feature->RenderHeight(),
                feature->LowResMV()?feature->RenderWidth():feature->TargetWidth(),
                feature->LowResMV()?feature->RenderHeight():feature->TargetHeight(),
                (D3D12_RESOURCE_STATES)cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                (D3D12_RESOURCE_STATES)cfg.MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            // Mint under the same Inputs guard as StartNewFrame and both tags.
            // NGX must carry this receipt across Evaluate, not re-query a later slot.
            return published && own->OwnsNativeZzzSource() ? own->NativeGuideSource(InCmdList) : dlssg::NativeSource{};
        }

        if (paramVelocity != nullptr)
        {
            Dx12Resource setResource {};
            setResource.type = FG_ResourceType::Velocity;
            setResource.cmdList = commandList;
            setResource.resource = paramVelocity;
            setResource.state = feature->GetUpscalerType() == Upscaler::Optical ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE :
                    (D3D12_RESOURCE_STATES) Config::Instance()->MVResourceBarrier.value_or(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            setResource.validity = feature->GetUpscalerType() == Upscaler::Optical ?
                    FG_ResourceValidity::ValidButMakeCopy : FG_ResourceValidity::ValidNow;

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

        ID3D12Resource* paramDepth = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

        if (paramDepth != nullptr)
        {
            auto done = false;

            if (feature->GetUpscalerType() != Upscaler::Optical && Config::Instance()->FGEnableDepthScale.value_or_default())
            {
                if (DepthScale == nullptr)
                    DepthScale = new DS_Dx12("Depth Scale", _device);

                if (DepthScale->CreateBufferResource(_device, paramDepth, feature->DisplayWidth(),
                                                     feature->DisplayHeight(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                    DepthScale->Buffer() != nullptr)
                {
                    DepthScale->SetBufferState(InCmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    if (DepthScale->Dispatch(InCmdList, paramDepth, DepthScale->Buffer()))
                    {
                        Dx12Resource setResource {};
                        setResource.type = FG_ResourceType::Depth;
                        setResource.cmdList = commandList;
                        setResource.resource = DepthScale->Buffer();
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
                setResource.cmdList = commandList;
                setResource.resource = paramDepth;
                setResource.width = feature->RenderWidth();
                setResource.height = feature->RenderHeight();
                setResource.state = feature->GetUpscalerType() == Upscaler::Optical ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE :
                    (D3D12_RESOURCE_STATES) Config::Instance()->DepthResourceBarrier.value_or(
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                setResource.validity = feature->GetUpscalerType() == Upscaler::Optical ?
                    FG_ResourceValidity::ValidButMakeCopy : FG_ResourceValidity::ValidNow;

                fg->SetResource(&setResource);
            }
        }

        LOG_DEBUG("(FG) copy buffers done, frame: {0}", fg->FrameCount());
    }
    return {};
}

void UpscalerInputsDx12::UpscaleEnd(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                                    IFeature_Dx12* feature)
{
    if (native::OpticalFgNrEnabled() && feature->GetUpscalerType() != Upscaler::Optical) return;

    Hudfix_Dx12::SetSkipStatus(false);
    if (feature->GetUpscalerType() == Upscaler::Optical) return; // final picture includes HUD; no raw HUDLess tag

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _device == nullptr)
        return;

    // FG Dispatch
    if (fg->IsActive() && Config::Instance()->FGEnabled.value_or_default() &&
        State::Instance().currentSwapchain != nullptr)
    {
        if (Config::Instance()->FGHUDFix.value_or_default())
        {
            // For signal after mv & depth copies
            Hudfix_Dx12::UpscaleEnd(feature->FrameCount(), State::Instance().lastFGFrameTime);

            ID3D12Resource* output = nullptr;
            if (InParameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &output);

            ResourceInfo info {};
            auto desc = output->GetDesc();
            info.buffer = output;
            info.width = desc.Width;
            info.height = desc.Height;
            info.format = desc.Format;
            info.flags = desc.Flags;
            info.type = UAV;
            info.captureInfo = CaptureInfo::Upscaler;

            Hudfix_Dx12::CheckForHudless(InCmdList, &info,
                                         (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value_or(
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                         true);
        }
        else
        {
            LOG_DEBUG("(FG) running, frame: {0}", feature->FrameCount());
        }
    }
}
