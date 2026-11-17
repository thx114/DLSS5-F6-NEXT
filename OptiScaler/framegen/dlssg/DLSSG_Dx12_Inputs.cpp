#include "pch.h"
#include <runtime/ModuleLifetime.h>
#include <runtime/D3D12DeviceIdentity.h>
#include <with_dx12/dx11_with_dx12.h>
#include <runtime/GuideCapture.h>
#include <runtime/GuideSourceMetadata.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

#include <native/OpticalFgNrPolicy.h>

#include "DLSSG_Dx12.h"
#include <inputs/taa/TaaSourceOutput.h>
#include "DLSSG_Dx12_Internal.h"
#include <runtime/ComIdentity.h>
#include <framegen/GuideOrientation.h>
#include <hooks/SwapchainCreateRule.h>
#include "StarRailF5DsPolicy.h"
#include <runtime/StarRailResidencyPolicy.h>
#include <upscalers/CallerCpuTrace.h>
#include "FgDepthContract.h"
#include <resource_tracking/ResTrack_dx12.h>
#include <cmath>
#include <bit>
#include <string_view>
#include <utility>
#include <iterator>
#include <type_traits>
#include <runtime/GameIdentity.h>
#include <runtime/HookCpuTiming.h>
#include "Kcd2Hdr.h"

#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <hooks/Reflex_Hooks.h>
#include <hooks/FG_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <native/NativeLowLatency.h>
#include "DlssgTelemetry.h"
#include <hooks/DxgiFactory_Hooks.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>

using namespace DirectX;

using dlssg::detail::g_dlssgTeardownQuarantined;

bool DLSSG_Dx12::OwnsDepthContract() const
{
    const auto& state=State::Instance();
    const auto game=dlssg::depth_contract::Game();
    return game!=GameRouting::Identity::Other && state.currentFG==this &&
        state.activeFgInput==FGInput::Upscaler && state.activeFgOutput==FGOutput::DLSSG &&
        !state.externalFrameGeneration && !native::OpticalFgNrEnabled();
}

bool DLSSG_Dx12::RefuseDepthContract(const char* stage, bool publicationStarted)
{
    PublishGuideObservation(GetIndex(),GuideDiagnostics::Stage::Rejected,stage);
    const auto now=GetTickCount64();
    if (_depthContractRows<64 && (_depthContractRows<3 ||
        (stage!=_depthContractStage && now-_depthContractLogTick>=1000)))
    {
        ++_depthContractRows; _depthContractLogTick=now; _depthContractStage=stage;
        LOG_WARN("R1054 FG depth refusal: pid={} game={} provider={} epoch={} frame={} stage={} pixel_correct=UNKNOWN generated=UNKNOWN",
            GetCurrentProcessId(),(int)dlssg::depth_contract::Game(),(uintptr_t)this,
            _inputOwnerEpoch,_frameCount,stage);
    }
    // Runtime activation/FGEnabled/fgChanged are deliberately NOT route identity:
    // before the first EvaluateState they can be false for a legitimate StarRail
    // source (e.g. the missing-exposure backend replacement).
    const bool starRailSource=OwnsUpscalerDepthContract() &&
        dlssg::depth_contract::Game()==GameRouting::Identity::StarRail &&
        State::Instance().swapchainInteropApi==SwapchainInteropApi::Dx11wDx12 &&
        !State::Instance().gameCalledSlInit;
    if (starRailSource || !publicationStarted)
    {
        // A missing/rejected source is a retained input gap in adamfg/F5-DS,
        // not a physical teardown. Do not introduce FreeResources, allocator
        // waits, pool reconstruction, Reflex Off, or same-frame retries.
        _guideTags[GetIndex()][0].accepted=false;
        _guideTags[GetIndex()][1].accepted=false;
        _starRailDispatchPending=false;
        _depthSourceRejected=true;
        if (starRailSource)
        {
            if (_haveRunningOptions)
                _inputContinuity.Hold(_frameCount,CurrentInputOwner());
            // The existing F5-DS final admission sends Off+retain/null tags.
            // With no running inputs there is no GPU work to tear down.
            return false;
        }
        if (OwnsNativeZzzSource())
        {
            PrepareNativeGuideGapPresent();
            return false; // The hook distinguishes successful Off from unsafe failure.
        }
        if (!_haveRunningOptions) return false;
        // Native missing/busy sources suspend SDK generation, not SR/NR or
        // physical owners. A later complete source sends On/reset normally.
        // Do not create _managedRetained merely because validation failed.
        if (!_depthSourceGap)
        {
            InvalidateDispatchOptions(); // Every Off revokes any shared On acknowledgement.
            auto options=_lastRunningOptions;
            options.mode=sl::DLSSGMode::eOff;
            options.flags|=sl::DLSSGFlags::eRetainResourcesWhenOff;
            const bool nativeZzz=OwnsNativeZzzSource();
            const auto source=NativeGuideFrameSource(GetIndex());
            const bool frameKnown=!nativeZzz || (source.sdkFrameKnown && source.frame==_frameCount);
            uint32_t id=nativeZzz ? source.sdkFrame : static_cast<uint32_t>(_frameCount);
            sl::FrameToken* token=nullptr;
            sl::ResourceTag tags[4]{};
            tags[0].type=sl::kBufferTypeDepth; tags[1].type=sl::kBufferTypeMotionVectors;
            tags[2].type=sl::kBufferTypeHUDLessColor; tags[3].type=sl::kBufferTypeUIColorAndAlpha;
            // No real origin means no invented null-tag frame. Off+retain
            // revokes consumption without releasing the recorded GPU owners.
            if (StreamlineHooks::SubmitDlssgOptions(StreamlineProxy::DLSSGSetOptions(),viewport,options)!=sl::Result::eOk ||
                (frameKnown && (!(nativeZzz ? dlssg::zzz::TokenFor(id,token,StreamlineProxy::GetNewFrameToken()) :
                    StreamlineProxy::GetNewFrameToken()(token,&id)==sl::Result::eOk && token) ||
                    StreamlineProxy::SetTagForFrame()(*token,viewport,tags,4,nullptr)!=sl::Result::eOk)))
            {
                // Actual SDK failure retains the original safe failure boundary.
                Deactivate();
                return false;
            }
            _depthSourceGap=true;
        }
        return false;
    }
    // Other routes keep the existing Off+retain/drain/free failure boundary.
    // A partial tag/conversion can already have recorded real GPU reads.
    _managedRetained=true;
    Deactivate();
    return false;
}

bool DLSSG_Dx12::OwnsUpscalerDepthContract() const
{
    return OwnsDepthContract();
}

bool DLSSG_Dx12::OwnsNativeZzzSource() const
{
    return OwnsUpscalerDepthContract() &&
        dlssg::depth_contract::Game()==GameRouting::Identity::ZenlessZoneZero &&
        State::Instance().swapchainInteropApi==SwapchainInteropApi::None;
}

void DLSSG_Dx12::RejectUpscalerGuideSource(const char* stage)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (OwnsUpscalerDepthContract() && CanSubmitInputs()) RefuseDepthContract(stage,false);
}

void DLSSG_Dx12::PublishGuideObservation(int slot, GuideDiagnostics::Stage stage, const char* reason) noexcept
{
    try
    {
        if (slot<0 || static_cast<size_t>(slot)>=std::size(_guideObservations)) return;
        auto& observation=_guideObservations[slot];
        auto& snapshot=observation.snapshot;
        // A new refused input cannot acquire the previous ring occupant's ID.
        if (stage==GuideDiagnostics::Stage::Rejected && observation.frame!=_frameCount)
        {
            snapshot={};observation.frame=_frameCount;
            observation.sourceTick=GuideDiagnostics::Now();
            snapshot.owner=_providerOwnerGeneration;
            snapshot.epoch=_inputContinuity.SourceGeneration();
            snapshot.configuration=_guideObservationConfiguration;
        }
        snapshot.stage=stage;snapshot.reason=reason;snapshot.tick=observation.sourceTick;
        for (unsigned field=0;field<=static_cast<unsigned>(GuideDiagnostics::Field::JitterY);++field)
            if (!snapshot.fields[field].valid)
            {
                snapshot.reason+="; missing=";snapshot.reason+=GuideDiagnostics::FieldNames[field];
            }
        if (stage==GuideDiagnostics::Stage::Rejected) snapshot.accepted=false;
        // SDK eOk, options On and interval-presented counts cannot distinguish
        // enabled interpolation from SDK flush/suppression. Keep it unknown.
        snapshot.sdkStateKnown=false;snapshot.generatedIdentityKnown=false;
        GuideDiagnostics::Publish(GuideDiagnostics::Consumer::Fg,snapshot);
    }
    catch (...) {} // Diagnostic allocation failures cannot fail SR/FG.
}

void DLSSG_Dx12::PollGuideObservations() noexcept
{
    for (UINT slot=0;slot<std::size(_guidePendingCompletions);++slot)
    {
        auto& observation=_guidePendingCompletions[slot];
        if (!observation.submitted || observation.completed || !observation.fence ||
            !observation.value || observation.value==UINT64_MAX) continue;
        const auto done=observation.fence->GetCompletedValue();
        if (done==UINT64_MAX || done<observation.value) continue;
        observation.completed=true;
        try
        {
            auto snapshot=observation.snapshot;
            // Completion may be polled much later. It must not rejuvenate the
            // source's age or make an old same-source snapshot look fresh.
            snapshot.stage=GuideDiagnostics::Stage::Completed;
            snapshot.tick=observation.sourceTick;
            snapshot.reason="guide producer GPU completed; SDK input processing/generation unknown; "+snapshot.reason;
            GuideDiagnostics::Publish(GuideDiagnostics::Consumer::Fg,std::move(snapshot));
        }
        catch (...) {}
        observation.fence.Reset();
    }
}

void DLSSG_Dx12::ObserveGuideSubmission(UINT slot, ID3D12Fence* fence, UINT64 value) noexcept
{
    if (slot>=std::size(_guideObservations) || !fence || !value || value==UINT64_MAX) return;
    auto& observation=_guideObservations[slot];
    const auto& depth=_guideTags[slot][0];const auto& motion=_guideTags[slot][1];
    // GetUICommandList can submit a different pending slot. Attribute that
    // actual Execute to its original source, never the newest callback.
    // A colour-only preview UI Execute is not the native guide producer.
    if (fence==_uiFence && (depth.input.cmdList!=_uiCommandList[slot] ||
        motion.input.cmdList!=_uiCommandList[slot])) return;
    if (!depth.accepted || !motion.accepted || depth.frame!=observation.frame ||
        motion.frame!=observation.frame || depth.generation!=observation.snapshot.epoch ||
        motion.generation!=observation.snapshot.epoch || !(depth.owner==motion.owner)) return;
    observation.fence=fence;observation.value=value;observation.submitted=true;
    observation.snapshot.submission=value;
    PublishGuideObservation(slot,GuideDiagnostics::Stage::Submitted,
        "guide producer Execute returned and actual owner Signal succeeded; GPU pending");
    try { _guidePendingCompletions[slot]=observation; } catch (...) {}
}

