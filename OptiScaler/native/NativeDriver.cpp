#include "pch.h"
#include <integration/HoYoShade.h>
#include <integration/HoYoShadeNaturalAssociation.h>
#include <integration/HoYoShadeColorContract.h>
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "NativeDriver.h"
#include <ui_mask/DrawUiMask.h>
#include "NrConsumerFilter.h"
#include "NativeWorkDevice.h"
#include <genshin/GenshinPresentNr.h>
#include <inputs/FG/GenshinBridge_Present_Dx12.h>
#include "OpticalFgNrPolicy.h"
#include "VirtualUpscalerDriver.h"
#include <resource_tracking/ResTrack_Dx12.h>

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

#include <imgui/imgui.h>
#include <menu/UiLocalization.h>

namespace native
{

namespace
{
constexpr float kPreviewMaxSpeed = 24.0f; // pixels per frame that show as full brightness

// Consumer ownership is independent of producer/color liveness. Keep the
// maintenance gate and RunFrame's actual virtual-consumer choice identical.
bool VirtualUpscalerRequested()
{
    return !GenshinPresentNr::Scope::Current() &&
        Config::Instance()->DlssNrNativeMotion.value_or_default() &&
        Config::Instance()->DlssNrNativeUpscaler.value_or_default();
}

NrInputObservation::SourceDepth ObserveAcquiredDepth(const FrameInput& input) noexcept
{
    using Reason = NrInputObservation::SourceDepth;
    if (input.depthCount > 0) return Reason::Supplied;
    const auto* presented = GenshinPresentNr::Scope::Current();
    if (!presented) return Reason::SourceHasNoDepth;
    const auto& receipt = presented->ticket.depth;
    if (!receipt.source) return Reason::MissingPresentReceipt;
    // Do not poll Known()/Ready() again for a diagnostic or try to open an FG
    // native guide as NR depth. Acquire/BindInput already made the real decision.
    if (!receipt.lease)
        return receipt.nativeGuide ? Reason::NativeGuideWithoutMappedLease : Reason::MissingMappedLease;
    return Reason::MappedDepthNotAcquiredOrBound;
}
} // namespace

void NativeDriver::ReleaseVirtualUpscaler()
{
    // A failed prefix/filter/continuation retains the ENTIRE virtual graph,
    // including parameters and provider state. Disable/takeover cannot turn
    // an intermediate Signal or CPU return into final GPU completion.
    if (_virtualUpscaler == nullptr || !_virtualUpscaler->Active()) return;
    if (_producer && _producer->BeforeSrPending()) return;
    _virtualUpscaler->Release();
}

void NativeDriver::PauseVirtualUpscaler()
{
    if (_virtualUpscaler != nullptr)
        _virtualUpscaler->Pause();
}

void NativeDriver::PublishSceneCuts(bool running, const char* failure)
{
    DlssNr::SceneCutInfo info;
    info.nativeRoute = true;
    info.mode = static_cast<unsigned>(DlssNrSceneCut::ModeFrom(Config::Instance()->DlssNrSceneCut.value_or_default()));
    info.running = running && info.mode != 0;
    info.failed = failure != nullptr;
    if (failure) info.reason = failure;
    if (_producer)
    {
        const auto& policy = _producer->SceneCuts();
        const auto& counts = policy.Counts();
        info.found = counts.found; info.flagged = counts.flagged; info.silent = counts.silent;
        info.requested = counts.resets; info.resets = policy.resets;
        info.stale = policy.stale; info.lateBy = policy.lateBy; info.lateByKnown = policy.lateByKnown;
        info.readbackFailures = _producer->SceneCutReadbackFailures();
        info.quarantined = _producer->Quarantined();
    }
    DlssNr::PublishNativeSceneCutStatus(info);
}

bool NativeDriver::Enabled()
{
    // FinalOutput/BeforeNR can keep color processing alive after NativeMotion
    // exits. Retire an old virtual SR/FG consumer even if the API adapter will
    // not request a new producer frame (and never reaches RunFrame's fallback).
    if (!VirtualUpscalerRequested())
        ReleaseVirtualUpscaler();

    if (!GenshinPresentNr::Scope::Current() && !Config::Instance()->DlssNrNativeMotion.value_or_default() &&
        !HoYoShade::Selected(HoYoShade::Stage::BeforeNr) && !HoYoShade::Selected(HoYoShade::Stage::FinalOutput))
    {
        _status = Status::Off;
        if (_producer) _producer->Reset();
        PublishSceneCuts(false);
        return false;
    }

    return true;
}

void NativeDriver::LogDiagnostics()
{
    if (_diagPresents % 300 != 0)
        return;

    LOG_INFO("{} frames: of {} presents, picture ready {}, waiting for the game's upscaler {}, "
             "submitted {}, flow valid {} (matched with depth {}), trust mask ran {}, native input applied {}, scene "
             "cuts {}, virtual upscaler {}",
             _tag, _diagPresents, _diagReady, _diagWaiting, _diagSubmitted, _diagFlowValid, _diagDepth, _diagTrust,
             _diagNative, _cuts, _virtualUpscaler != nullptr && _virtualUpscaler->Active() ? "active" : "not active");
}

NativeDriver::RunResult NativeDriver::RunFrame(IFrameSource& source, bool flowPreview, const GenshinBridgeFg::SenderInput* fgSender)
{
    RunResult out;
    if(fgSender) {
        auto* presented=GenshinPresentNr::Scope::Current();auto* wrapper=GenshinBridgeFg::PresentScope::Current();
        if(!fgSender->Known() || !presented || !wrapper || fgSender->pictureKind!=presented->FgKind() ||
           fgSender->pictureKind!=GenshinBridgeFg::CurrentPictureKind() ||
           GenshinBridgeFg::MakeKey(fgSender->ticket)!=GenshinBridgeFg::MakeKey(presented->ticket) ||
           fgSender->provider!=reinterpret_cast<uintptr_t>(State::Instance().currentFG) ||
           fgSender->providerSwapchain!=GenshinPresentNr::Identity(State::Instance().currentFGSwapchain)) {
            // An optional sender can become stale after ticket acquisition. Its
            // failure cannot revoke an independently current ordinary NR ticket.
            // The actual callback still needs the private native-parent capability.
            if(!presented || !GenshinPresentNr::MatchesNativeParentTicket(presented->ticket))return out;
            fgSender=nullptr;
        }
    }
    std::unique_ptr<GenshinBridgeFg::Recording> fgRecording;

    // Observe mode transitions even when Acquire is not ready; a waiting
    // native route must not retain an old Act request across menu changes.
    const bool useVirtualUpscaler = VirtualUpscalerRequested();
    const bool supportedSceneRoute = source.GetApi() != Api::Vulkan && State::Instance().api != Vulkan &&
        State::Instance().swapchainApi != Vulkan && State::Instance().swapchainInteropApi != SwapchainInteropApi::VkwDx12;
    if (_producer) _producer->SceneCutMode(!useVirtualUpscaler && supportedSceneRoute
        ? DlssNrSceneCut::ModeFrom(Config::Instance()->DlssNrSceneCut.value_or_default())
        : DlssNrSceneCut::Mode::Off);
    FrameInput input;
    const auto acquired = source.Acquire(input);
    if(auto* r=GenshinPresentNrTrace::Current())r->acquireStatus=static_cast<uint32_t>(acquired);

    ++_diagPresents;
    _diagReady += acquired == AcquireStatus::Ready ? 1 : 0;
    _diagWaiting += acquired == AcquireStatus::WaitingForUpscaler ? 1 : 0;

    const bool gameTookOver = _takeover.Update(acquired == AcquireStatus::WaitingForUpscaler, Util::MillisecondsNow());

    if (acquired == AcquireStatus::WaitingForUpscaler)
    {
        // The game's own upscaler call: ours pauses, and goes (with frame generation's context, as a game's feature
        // would) only once the game's calls have run for a while. A menu or cutscene that calls it for a moment would
        // otherwise cost a rebuild and a gap in frame generation each time (VirtualUpscalerHold.h).
        if (gameTookOver)
            ReleaseVirtualUpscaler();
        else
            PauseVirtualUpscaler();

        _status = Status::Waiting;

        if (_producer)
            _producer->Reset();

        PublishSceneCuts(false);
        LogDiagnostics();
        return out;
    }

    if (acquired != AcquireStatus::Ready)
    {
        PublishSceneCuts(false);
        LogDiagnostics();
        return out;
    }

    const bool virtualNativeNr = useVirtualUpscaler && OpticalFgNrEnabled();
    // The generic DX11 FG caller supplies an exact current parent only for this
    // synchronous invocation. The copyBack=false adapter intentionally has no
    // Source; capture a fresh proof using its authentic device/buffer identities.
    if (!input.naturalSource && input.api == Api::D3D11 && virtualNativeNr &&
        HoYoShade::Selected(HoYoShade::Stage::BeforeNr))
        input.naturalSource = nrfilter::OpticalInputScope::Capture(input.naturalDevice,
            input.naturalBackbuffer, reinterpret_cast<uintptr_t>(&source), input.frame);

    // End association on every return/exception. The producer's retained
    // FrameInput must never retain this natural swapchain/resource proof.
    // Like natural association, final-B ownership is synchronous until an
    // admitted transaction explicitly retains it. Do not pin a swapchain B in
    // NativeProducer's unrelated multi-frame HeldFrame ring.
    auto beforeNrNativeColor = std::move(input.beforeNrNativeColor);
    HoYoShade::NaturalAssociation::Source::Scope naturalSource(std::move(input.naturalSource));
    HoYoShade::BindNaturalSource(naturalSource.Get());

    // Capture the real submission queue once for this acquired frame. Its color
    // and public device must agree before any producer GPU object is created.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> workQueue = source.Queue();
    auto* queue = workQueue.Get();
    NativeWorkDevice work;
    if (!work.Resolve(input.picture, queue))
    {
        static thread_local unsigned reports = 0;
        if (reports++ < 8)
            LOG_WARN("{}: R041 native work refused: {} hr={:08X} source_device={} work_device={} queue_device={} queue={}",
                _tag, work.failure, static_cast<unsigned>(work.hr), static_cast<void*>(source.Device()),
                static_cast<void*>(work.device.Get()), static_cast<void*>(work.queueDevice.Get()), static_cast<void*>(queue));
        PublishSceneCuts(false, work.failure);
        source.Return(input, FrameOutput {});
        return out;
    }
    ID3D12Device* device = work.device.Get();

    if (_producer == nullptr || _producer->Device() != device)
    {
        auto fresh = std::make_unique<NativeProducer>();

        const double initStart = Util::MillisecondsNow();

        if (!fresh->Init(work, !input.colorOnly || fgSender != nullptr))
        {
            _failure = fresh->Error();
            _status = Status::Failed;
            static thread_local unsigned failures=0;
            if (failures++<8) LOG_ERROR("{}: R041 native creation failed: {} receipts={}",_tag,_failure,fresh->CreationReport());
            PublishSceneCuts(false, _failure.c_str());
            source.Return(input, FrameOutput {});
            return out;
        }

        static thread_local unsigned creationReports=0;
        if (creationReports++<4) LOG_INFO("{}: R041 native creation accepted queue={} creator={} receipts={}",
            _tag,static_cast<void*>(queue),static_cast<void*>(device),fresh->CreationReport());
        // A device replacement is not proof that the old queue completed.
        const bool oldComplete = !_producer || _producer->CanRetire();
        if (!oldComplete)
        {
            LOG_WARN("{}: retaining previous-device native GPU owners without completion proof", _tag);
            _producer.release(); // process-lifetime quarantine, never destroy in-flight inputs
        }
        // The upscaler backend belongs to the old device too.
        if (_virtualUpscaler != nullptr && oldComplete)
        {
            delete _virtualUpscaler;
            _virtualUpscaler = nullptr;
        }

        if (!oldComplete) _virtualUpscaler = nullptr; // retain its owners as well
        _producer = std::move(fresh);
        LOG_INFO("{}: optical flow and trust mask ready{} (in {:.1f} ms, on the present thread)", _tag, _readyNote,
                 Util::MillisecondsNow() - initStart);
    }

    // Presents the picture to an upscaler backend as a synthetic call instead of running DLSS-NR on it. Takes priority
    // when both are on. Under a D3D11 game's frame generation swap chain (Dx11wDx12SC) it runs from OnFGPresent instead
    // of the menu's present.
    if (useVirtualUpscaler && _virtualUpscaler == nullptr)
        _virtualUpscaler = new VirtualUpscalerDriver();
    else if (!useVirtualUpscaler)
        ReleaseVirtualUpscaler();

    NativeProducer::Options options;
    options.sourceDepth = ObserveAcquiredDepth(input);
    options.apply = GenshinPresentNr::Scope::Current() || useVirtualUpscaler || Config::Instance()->DlssNrNativeInput.value_or_default();
    const bool beforeNr = (!useVirtualUpscaler || virtualNativeNr) &&
        (input.api == Api::D3D12 || input.finalOriginal || virtualNativeNr) &&
        HoYoShade::Selected(HoYoShade::Stage::BeforeNr) &&
        (!Config::Instance()->DlssNrEnabled.value_or_default() || Config::Instance()->DlssNrFinishedPicture.value_or_default());
    const bool beforeSr = useVirtualUpscaler && !virtualNativeNr && input.api == Api::D3D12 &&
        !GenshinPresentNr::Scope::Current() && input.colorTransferKnown && input.colorSpace == ColorSpace::Srgb &&
        HoYoShade::ColorContract::Resolve(input.pictureFormat) &&
        HoYoShade::NativeBeforeSrAvailable(naturalSource.Get());
    const bool finalOriginal = !useVirtualUpscaler && input.api == Api::D3D11 && input.finalOriginal &&
        HoYoShade::Selected(HoYoShade::Stage::FinalOutput);
    // Authentic FG-original guide production still needs its existing flow
    // and capture callback with NR off; a color filter cannot replace it.
    options.colorOnly = input.colorOnly && !fgSender && !beforeSr;
    const auto filter = [&source, queue, &naturalSource, &beforeNrNativeColor](HoYoShade::Stage stage, const FrameInput& color, SyncPoint ready) {
        HoYoShade::ColorEndpoint endpoint;
        endpoint.stage = stage; endpoint.color = color.picture; endpoint.queue = queue;
        endpoint.uiMask = color.uiMask;
        endpoint.ready = {queue, ready.fence, ready.value, ready.submission,
            ready.submission ? ready.submission->Generation() : 0, ready.submission ? ready.submission->Batch() : 0};
        endpoint.stream = reinterpret_cast<uint64_t>(&source); endpoint.generation = 1;
        endpoint.frame = color.frame; endpoint.width = color.width; endpoint.height = color.height;
        endpoint.state = color.pictureState; endpoint.stateKnown = true;
        endpoint.sdr = color.colorTransferKnown && color.colorSpace == ColorSpace::Srgb;
        endpoint.naturalDevice = color.naturalDevice; endpoint.naturalBackbuffer = color.naturalBackbuffer;
        endpoint.naturalSource = naturalSource.Get();
        if (stage == HoYoShade::Stage::BeforeSr)
            endpoint.naturalContract = HoYoShade::NaturalSourceContract::BeforeSrNativeProducer;
        endpoint.beforeNrNativeColor.observation = color.nativePictureColor;
        const auto* presentColorScope = GenshinPresentNr::Scope::Current();
        const auto& colorConfig = *Config::Instance();
        if (stage == HoYoShade::Stage::BeforeNr && color.api == Api::D3D11 && color.finalOriginal &&
            beforeNrNativeColor && presentColorScope && colorConfig.DlssNrEnabled.value_or_default() &&
            colorConfig.DlssNrFinishedPicture.value_or_default() && !colorConfig.DlssNrRunBeforeSr.value_or_default() &&
            !colorConfig.DlssNrDeferredDlss.value_or_default() &&
            colorConfig.GenshinControlsCurrent(beforeNrNativeColor->ConfigEpoch()) &&
            presentColorScope->MatchesNativeColor(beforeNrNativeColor, endpoint.color, endpoint.queue,
                endpoint.stream, endpoint.generation, endpoint.frame, endpoint.naturalDevice,
                endpoint.naturalBackbuffer, endpoint.width, endpoint.height, color.nativePictureColor))
        {
            endpoint.colorMode = HoYoShade::ColorMode::GenshinNativeTypedBeforeNr;
            endpoint.beforeNrNativeColor.picture = beforeNrNativeColor;
            endpoint.sdr = false; // Native values/UNKNOWN transfer, never an SDR assertion.
        }
        const char* finalDepthReason = "not-requested";
        if (stage == HoYoShade::Stage::BeforeNr)
            endpoint.depth = HoYoShade::Depth::FromNative(color, queue, ready,
                endpoint.stream, endpoint.generation);
        else if (stage == HoYoShade::Stage::FinalOutput)
        {
            // Only this acquired copyback picture can supply final depth. The
            // live ticket must still own the exact source/picture/epoch and
            // reader admitted by BindInput; no earlier-stage depth is reused.
            const auto* presented = GenshinPresentNr::Scope::Current();
            if (color.api != Api::D3D11 || !color.finalOriginal)
                finalDepthReason = "not-d3d11-copyback";
            else if (color.depthCount != 1 || !color.depth[0])
                finalDepthReason = "acquired-without-single-depth";
            else if (!presented)
                finalDepthReason = "no-present-depth-owner";
            else
            {
                const auto& ticket = presented->ticket;
                const bool samePicture = ticket.DepthMatches() &&
                    color.width == ticket.pictureWidth && color.height == ticket.pictureHeight &&
                    color.depthGeneration == ticket.source.generation &&
                    color.depthSourceFrame == ticket.source.sourceFrame &&
                    color.depthPictureSerial == ticket.pictureSerial &&
                    color.depthEpoch == ticket.source.historyEpoch &&
                    color.depthLease == GenshinTransport::NativeDepthLease(ticket.depth);
                if (!samePicture)
                    finalDepthReason = "present-depth-owner-or-picture-mismatch";
                else
                {
                    // FromNative rechecks the content-owning reader, current
                    // producer submission, queue/fences and resource/state.
                    endpoint.depth = HoYoShade::Depth::FromNative(color, queue, ready,
                        endpoint.stream, endpoint.generation);
                    finalDepthReason = endpoint.depth.resource ? "owned-depth-binding-not-confirmed" :
                        "native-depth-reader-or-resource-refused";
                }
            }
        }
        const auto result = HoYoShade::RenderStage(endpoint);
        if (stage == HoYoShade::Stage::FinalOutput && !result.depthAvailable)
        {
            static thread_local uint64_t lastFinalDepthReport = 0;
            const auto now = GetTickCount64();
            if (!lastFinalDepthReport || now - lastFinalDepthReport >= 30000)
            {
                lastFinalDepthReport = now;
                LOG_INFO("HoYoShade final-original depth unavailable: frame={} reason={} depth_count={} generation={} source_frame={} picture={} epoch={} endpoint_depth={} stage_reason={} effects_executed={} fallback=color-only",
                    color.frame, finalDepthReason, color.depthCount, color.depthGeneration, color.depthSourceFrame,
                    color.depthPictureSerial, color.depthEpoch, endpoint.depth.resource != nullptr,
                    static_cast<unsigned>(result.reason), result.EffectsExecuted());
            }
        }
        const bool invalidBeforeSrTail = stage == HoYoShade::Stage::BeforeSr && result.Submitted() &&
            (!result.completion.fence || !result.completion.value || result.completion.value == UINT64_MAX ||
             result.completion.fence->GetCompletedValue() == UINT64_MAX);
        return NativeProducer::FilterResult{result.PictureChanged(), result.Uncertain() || invalidBeforeSrTail};
    };
    if (beforeSr) options.beforeSr = [filter](const FrameInput& color, SyncPoint ready) {
        return filter(HoYoShade::Stage::BeforeSr, color, ready);
    };
    if (beforeNr) options.beforeNr = [filter](const FrameInput& color, SyncPoint ready) {
        return filter(HoYoShade::Stage::BeforeNr, color, ready);
    };
    if (finalOriginal) options.finalOriginal = [filter](const FrameInput& color, SyncPoint ready) {
        return filter(HoYoShade::Stage::FinalOutput, color, ready);
    };
    options.submitProducerBeforeConsumer = beforeNr;
    options.flowPreview = flowPreview;
    options.previewMaxSpeed = kPreviewMaxSpeed;
    options.explicitConsumerReset = GenshinPresentNr::Scope::Current() && GenshinPresentNr::Scope::Current()->reset;
    options.sceneCutPolicy = !useVirtualUpscaler; // Off also suppresses optional NR resets on unsupported origins
    options.sceneCutMode = supportedSceneRoute
        ? DlssNrSceneCut::ModeFrom(Config::Instance()->DlssNrSceneCut.value_or_default())
        : DlssNrSceneCut::Mode::Off;

    const auto captureGameMotion = [&](ID3D12GraphicsCommandList* cmd,const NativeFrame& frame) {
        if(!fgSender)return;
        auto motion=input.gameMotion;
        const bool imported=motion && motion==fgSender->ticket.gameMotion &&
            motion->ImportReady(device,queue);
        const auto reason=motion?motion->Status():fgSender->ticket.gameMotionStatus;
        static unsigned motionReports=0;static uint64_t seen=0;
        const unsigned reasonIndex=static_cast<unsigned>(reason);
        if(motionReports<32 && (motionReports<4 || !(seen&(1ull<<reasonIndex)))) {
            ++motionReports;seen|=1ull<<reasonIndex;
            const std::array<uint32_t,16> empty{};
            const auto& status=motion?motion->Certificate():empty;
            LOG_INFO("R827 game motion certificate: pid={} generation={} source_frame={} picture={} imported={} reason={} counts={},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{} sentinel=65504 source=actual_PreparedMotion no_optical_fallback=true",
                GetCurrentProcessId(),fgSender->ticket.source.generation,fgSender->ticket.source.sourceFrame,
                fgSender->ticket.pictureSerial,imported,GenshinGameMotion::Text(reason),
                status[0],status[1],status[2],status[3],status[4],status[5],status[6],status[7],
                status[8],status[9],status[10],status[11],status[12],status[13],status[14],status[15]);
        }
        if(!imported || !motion->BeginRead(cmd,queue))return;
        // End barriers are mandatory even if optional recording allocation fails.
        struct EndRead {GenshinGameMotion::Lease& lease;ID3D12GraphicsCommandList* list;
            ~EndRead(){lease.EndRead(list);}} end{*motion,cmd};
        NativeFrame fgFrame=frame;fgFrame.motion=motion->Motion();fgFrame.depth=motion->Depth();
        fgFrame.depthReversed=fgSender->ticket.depth.convention==GenshinGuides::DepthConvention::DeviceZReversed;
        try {fgRecording=GenshinBridgeFg::Recording::Capture(cmd,queue,frame,fgFrame,*fgSender);}
        catch(const std::bad_alloc&) {fgRecording.reset();}
    };
    const auto apply = [this, &source, queue, useVirtualUpscaler, fgSender, &fgRecording, &captureGameMotion, uiMask=input.uiMask, importedDepth=input.depthResource](ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
    {
        if (useVirtualUpscaler)
            return _virtualUpscaler->Run(cmd, frame);

        if(fgSender && GenshinPresentNr::Original(fgSender->pictureKind)) {
            auto* presented=GenshinPresentNr::Scope::Current();
            if(fgSender->pictureKind!=GenshinBridgeFg::CurrentPictureKind() || !presented ||
               !presented->ObserveOriginal(cmd,queue,frame))return false;
            if(fgSender->camera) {
                captureGameMotion(cmd,frame);
            }
            // NativeProducer still submits the actual guides and signals its
            // real fence. processed/nativeRan remain false; no NR call or copy.
            return false;
        }
        if(auto* presented=GenshinPresentNr::Scope::Current()) {
            if(!presented->MatchesConsumer(cmd,queue,frame))return false;
            presented->consumerReached=true;
            if(auto* r=GenshinPresentNrTrace::Current()){r->consumer=true;r->consumerSeq=GenshinPresentNrTrace::Sequence();
                r->queue=reinterpret_cast<uintptr_t>(queue);r->list=reinterpret_cast<uintptr_t>(cmd);r->nativeReset=frame.reset;}
            GenshinSubstage::Event(GenshinSubstage::Stage::NrGuides,GenshinSubstage::State::Succeeded,
                GenshinSubstage::Resource::Nr,GenshinSubstage::Domain::Nr);
        }
        const DXGI_COLOR_SPACE_TYPE type = frame.space == ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                           : frame.space == ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        const auto* presented=GenshinPresentNr::Scope::Current();
        const uint64_t epoch = presented?presented->ticket.presentEpoch:
            PresentEpoch(source.GetApi(), State::Instance().frameCount,
                         State::Instance().vulkanPresentCount.load(std::memory_order_relaxed));
        // Mint ordinary NR authority at the real producer callback even when
        // optional FG accompanies it. Sender/depth/camera failure never grants
        // FG authority and cannot veto this independently authenticated NR call.
        GenshinPresentNr::NativeParentConsumer nativeParentConsumer(
            presented,cmd,queue,frame,epoch);
        DlssNr::NativeSceneCutResetScope resetReceipt(frame.sceneCutReset);
        NrInputObservation::Dispatch observed;
        observed.depth = frame.depthObservation;
        observed.queue = queue; observed.list = cmd; observed.color = frame.color;
        observed.guideDepth = frame.depth; observed.motion = frame.motion;
        observed.distrust = frame.historyDistrust; observed.epoch = epoch;
        if (presented)
        {
            observed.generation = presented->ticket.source.generation;
            observed.sourceFrame = presented->ticket.source.sourceFrame;
            observed.pictureSerial = presented->ticket.pictureSerial;
            const auto& mapped=presented->ticket.depth;const auto& source=mapped.provenance;
            observed.source.selected=source.selected;observed.source.original=source.original;observed.source.prepared=source.prepared;
            observed.source.lifetime=source.lifetime;observed.source.originalLifetime=source.originalLifetime;observed.source.loss=source.loss;
            observed.source.version=source.version;observed.source.clear=source.clear;observed.source.total=source.total;
            observed.source.covered=source.covered;observed.source.unresolved=source.unresolved;
            observed.source.mapping=mapped.mappingOwner?mapped.mappingOwner->serial:0;
            observed.source.mapped=mapped.mappingOwner?reinterpret_cast<uintptr_t>(mapped.mappingOwner->depth.texture.Get()):0;
            observed.source.imported=reinterpret_cast<uintptr_t>(importedDepth);
        }
        NrInputObservation::Scope observationScope(observed);
        const bool protectUi=DrawUiMask::Begin(uiMask,DrawUiMask::Consumer::Nr,cmd,frame.color,frame.pictureState);
        const bool ran = DlssNr::ApplyNativeInput(queue, cmd, frame.color, frame.depth, frame.motion,
                                        frame.historyDistrust, frame.depthReversed, frame.continuityReset || (presented && presented->reset), type, frame.pictureState,
                                        epoch, false, fgSender ? GenshinPresentNr::Scope::Current() : nullptr);
        if(protectUi)DrawUiMask::End(uiMask,DrawUiMask::Consumer::Nr,cmd,frame.color,frame.pictureState);
        // Capture on this exact producer list AFTER actual NR consumed its guides,
        // before NativeProducer closes/submits it. Missing observed camera refuses
        // only FG publication; it never installs a fabricated camera.
        if(ran && fgSender && fgSender->camera) {
            captureGameMotion(cmd,frame);
            static unsigned captureReports=0;if(captureReports<16){++captureReports;
                const auto dd=frame.depth?frame.depth->GetDesc():D3D12_RESOURCE_DESC{};
                const auto md=frame.motion?frame.motion->GetDesc():D3D12_RESOURCE_DESC{};
                const auto* scope=GenshinPresentNr::Scope::Current();
                LOG_INFO("R827 FG capture: pid={} generation={} captured={} offer={} scope_depth={} scope_consumer={} guide_epoch={} cut={} depth={} depth_format={} depth_size={}x{} motion_format={} motion_size={}x{} reversed={} ticket_reversed={} no_authority=true",
                    GetCurrentProcessId(),fgSender->ticket.source.generation,bool(fgRecording),fgSender->Known(),scope && scope->ticket.DepthMatches(),
                    scope && scope->MatchesConsumer(cmd,queue,frame),frame.guideEpoch,frame.guideCutSerial,frame.depth!=nullptr,
                    static_cast<unsigned>(dd.Format),dd.Width,dd.Height,static_cast<unsigned>(md.Format),md.Width,md.Height,frame.depthReversed,
                    fgSender->ticket.depth.convention==GenshinGuides::DepthConvention::DeviceZReversed);
            }
        }
        if (frame.sceneCutResetAccepted) *frame.sceneCutResetAccepted = resetReceipt.Accepted();
        return ran;
    };

    // Model creation returns before Dispatch's ordinary late hook setup. Its
    // recording must already be observed when NativeProducer submits it, or the
    // creation-completion gate can never become ready in a fresh SR-only game.
    if (GenshinPresentNr::Scope::Current()) ResTrack_Dx12::HookLateNrQueue(device);
    // Only a selected API12 color route can own this transient writer.
    // Menu/cleanup/another chain holding g_runMutex is NOT a color failure.
    HoYoShade::ColorWriteScope colorWriter((beforeNr || beforeSr) && input.api == Api::D3D12,
        device, queue, input.picture);
    if ((beforeNr || beforeSr) && input.api == Api::D3D12 && !colorWriter.Acquired()) {
        source.Return(input, FrameOutput {}); // untouched: natural is not armed
        return out;
    }
    FrameOutput output;
    const auto result = _producer->Run(queue, input, options, apply, output);
    // Only NativeProducer's successful actual queue signal retires these reads.
    // Failed Close/submit/Signal leaves the bounded lease permanently escrowed.
    if(input.gameMotion)input.gameMotion->BindCompletion(result.submitted?output.done.fence:nullptr,
        result.submitted?output.done.value:0);
    if (useVirtualUpscaler)
    {
        DlssNr::SceneCutInfo info;
        info.nativeRoute = true; info.virtualConsumer = true;
        info.failed = result.stoppedAt != nullptr;
        info.quarantined = _producer->Quarantined();
        info.reason = result.stoppedAt ? result.stoppedAt :
            "Virtual SR/FG retains legacy cut resets; direct-NR scene-cut policy/receipt does not apply";
        DlssNr::PublishNativeSceneCutStatus(info);
    }
    else if (supportedSceneRoute)
        PublishSceneCuts(result.submitted && (result.detectorRan || result.trustRan),
                         result.stoppedAt ? result.stoppedAt : result.detectorFailed ? "native detector dispatch" : nullptr);
    if(auto* r=GenshinPresentNrTrace::Current()) {
        r->submitted=result.submitted;r->composed=result.nativeRan;r->completionFence=reinterpret_cast<uintptr_t>(output.done.fence);
        r->completionValue=output.done.value;
        r->inputWait=result.submitted && input.ready.fence && input.ready.value;
        r->producerStop=result.stoppedAt?result.stoppedAt:"none";
    }
    if (!useVirtualUpscaler && options.apply && (!fgSender || !GenshinPresentNr::Original(fgSender->pictureKind)))
    {
        static thread_local NrInputObservation::Reporter producerReporter;
        const bool builtDepth = result.depthObservation.guide == NrInputObservation::GuideDepth::Supplied;
        if (producerReporter.Sample(result.depthObservation, builtDepth, result.nativeRan))
        {
            const auto* presented = GenshinPresentNr::Scope::Current();
            LOG_INFO("R849 NR producer input: pid={} frame={} generation={} picture={} source_depth={} "
                     "guide_depth={} acquired_count={} paired={} flow={} trust={} composed={} submitted={} stop={}",
                     GetCurrentProcessId(), input.frame, presented ? presented->ticket.source.generation : 0,
                     presented ? presented->ticket.pictureSerial : 0,
                     NrInputObservation::Text(result.depthObservation.source),
                     NrInputObservation::Text(result.depthObservation.guide), result.depthObservation.acquiredCount,
                     result.depthObservation.paired, result.flowValid, result.trustRan, result.nativeRan,
                     result.submitted, result.stoppedAt ? result.stoppedAt : "none");
        }
    }
    out.colorUncertain = output.uncertain;
    source.Return(input, output);
    colorWriter.FinishAfterReturn(output.uncertain);
    // Return synchronously authenticates the producer fence and orders the real
    // native-B copy. Failed Return/CopyOrdered cannot publish even if Run submitted.
    const bool fgReturned=fgRecording && fgRecording->Returned();
    if(fgReturned)out.fgPublished=fgRecording->Publish();
    if(fgSender){
        static unsigned returnReports=0;if(returnReports<16){++returnReports;
            const auto* scope=GenshinPresentNr::Scope::Current();
            LOG_INFO("R827 FG recording return: pid={} generation={} recording={} returned={} published={} model={} scope_consumer={} scope_model={} scope_copy={} completion={} no_authority=true",
                GetCurrentProcessId(),fgSender->ticket.source.generation,bool(fgRecording),fgReturned,out.fgPublished,result.nativeRan,
                scope && scope->consumerReached,scope && scope->consumerRan,scope && scope->screenCopyOrdered,scope && scope->completionFence && scope->completionValue);
        }
    }
    if(fgSender && out.fgPublished)GenshinBridgeFg::PresentScope::NativeReturned(*fgSender,queue);
    if(fgSender && out.fgPublished){
        static unsigned snapshotReports=0;if(snapshotReports<32){++snapshotReports;
            const auto* inner=GenshinPresentNr::Scope::Current();
            const auto* outer=GenshinBridgeFg::PresentScope::Current();
            LOG_INFO("R827 FG snapshot: pid={} generation={} stage={} staged_snapshot={} outer_stage={} outer_snapshot={} sender_provider={} interface_provider={} concrete_provider={} no_authority=true",
                GetCurrentProcessId(),fgSender->ticket.source.generation,inner?inner->SnapshotStage():"no-scope",
                inner && inner->ReturnedSnapshot(),outer?outer->NativeReturnStage():"no-scope",outer && outer->ReturnedSnapshot(),fgSender->provider,reinterpret_cast<uintptr_t>(State::Instance().currentFG),
                reinterpret_cast<uintptr_t>(dynamic_cast<DLSSG_Dx12*>(State::Instance().currentFG)));
        }
    }
    if (auto* presented=GenshinPresentNr::Scope::Current()) {
        static uint64_t attempts=0,entries=0,composed=0;
        const auto count=++attempts;entries+=presented->consumerReached?1:0;composed+=result.nativeRan?1:0;
        if(count<=3 || count%300==0 || (presented->consumerReached && entries<=3) || (result.nativeRan && composed<=3))
            LOG_INFO("Genshin presented NR execution: pid={} generation={} presentEpoch={} flowValid={} trustRan={} "
                "consumerReached={} modelComposed={} submitted={} screenCopyOrdered={} modelReset={} flowReset={} "
                "picture={} backbuffer={} completionFence={} completionValue={} sourceFrame={} pictureSerial={} adapterFrame={} "
                "ownerEpoch={} historyEpoch={} srPrivateOutput={:X} srSelectedRtv={:X} srSelectedSubresource={} srCopySerial={} "
                "reader_scope=current_backbuffer_to_native_picture sr_to_backbuffer_pixels=UNKNOWN final_visible=UNKNOWN",
                GetCurrentProcessId(),presented->ticket.source.generation,presented->ticket.presentEpoch,
                result.flowValid,result.trustRan,presented->consumerReached,result.nativeRan,result.submitted,
                presented->screenCopyOrdered,presented->reset,presented->flowReset,reinterpret_cast<uintptr_t>(input.picture),
                GenshinPresentNr::Identity(presented->ticket.backbuffer.Get()),
                reinterpret_cast<uintptr_t>(output.done.fence),output.done.value,
                presented->ticket.source.sourceFrame,presented->ticket.pictureSerial,input.frame,
                presented->ticket.source.ownerEpoch,presented->ticket.source.historyEpoch,
                presented->ticket.source.privateOutput,presented->ticket.source.selectedOutput,
                presented->ticket.source.selectedSubresource,presented->ticket.source.copyOrderedSerial);
    }
    if (OpticalFgNrEnabled() && !result.submitted)
        Config::Instance()->FGEnabled.set_volatile_value(false); // failed recording must never be consumed by FG

    if (result.stoppedAt != nullptr)
    {
        ++_stopped;

        if (_stopped == 1 || _stopped == 100 || _stopped % 1000 == 0)
            LOG_WARN("{}: the producer stopped at \"{}\" (HRESULT 0x{:X}, device removed reason 0x{:X}), "
                     "{} frames so far",
                     _tag, result.stoppedAt, (unsigned) result.stoppedHr, (unsigned) result.deviceRemoved, _stopped);
    }

    _diagFlowValid += result.flowValid ? 1 : 0;
    _diagDepth += result.flowValid && _producer->Flow() != nullptr && _producer->Flow()->UsedDepth() ? 1 : 0;
    _diagTrust += result.trustRan ? 1 : 0;
    _diagNative += result.nativeRan ? 1 : 0;
    _diagSubmitted += result.submitted ? 1 : 0;

    LogDiagnostics();

    _nativeRan = result.nativeRan;

    if (result.trustRan)
        _trustFrame = _frame;

    if (result.flowValid)
    {
        // How often the depth finder has a copy for the mask: the log shows it every 600 frames.
        ++_seen;
        _ran += result.trustRan ? 1 : 0;

        if (_seen == 600)
        {
            LOG_INFO("{}: the trust mask ran in {} of {} frames", _tag, _ran, _seen);
            _seen = _ran = 0;
        }
    }

    if (result.sceneCut)
    {
        ++_cuts;
        LOG_INFO("{}: scene cut seen ({:.0f}% distrusted), NR mode={} requests={} acceptedResets={}; final_visible=UNKNOWN", _tag,
                 result.distrustedShare * 100.0f, static_cast<unsigned>(options.sceneCutMode),
                 _producer->SceneCuts().Counts().resets, _producer->SceneCuts().resets);
    }

    if (result.submitted)
    {
        ++_frame;
        _status = Status::Running;
    }

    out.ran = true;
    out.submitted = result.submitted;
    return out;
}

bool NativeDriver::NrOnlyRunning() const
{
    return _status == Status::Running && _nativeRan && !Config::Instance()->DlssNrNativeUpscaler.value_or_default();
}

void NativeDriver::DrawStatus()
{
    if (OpticalFgNrEnabled())
    {
        MenuText::TextDisabled("独立光流 FG＋NR（实验）：%s", _nativeRan ? "正在输出" : "等待运动历史");
        MenuText::TextDisabled("游戏 DLSS 开关不改变此模式；NR 状态见呈现模式栏。");
        return;
    }
    switch (_status)
    {
    case Status::Off:
        MenuText::TextDisabled("等待首帧。");
        return;
    case Status::Waiting:
        MenuText::TextDisabled("游戏已调用超分，当前模式让出控制。");
        return;
    case Status::Failed:
        MenuText::TextDisabled("无法启动：%s", _failure.c_str());
        return;
    case Status::Running:
        break;
    }

    if (Config::Instance()->DlssNrNativeUpscaler.value_or_default())
    {
        if (_nativeRan && _virtualUpscaler != nullptr && _virtualUpscaler->Active())
            MenuText::TextDisabled("%s 正在稳定此画面。",
                                _virtualUpscaler->BackendName().c_str());
        else if (_virtualUpscaler != nullptr && !_virtualUpscaler->Error().empty())
            MenuText::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Stabiliser: %s", _virtualUpscaler->Error().c_str());
        else
            MenuText::TextDisabled("画面稳定：等待运动估计。");
    }
    else if (_nativeRan)
    {
        MenuText::TextDisabled("NR 正在处理此画面。");
    }
    else
    {
        const std::string reason = DlssNr::FinishedPictureStatus();
        MenuText::TextDisabled("%s", reason.empty() ? "NR: waiting for the motion estimate." : reason.c_str());
    }
}

void NativeDriver::DrawFlowTuning()
{
    // Always shown and open on Optical F5Low's page; the settings live in the running motion estimate, so there is
    // nothing to tune until a mode runs.
    if (!MenuText::TreeNodeEx("光流调优（立即生效，用于对比）##flowtuning", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (!_producer)
    {
        MenuText::TextDisabled("模式开始运行后显示。");
        ImGui::TreePop();
        return;
    }

    auto& tune = _producer->Flow()->Tuning();
    ImGui::SetNextItemWidth(160.0f);
    MenuText::SliderInt("平滑半径（0 为关闭）##flowsmooth", &tune.smoothRadius, 0, 4);
    ImGui::SetNextItemWidth(160.0f);
    MenuText::SliderInt("搜索半径##flowsearch", &tune.radius, 1, 3);
    ImGui::SetNextItemWidth(160.0f);
    MenuText::SliderInt("粗网格作为候选##flowcells", &tune.coarseCells, 1, 9);
    MenuText::Checkbox("上一帧光流作为候选##flowhistory", &tune.useHistory);
    MenuText::Checkbox("在同一表面匹配（使用深度）##flowdepth", &tune.depthMatching);
    MenuText::Checkbox("在相似外观区域匹配##flowlook", &tune.lookWeights);

    if (ImGui::IsItemHovered())
        MenuText::SetTooltip("%s", "只在亮度相近的邻域匹配运动，避免运动边缘影响旁边区域。");
    MenuText::Checkbox("平坦区域使用相机运动##flowglobal", &tune.globalCandidate);

    if (ImGui::IsItemHovered())
        MenuText::SetTooltip("%s", "为墙面、天空等平坦区域应用全画面运动；关闭时不应用。");
    ImGui::SetNextItemWidth(160.0f);
    MenuText::SliderInt("判定静止所需的连续不变帧数（0 为关闭）##flowstill", &tune.stillFrames, 0, 60);

    if (ImGui::IsItemHovered())
        MenuText::SetTooltip("%s", "连续这些帧保持不变后，HUD 等区域在镜头移动时保持静止。高值等待更久，0 为关闭。");
    ImGui::SetNextItemWidth(160.0f);
    MenuText::SliderFloat("静止判定允许的最大变化##flowstilleps", &tune.stillEpsilon, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetNextItemWidth(160.0f);
    MenuText::SliderFloat("静止判定的相机优势阈值##flowstillmargin", &tune.stillMargin, 0.0f, 0.2f, "%.3f");
    MenuText::Checkbox("低开销子像素修正##flowinverse", &tune.inverseRefinement);

    if (ImGui::IsItemHovered())
        MenuText::SetTooltip("%s", "更快的运动修正，但细线与颗粒的精度较低。");
    MenuText::Checkbox("感知亮度（HDR 与 SDR）##flowluma", &tune.perceptualLuma);

    if (ImGui::IsItemHovered())
        MenuText::SetTooltip("%s", "根据感知亮度匹配运动，使暗部细节与亮部同等计权，支持 HDR 和 SDR；关闭则使用旧方法。");
    bool preferStill = tune.zeroMargin > 0.0f;

    if (MenuText::Checkbox("平坦区域优先静止（实验功能）##flowzero", &preferStill))
        tune.zeroMargin = preferStill ? 0.001f : 0.0f;

    if (ImGui::IsItemHovered())
        MenuText::SetTooltip("%s", "无运动候选接近最优时保持平坦区域静止，可稳定 HUD，但普通墙面可能不跟随镜头。默认关闭。");
    MenuText::Checkbox("在切换发生帧检测硬切镜头##flowscene", &tune.sceneCutDetector);

    if (ImGui::IsItemHovered())
        MenuText::SetTooltip("%s", "在镜头切换发生帧检测，避免沿用旧运动。淡入淡出和曝光变化不计为切换。");
    ImGui::SetNextItemWidth(160.0f);
    MenuText::SliderFloat("可信度拐点##flowknee", &tune.confidenceKnee, 0.0005f, 0.05f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);

    if (MenuText::Button("接近旧版设置##flowold"))
    {
        tune.smoothRadius = 0;
        tune.radius = 2;
        tune.coarseCells = 1;
        tune.useHistory = false;
    }

    ImGui::SameLine();

    if (MenuText::Button("恢复默认##flowdefaults"))
        tune = OpticalFlowDx12::Settings {};

    ImGui::TreePop();
}

void NativeDriver::DrawTrustViewCombo()
{
    if (!_producer)
        return;

    static const char* kViews[] = {
        "Final mask", "Depth check",        "Revealed-surface check", "Flow consistency check",
        "Luma check", "Outside the picture"
    };
    int view = _producer->Trust()->Tuning().debugView;

    ImGui::SetNextItemWidth(220.0f);

    if (MenuText::Combo("显示##trustview", &view, kViews, IM_ARRAYSIZE(kViews)))
        _producer->Trust()->Tuning().debugView = view;
}

} // namespace native
