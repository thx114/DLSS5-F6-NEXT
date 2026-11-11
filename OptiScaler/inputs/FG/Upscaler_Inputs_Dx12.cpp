#include "pch.h"
#include <native/OpticalFgNrPolicy.h>
#include "Upscaler_Inputs_Dx12.h"
#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>
#include "shaders/depth_scale/DS_Dx12.h"
#include "UpscalerFrameParameters.h"
#include <runtime/F6InputPolicy.h>
#include <framegen/dlssg/FgDepthContract.h>
#include <framegen/dlssg/DLSSG_Dx12.h>

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
    if (F6InputPolicy::FsrBridgeEnabled && dlssg::depth_contract::Game()==GameRouting::Identity::Genshin &&
        State::Instance().activeFgInput==FGInput::Upscaler &&
        State::Instance().activeFgOutput==FGOutput::DLSSG) return {}; // Sealed Present adapter only.
    if (native::OpticalFgNrEnabled() && feature->GetUpscalerType() != Upscaler::Optical) return {};

    Hudfix_Dx12::SetSkipStatus(true);

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    const auto frame = fg_inputs::ReadFrameParameters(
        *InParameters, *feature, cfg, state.lastFGFrameTime,
        { OptiKeys::FSR_NearPlane, OptiKeys::FSR_FarPlane, OptiKeys::FSR_CameraFovVertical,
          OptiKeys::FSR_ViewSpaceToMetersFactor });
    state.lastFsrCameraFar = frame.cameraFar.value;
    state.lastFsrCameraNear = frame.cameraNear.value;

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _device == nullptr)
        return {};

    auto fgConstants = fg_inputs::MakeFgConstants<FG_Constants, FG_Flags>(frame);

    // Source admission remains an edge contract. A common scalar request does
    // not replace the native game's selected, synchronous source scope.
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

    const bool checkedSource=own && own->OwnsUpscalerDepthContract();
    if (checkedSource && !nativeZzz && fg->Mutex.getOwner()==2)
    {
        fg->Mutex.lock(4);
        fg->Mutex.unlockThis(4);
    }
    auto inputExecution=nativeZzz ? own->BeginNativeUpscalerInputs(_device,fgConstants) :
        checkedSource ? fg->GuardExecution(FGExecutionScope::Inputs) : FGExecutionGuard{};
    if (checkedSource && (!fg->CanSubmitInputs() || (nativeZzz && !own->OwnsNativeZzzSource()))) return {};
    const auto previousFrame=fg->FrameCount();
    const auto startedFrame=fg->StartNewFrame();
    if (checkedSource && (!fg->CanSubmitInputs() || startedFrame==previousFrame)) return {};

    fg_inputs::ApplyFrameParameters(*fg, frame);
    if (checkedSource)
    {
        // The actual selected feature's observed source ordinal, before this
        // Evaluate. Do not add one or substitute a provider/SDK token for it.
        const auto source=feature->FrameCount();
        own->ObserveUpscalerGuideSource(static_cast<IFeature*>(feature),InParameters,
            source>0?static_cast<UINT64>(source):0);
    }

    Hudfix_Dx12::UpscaleStart();

    // FG Prepare
    UINT frameIndex;
    if (!State::Instance().isShuttingDown && fg->IsActive() && Config::Instance()->FGEnabled.value_or_default() &&
        State::Instance().currentSwapchain != nullptr)
    {
        // Wait for present
        if (!checkedSource && fg->Mutex.getOwner() == 2)
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

        auto* paramVelocity = fg_inputs::ReadResource<ID3D12Resource>(*InParameters, NVSDK_NGX_Parameter_MotionVectors);

        if (checkedSource)
        {
            auto* realDepth = fg_inputs::ReadResource<ID3D12Resource>(*InParameters, NVSDK_NGX_Parameter_Depth);
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

        auto* paramDepth = fg_inputs::ReadResource<ID3D12Resource>(*InParameters, NVSDK_NGX_Parameter_Depth);

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
                                    IFeature_Dx12* feature, bool succeeded,
                                    const DLSSG_Dx12::UpscalerColourSourcePtr& colourSource,
                                    ID3D12Resource* evaluatedOutput)
{
    if (native::OpticalFgNrEnabled() && feature->GetUpscalerType() != Upscaler::Optical) return;

    Hudfix_Dx12::SetSkipStatus(false);
    if (!succeeded) return; // A failed Evaluate cannot publish a colour candidate.
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
            Hudfix_Dx12::UpscaleEnd(colourSource ? colourSource->Frame() : feature->FrameCount(),
                State::Instance().lastFGFrameTime);

            if (auto* own=dynamic_cast<DLSSG_Dx12*>(fg); own && own->OwnsUpscalerDepthContract())
            {
                own->CaptureUpscalerHudless(colourSource,InCmdList,evaluatedOutput,
                    (D3D12_RESOURCE_STATES)Config::Instance()->OutputResourceBarrier.value_or(
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                return;
            }

            auto* output = fg_inputs::ReadResource<ID3D12Resource>(*InParameters, NVSDK_NGX_Parameter_Output);
            if (output == nullptr)
                return;

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