void DLSSG_Dx12::ObserveUpscalerGuideSource(IFeature* feature, const NVSDK_NGX_Parameter* parameters,
    UINT64 sourceFrame, const GuideDiagnostics::Resource& colour,GuideSourceMetadata::Ptr rawSource)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    try
    {
        PollGuideObservations();
        const auto owner=CurrentInputOwner();
        const auto featureId=feature && feature->Handle() ? feature->Handle()->Id : 0;
        if (!_guideObservationConfiguration || !(owner==_guideObservationOwner) || featureId!=_guideObservationFeature)
        {
            if (_guideObservationConfiguration!=UINT64_MAX) ++_guideObservationConfiguration;
            _guideObservationOwner=owner;_guideObservationFeature=featureId;
        }
        auto& observation=_guideObservations[GetIndex()];
        observation={};
        observation.sourceTick=GuideDiagnostics::Now();
        _upscalerColourSources[GetIndex()].reset();
        _upscalerColourSucceeded[GetIndex()]=false;
        _upscalerFinalColours[GetIndex()].reset();
        _upscalerPendingHudless[GetIndex()]={};
        _guideUiTagFrame[GetIndex()]=0;_guideUiTagGeneration[GetIndex()]=0;
        auto& snapshot=observation.snapshot;
        observation.frame=_frameCount;snapshot.source=sourceFrame;snapshot.feature=featureId;
        snapshot.epoch=_inputContinuity.SourceGeneration();snapshot.owner=_providerOwnerGeneration;
        snapshot.configuration=_guideObservationConfiguration;
        snapshot.colour=colour;
        snapshot.colourChoice=colour.present ?
            "current successful Evaluate output identity; final present-colour match unverified" :
            "final present-colour source not observed";
        using Field=GuideDiagnostics::Field;
        using Origin=GuideDiagnostics::Origin;
        snapshot.rawSource=std::move(rawSource);
        if(snapshot.rawSource)
        {
            const auto& raw=*snapshot.rawSource;
            for(unsigned i=0;i<raw.provenance.fields.size();++i){
                const auto& value=raw.provenance.fields[i];
                snapshot.Set(static_cast<Field>(i),value.value,value.valid,static_cast<Origin>(value.origin));
            }
            snapshot.motionJitterKnown=raw.flagsKnown;
            snapshot.motionJittered=raw.jittered;
            snapshot.depthInverted=raw.depthInverted;
            // Independent FG clocks and colour-join identity remain untouched.
            // No claim about final colour phase follows from NGX jitter facts.
        }
        else
        {
        snapshot.Set(Field::CreateFlags,feature?feature->GetFeatureFlags():0,
            feature && feature->GetFeatureFlagsKnown(),Origin::RetainedFeature);
        const auto scalar=[&](Field field,const char* key,auto initial)
        {
            auto value=initial;
            const bool known=parameters && parameters->Get(key,&value)==NVSDK_NGX_Result_Success &&
                std::isfinite(static_cast<double>(value));
            snapshot.Set(field,known?static_cast<double>(value):0,known,Origin::Evaluate);
        };
        scalar(Field::RenderWidth,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,UINT{});
        scalar(Field::RenderHeight,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,UINT{});
        scalar(Field::DepthX,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,UINT{});
        scalar(Field::DepthY,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,UINT{});
        scalar(Field::MotionX,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,UINT{});
        scalar(Field::MotionY,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,UINT{});
        scalar(Field::ScaleX,NVSDK_NGX_Parameter_MV_Scale_X,float{});
        scalar(Field::ScaleY,NVSDK_NGX_Parameter_MV_Scale_Y,float{});
        scalar(Field::JitterX,NVSDK_NGX_Parameter_Jitter_Offset_X,float{});
        scalar(Field::JitterY,NVSDK_NGX_Parameter_Jitter_Offset_Y,float{});
        }
        const auto evaluate=GuideDiagnostics::CurrentEvaluation();
        auto facts=GuideDiagnostics::TraceFacts(snapshot,FrameTrace::Kind::GuideSourceObserved);
        facts.api=evaluate.api;facts.evaluateOrdinal=evaluate.ordinal;facts.apiFeature=evaluate.feature;
        facts.context=evaluate.context;
        GuideDiagnostics::Observe(GuideDiagnostics::Consumer::Fg,snapshot,facts,"source guides");
        PublishGuideObservation(GetIndex(),GuideDiagnostics::Stage::Received,"Evaluate received");
    }
    catch (...) {}
}

void DLSSG_Dx12::RequestGuideHooks()
{
    // Both callers hold _inputContinuityMutex on their source/Present thread.
    // Keep Detours' current-thread update synchronous, outside resource locks.
    if (_guideHooksRequested) return;
    ResTrack_Dx12::HookLateNrQueue(_device);
    // The void installer reports no readiness. As for native guides, missing
    // real Execute/Reset notifications must leave recorded resources retained.
    _guideHooksRequested=true;
}

void DLSSG_Dx12::ResetCompletedGuideRecordings() noexcept
{
    // Three diagnostic slots must not wait for the four-slot producer ring to
    // wrap. End only a sampled, already-submitted recording's replay lifetime;
    // the real UI owner fence (not the sampler's fence) authorizes its Reset.
    // No new recording/fence reservation, resource reuse or GPU wait occurs.
    for (UINT slot=0;slot<BUFFER_COUNT;++slot)
    {
        if (_uiCommandListResetted[slot] || _uiCloseFailed[slot] ||
            !_uiCommandList[slot] || !_uiCommandAllocator[slot] ||
            !GuideCapture::NeedsReset(_uiCommandList[slot]) ||
            PollUIAllocator(slot)!=UIAllocatorStatus::Ready) continue;
        if (FAILED(_uiCommandAllocator[slot]->Reset())) continue;
        if (FAILED(_uiCommandList[slot]->Reset(_uiCommandAllocator[slot],nullptr))) continue;
        DlssNr::FinishedPictureResetCommandList(_uiCommandList[slot]);
        // Leave it closed and unreserved. The ordinary owner opens/reserves it
        // on its next use; an unsignalled target here would strand that slot.
        if (FAILED(_uiCommandList[slot]->Close())) _uiCloseFailed[slot]=true;
    }
}

bool DLSSG_Dx12::TryOpenGuidePreviewRecording(UINT slot) noexcept
{
    // Present holds continuity and has already ordered the real producer.
    // Unlike GetUICommandList, this optional path neither waits nor submits
    // another slot. Only THIS allocator's actual owner fence permits Reset.
    if (slot>=std::size(_uiCommandList) || _uiCloseFailed[slot] || !_gameCommandQueue ||
        _gameCommandQueue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !dlssg::depth_contract::OnDevice(_device,_gameCommandQueue)) return false;
    if (_uiCommandListResetted[slot]) return _uiCommandList[slot]!=nullptr;
    if (_uiFenceValue>=UINT64_MAX-1 || PollUIAllocator(slot)!=UIAllocatorStatus::Ready) return false;
    if (FAILED(_uiCommandAllocator[slot]->Reset())) return false;
    if (FAILED(_uiCommandList[slot]->Reset(_uiCommandAllocator[slot],nullptr))) return false;
    _uiCommandListResetted[slot]=true;
    _uiAllocatorFenceValues[slot]=++_uiFenceValue;
    return true; // Present's existing Close/Execute/Signal owns this recording.
}

void DLSSG_Dx12::CaptureAcceptedGuides(UINT slot) noexcept
{
    const bool previewWanted=GuideCapture::PreviewWanted(GuideCapture::Stream::Fg);
    const bool offline=GuideCapture::OfflineWanted(GuideCapture::Stream::Fg);
    if (!previewWanted&&!offline) return;
    const auto mode=GuideCapture::GetPreviewMode();
    const bool wantsColour=previewWanted && mode==GuideCapture::PreviewMode::FgColour;
    // An early guide submission has no final colour yet. Do not reserve a
    // colour sample (and consume its rate-limit interval) before Present joins
    // that exact source's final endpoint. Offline guide capture is independent.
    const bool colourReady=slot<std::size(_upscalerFinalColours) && _upscalerFinalColours[slot] &&
        UpscalerColourCurrent(_upscalerFinalColours[slot]->source);
    GuideCapture::SampleToken sample;
    const bool preview=(!wantsColour || colourReady) &&
        GuideCapture::TryBeginSample(GuideCapture::Stream::Fg,sample);
    const bool colourOnly=preview && mode==GuideCapture::PreviewMode::FgColour;
    if (!preview&&!offline) return;
    struct Cancel {GuideCapture::SampleToken& token;~Cancel(){GuideCapture::CancelSample(token);}} cancel{sample};
    try
    {
        if (slot>=std::size(_guideObservations) || _uiCloseFailed[slot]) return;
        const auto& depth=_guideTags[slot][0];const auto& motion=_guideTags[slot][1];
        const auto owner=CurrentInputOwner();
        if (!depth.accepted || !motion.accepted || depth.frame!=_frameCount || motion.frame!=_frameCount ||
            depth.generation!=_inputContinuity.SourceGeneration() || motion.generation!=depth.generation ||
            !(depth.owner==owner) || !(motion.owner==owner)) return;
        if (OwnsNativeZzzSource() && (depth.nativeSerial!=NativeGuideFrameSource(slot).serial ||
            motion.nativeSerial!=NativeGuideFrameSource(slot).serial)) return;
        auto& snapshot=_guideObservations[slot].snapshot;
        if (_guideObservations[slot].frame!=_frameCount || snapshot.epoch!=_inputContinuity.SourceGeneration()) return;
        GuideCapture::Inputs inputs;
        const bool ownedGuides=_dx11GuideSealedFrame[slot]!=_frameCount &&
            _uiCommandListResetted[slot] && depth.input.cmdList==_uiCommandList[slot] &&
            motion.input.cmdList==_uiCommandList[slot];
        // A native ValidNow tag is identity only here: its borrowed source may
        // already have been reused. Colour does not require reading either guide.
        // Guide/offline capture still requires the exact open owned recording.
        if (!colourOnly || (offline && ownedGuides))
        {
            if (!ownedGuides)
            {
                if (preview) GuideCapture::PreviewUnavailable(
                    "Unavailable: current guides have no open independently owned recording");
                return;
            }
            inputs.depth=depth.input.copy?depth.input.copy:depth.input.resource;
            inputs.motion=motion.input.copy?motion.input.copy:motion.input.resource;
            inputs.depthState=depth.input.state;inputs.motionState=motion.input.state;
            inputs.depthRect={depth.input.left,depth.input.top,static_cast<UINT>(depth.input.width),depth.input.height};
            inputs.motionRect={motion.input.left,motion.input.top,static_cast<UINT>(motion.input.width),motion.input.height};
            // Exactly BuildGuideConstants' SDK UV conversion, not raw input scale.
            if (!motion.input.width || !motion.input.height) return;
            inputs.motionToUvX=_mvScaleX[slot]/static_cast<float>(motion.input.width);
            inputs.motionToUvY=_mvScaleY[slot]/static_cast<float>(motion.input.height);
            if (!std::isfinite(inputs.motionToUvX) || !std::isfinite(inputs.motionToUvY)) return;
        }
        if (colourOnly && (!_upscalerFinalColours[slot] ||
            !UpscalerColourCurrent(_upscalerFinalColours[slot]->source))) return;
        RequestGuideHooks(); // FG-only preview must not depend on NR/native use.
        if (colourOnly && !TryOpenGuidePreviewRecording(slot)) return;
        // Present has already joined the exact current-source final endpoint.
        // A try-only resource read never creates recordings or waits for a pool.
        std::shared_lock resources(_resourceMutex[slot],std::try_to_lock);
        if(!resources.owns_lock())return;
        const auto& final=_upscalerFinalColours[slot];
        const auto tagged=_frameResources[slot].find(FG_ResourceType::PresentColor);
        const auto ready=_resourceReady[slot].find(FG_ResourceType::PresentColor);
        if(final && final->source==_upscalerColourSources[slot] && UpscalerColourCurrent(final->source) &&
            tagged!=_frameResources[slot].end() && ready!=_resourceReady[slot].end() && ready->second){
            auto& actual=tagged->second;auto* colour=actual.GetResource();
            if(colour && actual.validity==FG_ResourceValidity::UntilPresent &&
                ComIdentity::Get(colour)==final->endpoint.colour &&
                actual.width==final->description.Width && actual.height==final->description.Height){
                inputs.colour=colour;inputs.colourState=actual.state;
                inputs.colourRect={actual.left,actual.top,static_cast<UINT>(actual.width),actual.height};
                // NGX's HDR flag describes SR input, not the final display
                // endpoint. Depth/MV are independent of colour transfer too.
                if (colourOnly) inputs.colourHdr=
                    DlssNr::NativeInputColourSpace(_swapChain,final->description.Format)!=
                        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            }
        }
        inputs.independentRecording=true;
        if (preview) GuideCapture::TryRecord(sample,_uiCommandList[slot],inputs,snapshot);
        if (offline && ownedGuides) GuideCapture::RecordOffline(GuideCapture::Stream::Fg,_uiCommandList[slot],inputs,snapshot);
        // The shared real Execute/Reset hooks notify GuideCapture exactly once.
        // Missing final colour stays unavailable for colour mode; guide-only
        // samples remain independent of NR and of the scene colour's HDR flag.
    }
    catch (...) {}
}

bool DLSSG_Dx12::FlipOwnedUpscalerGuide(Dx12Resource* input,const framegen::GuideOrientation& orientation)
{
    const auto slot=GetIndex();
    const bool depth=input->type==FG_ResourceType::Depth;
    const auto recording=OwnsNativeZzzSource() ? FindNativeGuideSource(NativeGuideFrameSource(slot)) : nullptr;
    if (OwnsNativeZzzSource() && !recording) return false;
    auto& shader=recording ? recording->flip[depth?0:1] : _guideFlip[slot][depth?0:1];
    if (!shader) shader=std::make_unique<RF_Dx12>(depth?"FG owned depth flip":"FG owned motion flip",_device);
    if (!shader->IsInit()) return false;
    ID3D12Resource* output=nullptr;
    if (recording)
    {
        auto& copy=recording->copies[depth?0:1];
        output=copy.Detach(); // The allocation helper owns/replaces this reference.
        const bool created=CreateBufferResource(_device,input->resource,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            &output,true,depth);
        copy.Attach(output);
        if (!created) return false;
    }
    else
    {
        auto& copy=_resourceCopy[slot][input->type];
        if (!CreateBufferResource(_device,input->resource,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            &copy,true,depth)) return false;
        output=copy;
    }
    if (!shader->Dispatch(input->cmdList,input->resource,output,input->width,input->height,!depth,
        depth ? orientation.depthFlipY : orientation.motionFlipY,orientation.motionYInverted)) return false;
    input->copy=output;input->state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

bool DLSSG_Dx12::SetUpscalerGuidePair(IFeature* selectedFeature, ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Parameter* parameters,
    ID3D12Resource* depth, ID3D12Resource* motion, UINT width, UINT height, UINT mvWidth, UINT mvHeight,
    D3D12_RESOURCE_STATES depthState, D3D12_RESOURCE_STATES motionState, uintptr_t evaluatedOutput,
    std::optional<source_colour::Receipt> sourceNr, GuideTransport transport)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!OwnsUpscalerDepthContract() || !CanSubmitInputs()) return false;
    if (sourceNr && (!selectedFeature || !selectedFeature->Handle() ||
        !sourceNr->Matches(reinterpret_cast<uintptr_t>(selectedFeature), selectedFeature->Handle()->Id, evaluatedOutput)))
        return RefuseDepthContract("source-nr-receipt-mismatch", false);
    // Activate starts the normal target-frame pause. SetResource rejects guides
    // during that pause on every upscaler route, including DX11-on12. Defer
    // before recording conversions or reserving a native receipt; otherwise
    // the first rejected tag tears down and restarts the same warmup forever.
    // This is scheduled warmup, not partial SDK publication.
    // UpscaleStart already advanced the frame; SR/NR continue toward the target.
    if (!IsActive() || IsPaused()) return false;
    const bool nativeZzzSource=OwnsNativeZzzSource();
    if (!_inputRecordingDepth || !parameters || !list || list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !dlssg::depth_contract::OnDevice(_device,list) ||
        !dlssg::depth_contract::OnDevice(_device,depth) || !dlssg::depth_contract::OnDevice(_device,motion) ||
        dlssg::depth_contract::SameOwner(depth,motion)) return RefuseDepthContract("source-guide-owner",false);
    UINT left=0,top=0,mvLeft=0,mvTop=0;
    dlssg::depth_contract::InputRect(parameters,true,left,top);
    dlssg::depth_contract::InputRect(parameters,false,mvLeft,mvTop);
    const auto depthDesc=depth->GetDesc(),motionDesc=motion->GetDesc();
    if (!dlssg::depth_contract::DepthFormat(depthDesc.Format) ||
        !dlssg::depth_contract::MotionFormat(motionDesc.Format))
        return RefuseDepthContract("source-guide-format",false);
    if (!dlssg::depth_contract::Region(depthDesc,left,top,width,height) ||
        !dlssg::depth_contract::Region(motionDesc,mvLeft,mvTop,mvWidth,mvHeight))
        return RefuseDepthContract("source-guide-subrect",false);
    const bool scale=Config::Instance()->FGEnableDepthScale.value_or_default();
    const bool preparedRaster=taa::source_output::current &&
        taa::source_output::PreparedRasterGuides(taa::source_output::current->handle);
    ObserveGuideProducer(preparedRaster);
    const auto orientation=framegen::ResolveGuideOrientation(*Config::Instance(), preparedRaster);
    const bool depthFlip=orientation.DepthCopy(),motionFlip=orientation.MotionCopy();
    const bool flip=depthFlip || motionFlip;
    if (((scale || depthFlip) && (left || top)) || (motionFlip && (mvLeft || mvTop)))
        return RefuseDepthContract("conversion-subrect-not-supported",false);
    auto* feature=selectedFeature;
    const UINT scaleWidth=feature?feature->DisplayWidth():0;
    const UINT scaleHeight=feature?feature->DisplayHeight():0;
    if (scale && (!feature || width>scaleWidth || height>scaleHeight || !scaleWidth || !scaleHeight))
        return RefuseDepthContract("depth-scale-output-domain",false);
    // The existing helper cannot make a readable view of R16_TYPELESS and
    // throws for DENY_SHADER_RESOURCE. Do not record an invalid conversion.
    if (((scale || depthFlip) && ((depthDesc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        depthDesc.Format==DXGI_FORMAT_R16_TYPELESS)) ||
        (motionFlip && (motionDesc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
        return RefuseDepthContract("conversion-srv-unavailable",false);
    const auto slot=GetIndex();
    const bool ownedRecording=list==_uiCommandList[slot] && _uiCommandListResetted[slot] &&
        _uiFence && _uiAllocatorFenceValues[slot] && _gameCommandQueue &&
        dlssg::depth_contract::OnDevice(_device,_gameCommandQueue);
    const bool nativeZzz=!ownedRecording && nativeZzzSource;
    std::shared_ptr<NativeGuideUse> nativeRecording; // Keep CPU references valid across COM/SDK reentry.
    D3D12_RESOURCE_DESC selectedOutputDescription{};
    // Native ZZZ records on the real SR list, not a provider UI list. Track
    // that exact recording's Execute/Signal/Reset before reusing shader state.
    if (!ownedRecording && !nativeZzz)
        return RefuseDepthContract("conversion-producer-completion-unavailable",false);
    if (nativeZzz)
    {
        const auto canonicalList=dlssg::depth_contract::NativeObject(list);
        Microsoft::WRL::ComPtr<IUnknown> listIdentity;
        if (canonicalList) canonicalList.As(&listIdentity);
        const auto frameScope=dlssg::zzz::Selection::Current(reinterpret_cast<uintptr_t>(listIdentity.Get()),
            dlssg::zzz::SelectedFeatureIdentity<IFeature>(feature),static_cast<uint32_t>(viewport),reinterpret_cast<uintptr_t>(this));
        if (!frameScope) return RefuseDepthContract("native-frame-scope-not-selected-entry",false);
        ID3D12Resource* output=nullptr;
        if (parameters->Get(NVSDK_NGX_Parameter_Output,&output)!=NVSDK_NGX_Result_Success)
            parameters->Get(NVSDK_NGX_Parameter_Output,(void**)&output);
        if (output) selectedOutputDescription=output->GetDesc();
        if (!output || !dlssg::depth_contract::OnDevice(_device,output) ||
            dlssg::depth_contract::SameOwner(output,depth) || dlssg::depth_contract::SameOwner(output,motion) ||
            selectedOutputDescription.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            return RefuseDepthContract("native-selected-sr-output",false);
        RequestGuideHooks();
        if (_nativeGuideFailed) return false;
        if (_device->GetDeviceRemovedReason()!=S_OK)
        {
            _nativeGuideFailed=true; _managedRetained=true; Deactivate();
            return false;
        }
        for (const auto& recording:_nativeGuideRecordings)
            if (recording->fence && recording->fence->GetCompletedValue()==UINT64_MAX)
            {
                _nativeGuideFailed=true; _managedRetained=true; Deactivate();
                return false;
            }
        // A presentation slot can wrap before the game's producer list Reset.
        // Keep that recording intact; acquire GPU state by actual retirement,
        // independently of frame%BUFFER_COUNT. Never wait for a future Reset.
        nativeRecording=AcquireNativeGuideRecording();
        auto& use=*nativeRecording;
        if (!use.fence && FAILED(_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&use.fence))))
            return RefuseDepthContract("native-guide-fence-unavailable",false);
        if (!use.fence || use.value>=UINT64_MAX-1 || use.fence->GetCompletedValue()==UINT64_MAX)
            return RefuseDepthContract("native-guide-fence-invalid",false);
        if (_nativeGuideSerial>=UINT64_MAX-1)
        {
            _nativeGuideFailed=true; _managedRetained=true; Deactivate();
            return false;
        }
        ++use.value;
        use.source={++_nativeGuideSerial,_frameCount,_inputContinuity.SourceGeneration(),_inputOwnerEpoch,
            frameScope->origin.frame,(uintptr_t)list,(uintptr_t)output,
            frameScope->origin.api,frameScope->origin.viewport,static_cast<uint32_t>(viewport),true,
            reinterpret_cast<uintptr_t>(this)};
        use.frameScope=frameScope;
        use.owner=CurrentInputOwner(); use.settings=_constants;
        use.constants={}; use.dispatched=use.presented=false;
        use.list=list; use.nativeList=dlssg::depth_contract::NativeObject(list);
        use.queue.Reset(); use.depth=depth; use.motion=motion; use.output=output;
        use.closed=use.submitted=use.executing=use.signalled=use.srSucceeded=use.ordered=false;
        use.execution=0;
        ClearNativeGuideFrame(slot); // Revoke old frame tags without retiring its recording.
        _nativeGuideFrames[slot]=&use;
        _nativeGuideListInterest.Publish(&use,use.list.Get(),use.nativeList.Get());
        _nativeGuideTracking.store(true,std::memory_order_release);
        if (use.source.serial<=3 || use.source.serial%600==0)
            LOG_INFO("F6 ZZZ selected frame source: api={} feature={} selected={} game_viewport={} provider={} provider_viewport={} upstream_raw={} sdk_frame={} frame={} serial={} native_list={} list={} thread={} upstream=pending",
                use.source.upstreamApi,frameScope->origin.feature,dlssg::zzz::SelectedFeatureIdentity<IFeature>(feature),
                use.source.upstreamViewport,use.source.provider,use.source.providerViewport,
                frameScope->origin.frame,use.source.sdkFrame,use.source.frame,use.source.serial,
                reinterpret_cast<uintptr_t>(listIdentity.Get()),reinterpret_cast<uintptr_t>(list),GetCurrentThreadId());
    }
    if ((scale || flip) && dlssg::depth_contract::Game()==GameRouting::Identity::StarRail &&
        (!_starRailGuideSwapchain || !_starRailGuideQueueCurrent))
        return RefuseDepthContract("starrail-conversion-not-on-presenting-queue",false);
    // Borrowing follows the actual cache transport, independently of the game.
    // Native DX12/Vulkan callers keep their own resource lifetime contract.
    const bool sharedDx11=ownedRecording && transport==GuideTransport::SharedDx11;
    if (sharedDx11)
    {
        // A different queue needs a real producer receipt; do not guess order.
        if (!dlssg::depth_contract::SameOwner(_gameCommandQueue,Dx11WithDx12::GetD3D12CommandQueue()))
            return RefuseDepthContract("dx11-guide-source-queue-mismatch",false);
    }
    const auto borrowDx11Guides=[&] {
        if (!sharedDx11) return;
        // CPU-only validation/allocation may fail without recording a read.
        // Pin immediately before the first conversion or SDK snapshot.
        _dx11GuideInputs[slot][0]=depth; _dx11GuideInputs[slot][1]=motion;
        _dx11GuideBorrowed[slot]=true; _dx11GuideSealedFrame[slot]=0;
        _dx11GuideSubmissions[slot]={};
    };
    // Once commands/tags can be recorded, even an undispatched frame owns real
    // GPU/SDK state. A CPU failure cannot bypass the established teardown path.
    _managedRetained=true;_continuityOwner=true;_teardownBorrowedInputs=true;_guideContractOwned=true;
    // RF's allocation helper releases a mismatched destination immediately.
    // Detect the generation change before recording anything; the old real
    // owners must first pass Off/host completion/SDK free above.
    if (flip && !nativeZzz)
    {
        for (auto type : {FG_ResourceType::Depth,FG_ResourceType::Velocity})
        {
            if (type==FG_ResourceType::Depth ? !depthFlip : !motionFlip) continue;
            const auto previous=_resourceCopy[slot].find(type);
            if (previous==_resourceCopy[slot].end() || !previous->second) continue;
            const auto old=previous->second->GetDesc();
            const auto& source=type==FG_ResourceType::Depth ? depthDesc : motionDesc;
            const auto expectedWidth=type==FG_ResourceType::Depth && scale ? scaleWidth : source.Width;
            const auto expectedHeight=type==FG_ResourceType::Depth && scale ? scaleHeight : source.Height;
            const auto expectedFormat=type==FG_ResourceType::Depth ? DXGI_FORMAT_R32_FLOAT : source.Format;
            if (old.Width!=expectedWidth || old.Height!=expectedHeight || old.Format!=expectedFormat ||
                old.Flags!=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                return RefuseDepthContract("guide-copy-generation-needs-drain");
        }
    }
    ID3D12Resource* actualDepth=depth;
    if (scale)
    {
        const auto amount=Config::Instance()->FGDepthScaleMax.value_or_default();
        if (!std::isfinite(amount) || amount<=0 || !feature ||
            width>(feature->LowResMV()?feature->RenderWidth():feature->DisplayWidth()) ||
            height>(feature->LowResMV()?feature->RenderHeight():feature->DisplayHeight()))
            return RefuseDepthContract("depth-scale-domain");
        auto& shader=nativeZzz ? nativeRecording->depthScale : _guideDepthScale[slot];
        if (shader && shader->Buffer())
        {
            const auto old=shader->Buffer()->GetDesc();
            if (old.Width!=scaleWidth || old.Height!=scaleHeight)
            {
                if (!nativeZzz) return RefuseDepthContract("depth-scale-generation-needs-drain");
                // Acquire selected an already retired recording. Its previous
                // size can change before this recording emits its first command.
                shader.reset();
            }
        }
        if (!shader) shader=std::make_unique<DS_Dx12>("FG owned depth scale",_device);
        // Preserve the rollback's display-sized allocation. A changing render
        // subrect only changes the tag; it must not rebuild the FG pool.
        if (!shader->CreateBufferResource(_device,depth,scaleWidth,scaleHeight,D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
            !shader->Buffer()) return RefuseDepthContract("depth-scale-allocation");
        borrowDx11Guides();
        shader->SetBufferState(list,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ResourceBarrier(list,depth,depthState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool recorded=shader->Dispatch(list,depth,shader->Buffer());
        ResourceBarrier(list,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,depthState);
        if (!recorded) return RefuseDepthContract("depth-scale-not-recorded");
        actualDepth=shader->Buffer();depthState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (!scale) borrowDx11Guides();
    const auto publish=[&](FG_ResourceType type,ID3D12Resource* resource,UINT x,UINT y,
        UINT w,UINT h,D3D12_RESOURCE_STATES state)
    {
        Dx12Resource tag{};tag.type=type;tag.resource=resource;tag.cmdList=list;
        tag.left=x;tag.top=y;tag.width=w;tag.height=h;tag.state=state;
        tag.preparedRasterGuides=preparedRaster;
        // Owned StarRail outputs preserve the rollback's UntilPresent tag.
        // Their writes are ordered on the real presenting queue; raw/cache
        // inputs keep the rollback's existing eOnlyValidNow SDK snapshot.
        tag.validity=type==FG_ResourceType::Depth && scale &&
            dlssg::depth_contract::Game()==GameRouting::Identity::StarRail ?
            FG_ResourceValidity::UntilPresent : FG_ResourceValidity::ValidNow;
        const GuidePublication publication{&tag,orientation};
        const auto* previous=_upscalerGuidePublication;
        _upscalerGuidePublication=&publication;
        struct EndPublication { const GuidePublication*& slot; const GuidePublication* previous;
            ~EndPublication(){slot=previous;} } finish{_upscalerGuidePublication,previous};
        return SetResource(&tag);
    };
    if (!publish(FG_ResourceType::Velocity,motion,mvLeft,mvTop,mvWidth,mvHeight,motionState) ||
        !publish(FG_ResourceType::Depth,actualDepth,left,top,width,height,depthState))
    {
        if (CanSubmitInputs()) RefuseDepthContract("guide-pair-partial-publication");
        return false;
    }
    if (nativeZzz)
    {
        auto& use=*nativeRecording;
        // Snapshot the actual source camera/jitter/MV/reset only after both
        // accepted tags. Dispatch must not rebuild it from a later recording.
        if (!NativeGuideSourceCurrent(use.source) || !BuildGuideConstants(slot,use.constants,true))
            return RefuseDepthContract("native-source-constants");
    }
    try
    {
        auto& snapshot=_guideObservations[slot].snapshot;
        const auto describe=[](const GuideTag& tag)
        {
            return GuideDiagnostics::Resource{true,static_cast<UINT>(tag.description.Format),
                tag.description.Width,tag.description.Height,
                {tag.input.left,tag.input.top,static_cast<UINT>(tag.input.width),tag.input.height}};
        };
        snapshot.depth=describe(_guideTags[slot][0]);snapshot.motion=describe(_guideTags[slot][1]);
        // Actual accepted resources BEFORE depth scaling/row/sign conversion.
        snapshot.rawDepth={true,static_cast<UINT>(depthDesc.Format),depthDesc.Width,depthDesc.Height,{left,top,width,height}};
        snapshot.rawMotion={true,static_cast<UINT>(motionDesc.Format),motionDesc.Width,motionDesc.Height,{mvLeft,mvTop,mvWidth,mvHeight}};
        snapshot.rawSdkUvX=mvWidth?_mvScaleX[slot]/static_cast<float>(mvWidth):0;
        snapshot.rawSdkUvY=mvHeight?_mvScaleY[slot]/static_cast<float>(mvHeight):0;
        snapshot.rawMappingKnown=snapshot.rawSource && GuideSourceMetadata::AcceptedMapping(
            *snapshot.rawSource,snapshot.rawDepth,snapshot.rawMotion,snapshot.rawSdkUvX,snapshot.rawSdkUvY);
        snapshot.depthFlip=orientation.depthFlipY;snapshot.motionFlip=orientation.motionFlipY;
        snapshot.yNegated=orientation.motionYInverted;snapshot.depthInverted=IsInvertedDepth();
        snapshot.scaleX=_mvScaleX[slot]/static_cast<float>(_guideTags[slot][1].input.width);
        snapshot.scaleY=_mvScaleY[slot]/static_cast<float>(_guideTags[slot][1].input.height);
        snapshot.jitterX=_jitterX[slot];snapshot.jitterY=_jitterY[slot];
        snapshot.accepted=true;
        snapshot.Set(GuideDiagnostics::Field::DepthBounds,1,true,GuideDiagnostics::Origin::ActualCall);
        snapshot.Set(GuideDiagnostics::Field::MotionBounds,1,true,GuideDiagnostics::Origin::ActualCall);
        // A successful SDK tag is not a phase or display-colour certification.
        PublishGuideObservation(slot,GuideDiagnostics::Stage::Recorded,
            "current depth/MV tags accepted; constants/producer submission pending; UI optional");
    }
    catch (...) {}
    if (_frameCount<=32 || _frameCount%600==0)
        LOG_INFO("F6 FG guide coordinates: frame={} depthFlipY={} motionFlipY={} motionYInverted={} offset={} depth={}x{} mv={}x{} sourceMvScale={},{} jitter={},{} signApplied=texture constantsScaleUnchanged=true",
            _frameCount,orientation.depthFlipY,orientation.motionFlipY,orientation.motionYInverted,
            orientation.UsesOffset(),width,height,mvWidth,mvHeight,_mvScaleX[slot],_mvScaleY[slot],
            _jitterX[slot],_jitterY[slot]);
    _depthSourceRejected=false; // Only a complete real pair can recover admission.
    try
    {
        const auto& observation=_guideObservations[slot];
        const auto output=nativeZzz ? reinterpret_cast<uintptr_t>(nativeRecording->output.Get()) : evaluatedOutput;
        if (output && observation.frame==_frameCount && _upscalerColourSerial<UINT64_MAX)
        {
            const auto& snapshot=observation.snapshot;
            dlssg::colour::Source key{++_upscalerColourSerial,_frameCount,
                _inputContinuity.SourceGeneration(),_inputOwnerEpoch,snapshot.feature,snapshot.configuration,
                reinterpret_cast<uintptr_t>(this),ComIdentity::Get(_swapChain),output};
            _upscalerColourSources[slot]=UpscalerColourSourcePtr(new UpscalerColourSource(key,
                CurrentInputOwner(),snapshot,selectedOutputDescription,reinterpret_cast<uintptr_t>(list),!nativeZzz,sourceNr));
            // DX11's callback already follows a successful real Evaluate. Native
            // success must come from CompleteNativeGuideSource's exact receipt.
            _upscalerColourSucceeded[slot]=!nativeZzz;
        }
    }
    catch (const std::bad_alloc&) {} // Optional colour remains unavailable; guides still work.
    return true;
}

bool DLSSG_Dx12::UpscalerColourCurrent(const UpscalerColourSourcePtr& source) const
{
    if (!source || !OwnsUpscalerDepthContract()) return false;
    const auto slot=source->key.frame%std::size(_upscalerColourSources);
    return source==_upscalerColourSources[slot] && source->key.frame==_frameCount &&
        source->key.generation==_inputContinuity.SourceGeneration() && source->key.epoch==_inputOwnerEpoch &&
        source->key.provider==reinterpret_cast<uintptr_t>(this) && source->owner==CurrentInputOwner() &&
        _inputContinuity.Current(slot,source->key.frame,source->owner);
}

bool DLSSG_Dx12::SealDx11GuideInputs(bool tagsAccepted, ID3D12CommandQueue* sourceQueue)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    const auto slot=GetIndex();
    const auto fail=[&] {
        // No next shared upload, provider replacement or later fence can make
        // an uncertain borrowed read safe. Keep all actual owners retained.
        Dx11WithDx12::BlockFgGuideWrites();
        _managedRetained=true; Deactivate(); QuarantineFailedTeardown();
        return false;
    };
    // A frame/marker change cannot hide an older shared reader from the next
    // cache writer. Its current source no longer authenticates that recording.
    for (UINT other=0;other<BUFFER_COUNT;++other)
        if (other!=slot && _dx11GuideBorrowed[other]) return fail();
    if (!_dx11GuideBorrowed[slot]) return tagsAccepted; // Warmup/validation recorded no borrowed read.
    const auto& depth=_guideTags[slot][0];const auto& motion=_guideTags[slot][1];
    const auto owner=CurrentInputOwner();
    if (!tagsAccepted || !CanSubmitInputs() ||
        !dlssg::depth_contract::SameOwner(sourceQueue,_gameCommandQueue) ||
        !dlssg::depth_contract::SameOwner(sourceQueue,Dx11WithDx12::GetD3D12CommandQueue()) ||
        _uiCloseFailed[slot] || !depth.accepted || !motion.accepted ||
        depth.frame!=_frameCount || motion.frame!=_frameCount ||
        depth.generation!=_inputContinuity.SourceGeneration() || motion.generation!=depth.generation ||
        !(depth.owner==owner) || !(motion.owner==owner)) return fail();
    if (_uiCommandListResetted[slot])
    {
        // Capture only on the original still-open producer. Guide/offline
        // diagnostics defer StarRail sealing until its final colour is joined;
        // normal source handback precedes that independent Present work.
        CaptureAcceptedGuides(slot);
        if (!SubmitUICommandList(slot)) return fail();
    }
    else
    {
        // An earlier real submission may already have closed this recording.
        // Its exact successful owner Signal, not a Ready bit, permits handback.
        const auto& submission=_dx11GuideSubmissions[slot];
        if (submission.frame!=_frameCount || submission.generation!=depth.generation ||
            submission.fence!=_uiFence || !submission.value ||
            submission.value!=_uiAllocatorFenceValues[slot] ||
            !dlssg::depth_contract::SameOwner(submission.queue,_gameCommandQueue)) return fail();
    }
    if (!Dx11WithDx12::SyncDx12ToDx11(_gameCommandQueue)) return fail();
    _dx11GuideSealedFrame[slot]=_frameCount;
    _dx11GuideBorrowed[slot]=false;
    // The shared fence Signal was queued after the SDK's real snapshot read;
    // DX11 Wait now precedes every later source write. CPU waits for neither NR nor FG.
    return true;
}

DLSSG_Dx12::UpscalerColourSourcePtr DLSSG_Dx12::CurrentUpscalerColourSource()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    const auto source=_upscalerColourSources[GetIndex()];
    return UpscalerColourCurrent(source) ? source : nullptr;
}

void DLSSG_Dx12::CaptureUpscalerHudless(const UpscalerColourSourcePtr& source,
    ID3D12GraphicsCommandList* list, ID3D12Resource* output, D3D12_RESOURCE_STATES state)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!UpscalerColourCurrent(source) || source->bridge || !_upscalerColourSucceeded[GetIndex()] ||
        source->list!=reinterpret_cast<uintptr_t>(list) || source->key.output!=reinterpret_cast<uintptr_t>(output)) return;
    const auto& desc=source->outputDescription; // Reuse the selected Evaluate's actual description.
    ResourceInfo info{};
    info.buffer=output;info.width=desc.Width;info.height=desc.Height;info.format=desc.Format;
    info.flags=desc.Flags;info.type=UAV;info.captureInfo=CaptureInfo::Upscaler;
    const auto previous=_upscalerHudlessCapture;
    _upscalerHudlessCapture=source;
    struct EndCapture { UpscalerColourSourcePtr& active; UpscalerColourSourcePtr previous;
        ~EndCapture(){active=std::move(previous);} } finish{_upscalerHudlessCapture,previous};
    Hudfix_Dx12::CheckForHudless(list,&info,state,true);
}

void DLSSG_Dx12::JoinUpscalerFinalColour(const UpscalerColourSourcePtr& source, IDXGISwapChain* swapchain,
    ID3D12CommandQueue* queue, const DlssNr::FinishedPictureOutcome& outcome,
    ID3D12Resource* copiedColour, ID3D12Fence* copyFence, UINT64 copyValue)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!UpscalerColourCurrent(source) || !CanSubmitInputs() || !_upscalerColourSucceeded[GetIndex()] ||
        !queue || !dlssg::depth_contract::SameOwner(queue,_gameCommandQueue) ||
        source->key.swapchain!=ComIdentity::Get(swapchain) ||
        !dlssg::depth_contract::SameOwner(swapchain,_swapChain)) return;
    const auto slot=GetIndex();
    // The bridge's outer endpoint owns this join. A nested generic FG Present
    // cannot erase/relabel it, nor can a second Present reuse the same SR token.
    if (outcome.bridgeEndpoint!=source->bridge || (source->bridge && !copiedColour) ||
        _upscalerFinalColours[slot]) return;
    // These are observations of this source-bound endpoint, not admission or
    // a claim that the NR pixels completed. In particular record uncertain NR
    // failure before refusing its colour, so it cannot look like a skipped NR.
    {
        auto facts=GuideDiagnostics::TraceFacts(source->snapshot,FrameTrace::Kind::NrOutcome);
        facts.nrOutcomeKnown=true;facts.nrResult=static_cast<int>(outcome.result);
        facts.nrReason=static_cast<int>(outcome.reason);facts.colour=outcome.color;
        facts.queue=outcome.queue;facts.fenceValue=outcome.fenceValue;
        GuideDiagnostics::Observe(GuideDiagnostics::Consumer::Fg,source->snapshot,facts,"NR outcome");
    }
    if (outcome.result==DlssNr::FinishedPictureResult::FailedUncertain) return;
    // A Present NR transaction may only join the exact source selected before
    // the wrapper copied this colour. A diagnostic observation cannot grant it.
    if (outcome.source && outcome.source != source->snapshot.rawSource) return;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> sc;
    Microsoft::WRL::ComPtr<ID3D12Resource> colour;
    if (!swapchain || FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc)))) return;
    const auto index=sc->GetCurrentBackBufferIndex();
    if (FAILED(sc->GetBuffer(index,IID_PPV_ARGS(&colour)))) return;
    const dlssg::colour::Endpoint endpoint{ComIdentity::Get(swapchain),ComIdentity::Get(colour.Get()),
        ComIdentity::Get(queue),ComIdentity::Get(_device),State::Instance().frameCount};
    const dlssg::colour::Endpoint observed{outcome.swapchain,outcome.color,outcome.queue,outcome.device,outcome.presentEpoch};
    if (!dlssg::colour::SameEndpoint(endpoint,observed) || !outcome.endpointSerial ||
        outcome.bridgeEndpoint!=source->bridge) return;
    if (source->bridge && (!copiedColour || !dlssg::depth_contract::SameOwner(copiedColour,colour.Get()) ||
        !copyFence || !copyValue || copyValue==UINT64_MAX || copyFence->GetCompletedValue()==UINT64_MAX)) return;
    const auto mutation=dlssg::colour::CombinedMutation(source->sourceNr,
        outcome.Written() ? dlssg::colour::Mutation::NrWritten : dlssg::colour::Mutation::Original);
    if (mutation == dlssg::colour::Mutation::Uncertain) return;
    // WrittenOrdered means the real endpoint submitted on this very queue.
    // Retain that distinction; a CPU return or UI fence is never completion.
    if (outcome.Written() && (!outcome.fence || !outcome.fenceValue || outcome.fenceValue==UINT64_MAX ||
        !outcome.submissionSerial || outcome.fence->GetCompletedValue()==UINT64_MAX)) return;
    const auto desc=colour->GetDesc();
    if (desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width>UINT_MAX) return;
    std::shared_ptr<const UpscalerFinalColour> join;
    try
    {
        join=std::shared_ptr<const UpscalerFinalColour>(new UpscalerFinalColour(source,endpoint,
            source->bridge ? dlssg::colour::Route::Dx11CopiedPresent : dlssg::colour::Route::NativePresent,
            mutation,index,desc,outcome.submissionSerial,outcome.fenceValue,copyValue));
    }
    catch (const std::bad_alloc&) { return; } // Missing optional colour never fails SR/FG.
    if (!UpscalerColourCurrent(source)) return;
    _upscalerFinalColours[slot]=join;
    Dx12Resource tag{};tag.type=FG_ResourceType::PresentColor;tag.resource=colour.Get();
    tag.frameIndex=slot;tag.width=desc.Width;tag.height=desc.Height;
    tag.state=D3D12_RESOURCE_STATE_PRESENT;tag.validity=FG_ResourceValidity::UntilPresent;
    const auto publish=[&](Dx12Resource& input)
    {
        const auto* previous=_upscalerColourPublication;
        _upscalerColourPublication=&input;
        struct EndPublication { const Dx12Resource*& active; const Dx12Resource* previous;
            ~EndPublication(){active=previous;} } finish{_upscalerColourPublication,previous};
        return SetResource(&input);
    };
    if (!publish(tag)) return;
    // Do not republish raw/pre-NR HUDless beside a processed final image. The
    // absence of a compatible optional tag never suppresses FG itself.
    auto candidate=_upscalerPendingHudless[slot];
    if (candidate.source && dlssg::colour::CanUseHudless(source->key,candidate.source->key,mutation))
    {
        // The copy was already recorded by Hudfix. Its old producer list may
        // now be closed/submitted; eValidUntilPresent needs no borrowed list.
        candidate.input.cmdList=nullptr;
        publish(candidate.input);
    }
    if (!UpscalerColourCurrent(source) || _upscalerFinalColours[slot]!=join || !CanSubmitInputs()) return;
    try
    {
        auto& observation=_guideObservations[slot];auto& snapshot=observation.snapshot;
        snapshot.fields=source->snapshot.fields; // Reuse the exact source metadata, no parameter re-query.
        snapshot.colour={true,static_cast<UINT>(desc.Format),desc.Width,desc.Height,
            {0,0,static_cast<UINT>(desc.Width),desc.Height}};
        snapshot.picture=endpoint.present;
        snapshot.colourChoice=mutation == dlssg::colour::Mutation::NrWritten ?
            "source or Present NR written; final game composition copied; pixel lineage unverified" :
            "same-source current original present route; pixel lineage unverified";
        auto facts=GuideDiagnostics::TraceFacts(snapshot,FrameTrace::Kind::FinalColourJoined);
        facts.colour=endpoint.colour;facts.queue=endpoint.queue;facts.fenceValue=outcome.fenceValue;
        facts.nrOutcomeKnown=true;facts.nrResult=static_cast<int>(outcome.result);
        facts.nrReason=static_cast<int>(outcome.reason);
        GuideDiagnostics::Observe(GuideDiagnostics::Consumer::Fg,snapshot,facts,"final colour");
        PublishGuideObservation(slot,observation.submitted?GuideDiagnostics::Stage::Submitted:
            GuideDiagnostics::Stage::Recorded,"final colour route joined; optional HUDless checked against NR mutation");
    }
    catch (...) {} // Telemetry does not grant or revoke the actual join.
}

bool DLSSG_Dx12::NativeGuideRetired(const NativeGuideUse& use) const
{
    if (!use.list) return true;
    if (_nativeGuideFailed || !use.closed) return false;
    if (!use.submitted) return true; // Successful Reset discarded this unexecuted recording.
    if (!use.signalled || !use.fence || !use.value || use.value==UINT64_MAX) return false;
    const auto done=use.fence->GetCompletedValue();
    return done!=UINT64_MAX && done>=use.value;
}

std::shared_ptr<DLSSG_Dx12::NativeGuideUse> DLSSG_Dx12::AcquireNativeGuideRecording()
{
    // Continuity is held by the selected SR transaction. A recording is
    // recyclable only after successful Reset and its own producer completion;
    // removing a frame identity is never authority to recycle GPU state.
    std::shared_ptr<NativeGuideUse> reusable;
    UINT retired=0;
    for (auto it=_nativeGuideRecordings.begin();it!=_nativeGuideRecordings.end();)
    {
        auto& use=**it;
        if (it->use_count()!=1 || (use.source.frame==_frameCount && use.source.epoch==_inputOwnerEpoch) ||
            !NativeGuideRetired(use)) { ++it; continue; }
        _nativeGuideListInterest.Erase(&use);
        for (UINT slot=0;slot<std::size(_nativeGuideFrames);++slot)
        {
            if (_nativeGuideFrames[slot]!=&use) continue;
            ClearNativeGuideFrame(slot);
        }
        if (!reusable) reusable=std::move(*it);
        it=_nativeGuideRecordings.erase(it);
        ++retired;
    }
    const bool created=!reusable;
    if (!reusable) reusable=std::make_shared<NativeGuideUse>();
    _nativeGuideRecordings.push_back(reusable);
    if (created || _nativeGuideSerial<=3 || _nativeGuideSerial%600==0)
        LOG_INFO("F6 native guide recording lifecycle: pid={} frame={} epoch={} frame_slot={} owners={} retired={} created={} presentation_slots={} retirement=Reset+producer_fence",
            GetCurrentProcessId(),_frameCount,_inputOwnerEpoch,GetIndex(),_nativeGuideRecordings.size(),
            retired,created,BUFFER_COUNT);
    return reusable;
}

void DLSSG_Dx12::ClearNativeGuideFrame(UINT slot)
{
    // CPU consumer identity only. The independent recording registry retains
    // every GPU owner until its successful Reset and exact producer fence.
    std::unique_lock resources(_resourceMutex[slot]);
    _nativeGuideFrames[slot]=nullptr;
    for (auto type : {FG_ResourceType::Depth,FG_ResourceType::Velocity})
    {
        _frameResources[slot].erase(type); _resourceReady[slot].erase(type);
    }
    _guideTags[slot][0]={}; _guideTags[slot][1]={};
}

dlssg::NativeSource DLSSG_Dx12::NativeGuideFrameSource(UINT slot) const
{
    if (slot>=std::size(_nativeGuideFrames)) return {};
    const auto* use=_nativeGuideFrames[slot];
    return use ? use->source : dlssg::NativeSource{};
}

bool DLSSG_Dx12::NativeGuidesRetired() const
{
    for (const auto& recording:_nativeGuideRecordings)
        if (recording.use_count()!=1 || !NativeGuideRetired(*recording)) return false;
    return true;
}

UINT64 DLSSG_Dx12::NativeGuidesSubmitting(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!TracksNativeGuides() || !lists) return 0;
    // SDK Present may wait for a worker submitting an unrelated UI/SDK list.
    // That worker must not wait for the source continuity held across Present.
    if (!_nativeGuideListInterest.MayContain(count,lists)) return 0;
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownQuarantined) return 0;
    if (_nativeGuideExecution>=UINT64_MAX-1)
    {
        _nativeGuideFailed=true; _managedRetained=true; Deactivate(); return 0;
    }
    const auto execution=++_nativeGuideExecution;
    const auto recordings=_nativeGuideRecordings; // COM callbacks may reenter admission and grow the registry.
    for (UINT i=0;i<count;++i) for (const auto& recording:recordings)
    {
        auto& use=*recording;
        if (!use.list || use.closed ||
            (use.list.Get()!=lists[i] && use.nativeList.Get()!=lists[i])) continue;
        if (use.submitted)
        {
            _nativeGuideFailed=true;
            if (queue) queue->AddRef(); // A replay on a second queue retains both timelines.
        }
        else
        {
            // Reserve before Execute: a concurrent successful Reset cannot
            // mistake an escaped recording for an unsubmitted discard.
            use.queue=queue; use.submitted=true; use.executing=true; use.execution=execution;
        }
    }
    return execution;
}

void DLSSG_Dx12::NativeGuidesSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
    UINT64 execution)
{
    if (!execution || !TracksNativeGuides() || !lists) return;
    if (!_nativeGuideListInterest.MayContain(count,lists)) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownQuarantined) return;
    if (_nativeGuideFailed) { _managedRetained=true; Deactivate(); return; }
    const auto recordings=_nativeGuideRecordings;
    for (UINT i=0;i<count;++i) for (const auto& recording:recordings)
    {
        auto& use=*recording;
        if (!use.list || !use.executing || use.execution!=execution ||
            (use.list.Get()!=lists[i] && use.nativeList.Get()!=lists[i])) continue;
        // Called after the actual Execute, even if another thread Reset already
        // closed the recording. Signal is the only GPU completion authority.
        if (_nativeGuideFailed || !queue || use.queue.Get()!=queue ||
            queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !dlssg::depth_contract::OnDevice(_device,queue) || !use.fence || !use.value ||
            use.value==UINT64_MAX || FAILED(queue->Signal(use.fence.Get(),use.value)))
        {
            _nativeGuideFailed=true;
            _managedRetained=true;
            Deactivate(); // Existing drain cannot release this poisoned native graph.
            return;
        }
        use.executing=false; use.signalled=true;
        const auto slot=static_cast<UINT>(use.source.frame%BUFFER_COUNT);
        if (_nativeGuideFrames[slot]==&use && _guideObservations[slot].frame==use.source.frame &&
            _guideObservations[slot].snapshot.epoch==use.source.generation)
            ObserveGuideSubmission(slot,use.fence.Get(),use.value);
        if (++_nativeGuideSubmissions<=3 || _nativeGuideSubmissions%600==0)
            LOG_INFO("R1058 native FG producer submitted: pid={} frame={} serial={} source_generation={} input_epoch={} sdk_frame={} list={} native_list={} output={} producer_queue={} fence={} target={} sr_succeeded={} SDK_snapshot=ValidNow generated=UNKNOWN",
                GetCurrentProcessId(),use.source.frame,use.source.serial,use.source.generation,use.source.epoch,use.source.sdkFrame,
                (uintptr_t)use.list.Get(),(uintptr_t)use.nativeList.Get(),(uintptr_t)use.output.Get(),
                (uintptr_t)queue,(uintptr_t)use.fence.Get(),use.value,use.srSucceeded);
    }
}

void DLSSG_Dx12::NativeGuidesReset(ID3D12CommandList* list)
{
    if (!TracksNativeGuides() || !list) return;
    if (!_nativeGuideListInterest.MayContain(list)) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownQuarantined) return;
    for (auto& recording:_nativeGuideRecordings)
    {
        auto& use=*recording;
        if ((use.list.Get()==list || use.nativeList.Get()==list) && !use.closed) use.closed=true;
    }
    // Reset closes recording replay authority; it is NOT GPU completion.
}

dlssg::NativeSource DLSSG_Dx12::NativeGuideSource(ID3D12CommandList* list)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!OwnsNativeZzzSource() || !TracksNativeGuides()) return {};
    const auto* use=_nativeGuideFrames[GetIndex()];
    return use && use->list.Get()==list && !use->closed && !use->submitted &&
        NativeGuideSourceCurrent(use->source) ? use->source : dlssg::NativeSource{};
}

std::shared_ptr<DLSSG_Dx12::NativeGuideUse> DLSSG_Dx12::FindNativeGuideSource(const dlssg::NativeSource& source)
{
    if (!source) return nullptr;
    for (auto& recording:_nativeGuideRecordings)
    {
        auto& use=*recording;
        if (use.source==source && (uintptr_t)use.list.Get()==source.list &&
            (uintptr_t)use.output.Get()==source.output) return recording;
    }
    return nullptr;
}

bool DLSSG_Dx12::NativeGuideSourceCurrent(const dlssg::NativeSource& source)
{
    const auto use=FindNativeGuideSource(source);
    return use && _nativeGuideFrames[source.frame%BUFFER_COUNT]==use.get() && OwnsNativeZzzSource() && CanSubmitInputs() && !_nativeGuideFailed &&
        source.sdkFrameKnown && source.providerViewport==static_cast<uint32_t>(viewport) &&
        source.provider==reinterpret_cast<uintptr_t>(this) && use->frameScope &&
        source.upstreamViewport==use->frameScope->origin.viewport &&
        source.upstreamApi==use->frameScope->origin.api && source.sdkFrame==use->frameScope->origin.frame &&
        use->frameScope->Preparing() &&
        source.frame==_frameCount && source.generation==_inputContinuity.SourceGeneration() &&
        source.epoch==_inputOwnerEpoch && use->owner==CurrentInputOwner() &&
        use->settings.flags==_constants.flags &&
        use->settings.displayWidth==_constants.displayWidth && use->settings.displayHeight==_constants.displayHeight;
}

bool DLSSG_Dx12::NativeGuideToken(const dlssg::NativeSource& source, sl::FrameToken*& token)
{
    token=nullptr;
    if (!NativeGuideSourceCurrent(source)) return false;
    const auto getToken=StreamlineProxy::GetNewFrameToken();
    if (!dlssg::zzz::TokenFor(source.sdkFrame,token,getToken)) return false;
    // Keep only the immutable ID in the receipt, never an SDK-owned token pointer
    // beyond its API scope. Recursive vendor callbacks cannot relabel this call.
    return static_cast<uint32_t>(*token)==source.sdkFrame && NativeGuideSourceCurrent(source);
}

bool DLSSG_Dx12::NativeGuidePresentToken(sl::FrameToken*& token, dlssg::NativeSource& source)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    token=nullptr;
    const auto key=source ? source : _nativeGuideDispatch;
    const auto use=FindNativeGuideSource(key);
    if (!use || !use->dispatched || !IsActive() || IsPaused() || !NativeGuideToken(key,token) ||
        !GuideTagsCurrent(static_cast<int>(key.frame%BUFFER_COUNT),key.frame)) return false;
    source=key; // A later End/Sleep must resolve this same source, never the newest one.
    return true;
}

bool DLSSG_Dx12::BeginNativeGuidePresent(const dlssg::NativeSource& source)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    const auto use=FindNativeGuideSource(source);
    // A successful Dispatch authorizes one actual SDK Present, independently
    // of who sends Reflex markers. Failed/occluded Presents consume it too.
    if (!source || source!=_nativeGuideDispatch || !use || !use->dispatched ||
        !IsActive() || IsPaused() || !NativeGuideSourceCurrent(source) ||
        !GuideTagsCurrent(static_cast<int>(source.frame%BUFFER_COUNT),source.frame)) return false;
    return dlssg::zzz::ConsumePresentOnce(use->presented);
}

bool DLSSG_Dx12::NativeGuideBasePresentSafe()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    // Off failure deactivates logical FG but leaves this real teardown pending.
    // Its successful Off (or completed safe teardown) may permit base Present;
    // unknown Execute/Signal or quarantine never becomes safe just by going Off.
    if (!OwnsNativeZzzSource() || !dlssg::zzz::BasePresentSafe(_teardownPending,_teardownOffAccepted,
        _nativeGuideFailed || _zzzSCFailed || _hookRetiring || _ownerChanging || _teardownCompleting ||
        _teardownQuarantined || g_dlssgTeardownQuarantined.load())) return false;
    for (const auto& recording:_nativeGuideRecordings)
    {
        const auto& use=*recording;
        if (use.executing || (use.submitted && (!use.signalled || !use.queue || !use.fence ||
            !use.value || use.value==UINT64_MAX || use.fence->GetCompletedValue()==UINT64_MAX))) return false;
    }
    return true;
}

bool DLSSG_Dx12::NativeGuideGapSafe()
{
    if (!OwnsNativeZzzSource() || !CanSubmitInputs() || _nativeGuideFailed || _zzzSCFailed ||
        !_device || !_gameCommandQueue || _device->GetDeviceRemovedReason()!=S_OK) return false;
    for (const auto& recording:_nativeGuideRecordings)
    {
        const auto& use=*recording;
        if (use.executing || (use.submitted && (!use.signalled || !use.queue || !use.fence ||
            !use.value || use.value==UINT64_MAX || use.fence->GetCompletedValue()==UINT64_MAX))) return false;
    }
    return true; // A known pending fence retains its owners; Off does not retire it.
}

bool DLSSG_Dx12::PrepareNativeGuideGapPresent()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    const auto owner=CurrentInputOwner();
    const auto localViewport=static_cast<uint32_t>(viewport);
    const auto stillSafe=[&] {
        return NativeGuideGapSafe() && CurrentInputOwner()==owner &&
            static_cast<uint32_t>(viewport)==localViewport;
    };
    if (!stillSafe()) { _nativeGapOffProof.Invalidate(); return false; }
    // Revoke consumption only. Keep all native recordings/resources and their
    // real queue/fence receipts, including work which has not been submitted.
    _depthSourceRejected=true;
    _guideTags[GetIndex()][0].accepted=false;
    _guideTags[GetIndex()][1].accepted=false;
    _nativeGuideDispatch={};
    const bool off=_nativeGapOffProof.Ensure(owner,localViewport,[&] {
        InvalidateDispatchOptions();
        auto options=_lastRunningOptions;
        options.mode=sl::DLSSGMode::eOff;
        options.flags|=sl::DLSSGFlags::eRetainResourcesWhenOff;
        const auto setOptions=StreamlineProxy::DLSSGSetOptions();
        return setOptions && StreamlineHooks::SubmitDlssgOptions(setOptions,viewport,options)==sl::Result::eOk;
    },stillSafe);
    if (off) _depthSourceGap=true;
    else if (OwnsNativeZzzSource() && CurrentInputOwner()==owner)
    {
        // An unsuccessful/unknown Off must not be retried into a fake safe
        // result on this Present stack. Existing teardown retains the owners.
        _managedRetained=true;
        Deactivate();
    }
    return off;
}

void DLSSG_Dx12::CompleteNativeGuideSource(ID3D12CommandList* list, const dlssg::NativeSource& source,
    ID3D12Resource* output, bool succeeded)
{
    if (!source || !TracksNativeGuides()) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    // Evaluate may span a newer source/marker. Resolve the original transaction,
    // never the current ring slot, and never grant it a newer frame identity.
    const auto use=FindNativeGuideSource(source);
    if (!use || use->list.Get()!=list || use->closed || use->submitted) return;
    use->srSucceeded=succeeded && output==use->output.Get() &&
        source.epoch==_inputOwnerEpoch && source.generation==_inputContinuity.SourceGeneration();
    const auto colourSlot=static_cast<UINT>(source.frame%BUFFER_COUNT);
    if (_nativeGuideFrames[colourSlot]!=use.get()) return; // Old completion cannot touch a newer frame.
    const auto colourSource=_upscalerColourSources[colourSlot];
    if (UpscalerColourCurrent(colourSource) && !colourSource->bridge &&
        colourSource->key.frame==source.frame && colourSource->list==source.list &&
        colourSource->key.output==source.output)
        _upscalerColourSucceeded[colourSlot]=use->srSucceeded;
    if (use->srSucceeded)
    {
        try
        {
            const auto slot=colourSlot;
            auto& snapshot=_guideObservations[slot].snapshot;
            if (colourSource && _guideObservations[slot].frame==source.frame && snapshot.epoch==source.generation)
            {
                const auto& desc=colourSource->outputDescription;
                snapshot.colour={true,static_cast<UINT>(desc.Format),desc.Width,desc.Height,
                    {0,0,static_cast<UINT>(desc.Width),desc.Height}};
                snapshot.colourChoice="same selected SR transaction output identity; final present-colour match unverified";
            }
        }
        catch (...) {}
    }
    if (!use->srSucceeded && source.frame==_frameCount)
        RefuseDepthContract("native-selected-sr-failed-or-output-changed",false);
}

bool DLSSG_Dx12::OrderNativeGuides(UINT slot)
{
    if (!OwnsNativeZzzSource()) return true; // Existing wDX12 UI submission path.
    const auto recording=FindNativeGuideSource(NativeGuideFrameSource(slot));
    if (!recording) return false;
    auto& use=*recording;
    if (!NativeGuideSourceCurrent(use.source)) return false;
    if ((use.fence && use.fence->GetCompletedValue()==UINT64_MAX) ||
        (_device && _device->GetDeviceRemovedReason()!=S_OK))
    {
        _nativeGuideFailed=true; _managedRetained=true; Deactivate();
        return false;
    }
    if (!use.frameScope || !use.frameScope->Consumable(use.srSucceeded) || !use.submitted || !use.signalled ||
        !use.queue || !use.fence || !_gameCommandQueue || !use.value || use.value==UINT64_MAX) return false;
    if (use.ordered) return true;
    // Same-queue FIFO already orders the producer and SDK snapshots. A different
    // actual producer queue needs its real post-Execute fence, never the UI fence.
    const auto presentingQueue=dlssg::depth_contract::NativeObject(_gameCommandQueue);
    if (!dlssg::depth_contract::SameOwner(use.queue.Get(),presentingQueue.Get()) &&
        FAILED(_gameCommandQueue->Wait(use.fence.Get(),use.value)))
    {
        _nativeGuideFailed=true; _managedRetained=true; Deactivate();
        return false;
    }
    use.ordered=true;
    return true;
}

bool DLSSG_Dx12::GuideTagsCurrent(int slot, UINT64 frame)
{
    if (!OwnsDepthContract()) return false;
    const auto owner=CurrentInputOwner();
    if (slot<0 || slot>=BUFFER_COUNT || frame!=_frameCount ||
        !_inputContinuity.Current(slot,frame,owner)) return false;
    const auto* native=_nativeGuideFrames[slot];
    const bool nativeZzz=OwnsNativeZzzSource();
    if (nativeZzz && (!native || !NativeGuideSourceCurrent(native->source) ||
        !native->frameScope || !native->frameScope->Consumable(native->srSucceeded) ||
        !native->srSucceeded || !native->submitted || !native->signalled || !native->ordered)) return false;
    std::shared_lock resources(_resourceMutex[slot]);
    for (auto type : {FG_ResourceType::Depth,FG_ResourceType::Velocity})
    {
        const auto& tag=_guideTags[slot][type==FG_ResourceType::Depth ? 0 : 1];
        const auto found=_frameResources[slot].find(type);
        if (!tag.accepted || tag.frame!=frame || (nativeZzz && tag.nativeSerial!=native->source.serial) ||
            tag.generation!=_inputContinuity.SourceGeneration() || !(tag.owner==owner) ||
            found==_frameResources[slot].end()) return false;
        const auto& a=found->second; const auto& b=tag.input;
        // No borrowed-resource dereference. eOnlyValidNow requests the SDK's
        // source-lifetime contract, not proof of a GPU copy. UntilPresent is
        // a sealed Genshin lease or a StarRail-owned presenting-queue output.
        if (a.resource!=b.resource || a.copy!=b.copy || a.cmdList!=b.cmdList ||
            a.state!=b.state || a.validity!=b.validity || a.left!=b.left || a.top!=b.top ||
            a.width!=b.width || a.height!=b.height || a.preparedRasterGuides!=b.preparedRasterGuides ||
            !dlssg::depth_contract::Region(tag.description,a.left,a.top,a.width,a.height)) return false;
        if (a.validity==FG_ResourceValidity::ValidNow && !a.cmdList) return false;
        if (a.cmdList && a.cmdList==_uiCommandList[slot] &&
            (_uiCommandListResetted[slot] || _uiCloseFailed[slot] || !_uiFence ||
             !_uiAllocatorFenceValues[slot] || _uiFence->GetCompletedValue()==UINT64_MAX)) return false;
        if (a.validity!=FG_ResourceValidity::ValidNow && !false)
        {
            if (dlssg::depth_contract::Game()!=GameRouting::Identity::StarRail ||
                !_starRailGuideQueueCurrent || a.cmdList!=_uiCommandList[slot]) return false;
            const auto* scaled=_guideDepthScale[slot].get();
            const bool ownedCopy=a.copy && _resourceCopy[slot].contains(type) &&
                _resourceCopy[slot].at(type)==a.copy;
            const bool ownedScale=type==FG_ResourceType::Depth && scaled &&
                _guideDepthScale[slot]->Buffer()==a.resource;
            if (!ownedCopy && !ownedScale) return false;
        }
    }
    const auto& depth=_guideTags[slot][0].input;
    const auto& motion=_guideTags[slot][1].input;
    return (depth.copy ? depth.copy : depth.resource)!=(motion.copy ? motion.copy : motion.resource);
}

bool DLSSG_Dx12::BuildGuideConstants(int slot, sl::Constants& constData, bool includeCamera)
{
    constData = {};

    if (IsInfiniteDepth() && _cameraFar[slot] > _cameraNear[slot])
        _cameraFar[slot] = std::numeric_limits<float>::infinity();
    else if (IsInfiniteDepth() && _cameraNear[slot] > _cameraFar[slot])
        _cameraNear[slot] = std::numeric_limits<float>::infinity();

    if (includeCamera)
    {
        if (_cameraPosition[slot][0] != 0.0f || _cameraPosition[slot][1] != 0.0f || _cameraPosition[slot][2] != 0.0f)
        {
            constData.cameraPos.x = _cameraPosition[slot][0];
            constData.cameraPos.y = _cameraPosition[slot][1];
            constData.cameraPos.z = _cameraPosition[slot][2];

            constData.cameraUp.x = _cameraUp[slot][0];
            constData.cameraUp.y = _cameraUp[slot][1];
            constData.cameraUp.z = _cameraUp[slot][2];

            constData.cameraFwd.x = _cameraForward[slot][0];
            constData.cameraFwd.y = _cameraForward[slot][1];
            constData.cameraFwd.z = _cameraForward[slot][2];

            constData.cameraRight.x = _cameraRight[slot][0];
            constData.cameraRight.y = _cameraRight[slot][1];
            constData.cameraRight.z = _cameraRight[slot][2];
        }
        else
        {
            constData.cameraPos = { 0.0f, 0.0f, 0.0f };
            constData.cameraUp = { 0.0f, 0.0f, 1.0f };
            constData.cameraRight = { 0.0f, 1.0f, 0.0f };
            constData.cameraFwd = { 1.0f, 0.0f, 0.0f };
            constData.cameraPinholeOffset = { 0.0f, 0.0f };

            XMMATRIX cameraViewToClip {};

            // XMMatrixPerspectiveFovRH will fail if input values are incorrect
            if (_cameraNear[slot] > 0.f && _cameraFar[slot] > 0.f &&
                !XMScalarNearEqual(_cameraVFov[slot], 0.0f, 0.00001f) &&
                !XMScalarNearEqual(_cameraAspectRatio[slot], 0.0f, 0.00001f))
            {
                if (XMScalarNearEqual(_cameraNear[slot], _cameraFar[slot], 0.00001f))
                    _cameraFar[slot]++;

                cameraViewToClip = XMMatrixPerspectiveFovRH(_cameraVFov[slot], _cameraAspectRatio[slot],
                                                            _cameraNear[slot], _cameraFar[slot]);
            }
            else
            {
                LOG_WARN("Can't calculate projectionMatrix");
            }

            XMMATRIX clipToCameraView = XMMatrixInverse(nullptr, cameraViewToClip);

            auto prev = XMMatrixIdentity();

            // Convert to sl::float4x4 for Streamline
            XMFLOAT4X4 temp;
            XMStoreFloat4x4(&temp, cameraViewToClip);
            memcpy(&constData.cameraViewToClip, &temp, sizeof(sl::float4x4));
            XMStoreFloat4x4(&temp, clipToCameraView);
            memcpy(&constData.clipToCameraView, &temp, sizeof(sl::float4x4));

            XMStoreFloat4x4(&temp, prev);
            memcpy(&constData.clipToLensClip, &temp, sizeof(sl::float4x4));
            memcpy(&constData.clipToPrevClip, &temp, sizeof(sl::float4x4));
            memcpy(&constData.prevClipToClip, &temp, sizeof(sl::float4x4));
        }

    }

    constData.cameraAspectRatio = _cameraAspectRatio[slot];
    constData.cameraFOV = _cameraVFov[slot];
    constData.cameraNear = _cameraNear[slot];
    constData.cameraFar = _cameraFar[slot];

    constData.jitterOffset.x = _jitterX[slot];
    constData.jitterOffset.y = _jitterY[slot];

    {
        auto mv = GetResource(FG_ResourceType::Velocity, slot);

        if (!mv)
        {
            LOG_ERROR("Motion vectors missing for: {}", slot);

            return false;
        }

        constData.mvecScale.x = _mvScaleX[slot] / (float) mv->width;
        constData.mvecScale.y = _mvScaleY[slot] / (float) mv->height;
    }

    // LOG_DEBUG("MvRes: {}x{}, Games MvScale : {}x{}, SL MvScale: {}x{}", mv->width, mv->height, _mvScaleX[slot],
    //           _mvScaleY[slot], constData.mvecScale.x, constData.mvecScale.y);

    // if (State::Instance().currentFeature != nullptr)
    //{
    //     auto fResX = State::Instance().currentFeature->RenderWidth();
    //     auto fResY = State::Instance().currentFeature->RenderHeight();

    //    LOG_DEBUG("Feature LowResMV: {} RenderRes : {}x{}, CMvScale: {}x{}",
    //              State::Instance().currentFeature->LowResMV(), fResX, fResY, 1.0f / (float) fResX,
    //              1.0f / (float) fResY);
    //}

    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.reset = _reset[slot] != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    else
        constData.reset = sl::Boolean::eFalse;

    constData.depthInverted = IsInvertedDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constData.cameraMotionIncluded = sl::Boolean::eTrue;
    constData.motionVectors3D = sl::Boolean::eFalse;
    // constData.motionVectorsInvalidValue = 0.0f;
    constData.orthographicProjection = sl::Boolean::eFalse;
    constData.motionVectorsDilated = IsLowResMV() ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    constData.motionVectorsJittered = IsJitteredMVs() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    return true;
}
