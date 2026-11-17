#include "pch.h"
#include <runtime/ComIdentity.h>
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
    return
        Config::Instance()->DlssNrNativeMotion.value_or_default() &&
        Config::Instance()->DlssNrNativeUpscaler.value_or_default();
}

NrInputObservation::SourceDepth ObserveAcquiredDepth(const FrameInput& input) noexcept
{
    return input.depthCount > 0 ? NrInputObservation::SourceDepth::Supplied : NrInputObservation::SourceDepth::SourceHasNoDepth;
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

    if ( !Config::Instance()->DlssNrNativeMotion.value_or_default() &&
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

NativeDriver::RunResult NativeDriver::RunFrame(IFrameSource& source, bool flowPreview)
{
    RunResult out;
    // Observe mode transitions even when Acquire is not ready; a waiting
    // native route must not retain an old Act request across menu changes.
    const bool useVirtualUpscaler = VirtualUpscalerRequested();
    const bool supportedSceneRoute = source.GetApi() != Api::Vulkan && State::Instance().api != Vulkan &&
        State::Instance().swapchainApi != Vulkan && State::Instance().swapchainInteropApi != SwapchainInteropApi::VkwDx12;
    if (_producer) _producer->SceneCutMode(!useVirtualUpscaler && supportedSceneRoute
        ? DlssNrSceneCut::ModeFrom(Config::Instance()->DlssNrSceneCut.value_or_default())
        : DlssNrSceneCut::Mode::Off);

    auto& timingRecorder = _cpuTiming;
    NativeProducerCpuTiming::Snapshot timingSnapshot;
    if (timingRecorder.TakePublished(timingSnapshot)) {
        const auto& acquire = timingSnapshot.stages[static_cast<unsigned>(NativeProducerCpuTiming::Stage::Acquire)];
        const auto& run = timingSnapshot.stages[static_cast<unsigned>(NativeProducerCpuTiming::Stage::Run)];
        const auto& returned = timingSnapshot.stages[static_cast<unsigned>(NativeProducerCpuTiming::Stage::Return)];
        const auto& allocator = timingSnapshot.stages[static_cast<unsigned>(NativeProducerCpuTiming::Stage::AllocatorWait)];
        LOG_INFO("{} CPU timing: window_ms={} attempts={} attempt_ids={}..{} "
                 "acquire_calls={} samples={} sample_frame={} estimate_known={} estimate_ms_s={:.3f} "
                 "run_calls={} samples={} sample_frame={} estimate_known={} estimate_ms_s={:.3f} "
                 "return_calls={} samples={} sample_frame={} estimate_known={} estimate_ms_s={:.3f} "
                 "allocator_wait_calls={} samples={} sample_frame={} estimate_known={} estimate_ms_s={:.3f} nested_in_run=true",
                 _tag, timingSnapshot.endMs - timingSnapshot.beginMs, timingSnapshot.frames,
                 timingSnapshot.firstFrame, timingSnapshot.lastFrame,
                 acquire.calls, acquire.samples, acquire.lastSampleFrame, acquire.estimateKnown, acquire.estimatedMsPerSecond,
                 run.calls, run.samples, run.lastSampleFrame, run.estimateKnown, run.estimatedMsPerSecond,
                 returned.calls, returned.samples, returned.lastSampleFrame, returned.estimateKnown, returned.estimatedMsPerSecond,
                 allocator.calls, allocator.samples, allocator.lastSampleFrame, allocator.estimateKnown, allocator.estimatedMsPerSecond);
    }
    auto timing = timingRecorder.BeginFrame();
    const auto returnInput = [&](const FrameInput& returnedInput, const FrameOutput& returnedOutput) {
        auto timingScope = timing.Measure(NativeProducerCpuTiming::Stage::Return);
        source.Return(returnedInput, returnedOutput);
    };

    FrameInput input;
    AcquireStatus acquired;
    {
        auto timingScope = timing.Measure(NativeProducerCpuTiming::Stage::Acquire);
        acquired = source.Acquire(input);
    }
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
        returnInput(input, FrameOutput {});
        return out;
    }
    ID3D12Device* device = work.device.Get();

    if (_producer == nullptr || _producer->Device() != device)
    {
        auto fresh = std::make_unique<NativeProducer>();

        const double initStart = Util::MillisecondsNow();

        if (!fresh->Init(work, !input.colorOnly))
        {
            _failure = fresh->Error();
            _status = Status::Failed;
            static thread_local unsigned failures=0;
            if (failures++<8) LOG_ERROR("{}: R041 native creation failed: {} receipts={}",_tag,_failure,fresh->CreationReport());
            PublishSceneCuts(false, _failure.c_str());
            returnInput(input, FrameOutput {});
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
    options.apply = useVirtualUpscaler || Config::Instance()->DlssNrNativeInput.value_or_default();
    const bool beforeNr = (!useVirtualUpscaler || virtualNativeNr) &&
        (input.api == Api::D3D12 || input.finalOriginal || virtualNativeNr) &&
        HoYoShade::Selected(HoYoShade::Stage::BeforeNr) &&
        (!Config::Instance()->DlssNrEnabled.value_or_default() || Config::Instance()->DlssNrFinishedPicture.value_or_default());
    const bool beforeSr = useVirtualUpscaler && !virtualNativeNr && input.api == Api::D3D12 &&
         input.filterSdrAllowed &&
        HoYoShade::ColorContract::Resolve(input.pictureFormat) &&
        HoYoShade::NativeBeforeSrAvailable(naturalSource.Get());
    const bool finalOriginal = !useVirtualUpscaler && input.api == Api::D3D11 && input.finalOriginal &&
        HoYoShade::Selected(HoYoShade::Stage::FinalOutput);
    options.colorOnly = input.colorOnly &&  !beforeSr;
    const auto filter = [&source, queue, &naturalSource](HoYoShade::Stage stage, const FrameInput& color, SyncPoint ready) {
        HoYoShade::ColorEndpoint endpoint;
        endpoint.stage = stage; endpoint.color = color.picture; endpoint.queue = queue;
        endpoint.ready = {queue, ready.fence, ready.value, ready.submission,
            ready.submission ? ready.submission->Generation() : 0, ready.submission ? ready.submission->Batch() : 0};
        endpoint.stream = reinterpret_cast<uint64_t>(&source); endpoint.generation = 1;
        endpoint.frame = color.frame; endpoint.width = color.width; endpoint.height = color.height;
        endpoint.state = color.pictureState; endpoint.stateKnown = true;
        endpoint.sdr = color.filterSdrAllowed;
        endpoint.naturalDevice = color.naturalDevice; endpoint.naturalBackbuffer = color.naturalBackbuffer;
        endpoint.naturalSource = naturalSource.Get();
        if (stage == HoYoShade::Stage::BeforeSr)
            endpoint.naturalContract = HoYoShade::NaturalSourceContract::BeforeSrNativeProducer;
        const char* finalDepthReason = "not-requested";
        if (stage == HoYoShade::Stage::BeforeNr)
            endpoint.depth = HoYoShade::Depth::FromNative(color, queue, ready,
                endpoint.stream, endpoint.generation);
        else if (stage == HoYoShade::Stage::FinalOutput)
        {
            endpoint.depth = HoYoShade::Depth::FromNative(color, queue, ready, endpoint.stream, endpoint.generation);
            finalDepthReason = endpoint.depth.resource ? "owned-native-depth" : "native-depth-unavailable";
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
    options.explicitConsumerReset = false;
    options.sceneCutPolicy = !useVirtualUpscaler; // Off also suppresses optional NR resets on unsupported origins
    options.sceneCutMode = supportedSceneRoute
        ? DlssNrSceneCut::ModeFrom(Config::Instance()->DlssNrSceneCut.value_or_default())
        : DlssNrSceneCut::Mode::Off;

    const auto apply = [this, &source, queue, useVirtualUpscaler, uiMask=input.uiMask, importedDepth=input.depthResource](ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
    {
        if (useVirtualUpscaler)
            return _virtualUpscaler->Run(cmd, frame);

        const DXGI_COLOR_SPACE_TYPE type = frame.space == ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                           : frame.space == ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        const uint64_t epoch = PresentEpoch(source.GetApi(), State::Instance().frameCount,
                         State::Instance().vulkanPresentCount.load(std::memory_order_relaxed));
        // Mint ordinary NR authority at the real producer callback even when
        // optional FG accompanies it. Sender/depth/camera failure never grants
        // FG authority and cannot veto this independently authenticated NR call.
        DlssNr::NativeSceneCutResetScope resetReceipt(frame.sceneCutReset);
        NrInputObservation::Dispatch observed;
        observed.depth = frame.depthObservation;
        observed.queue = queue; observed.list = cmd; observed.color = frame.color;
        observed.guideDepth = frame.depth; observed.motion = frame.motion;
        observed.distrust = frame.historyDistrust; observed.epoch = epoch;
        NrInputObservation::Scope observationScope(observed);
        const bool protectUi=DrawUiMask::Begin(uiMask,DrawUiMask::Consumer::Nr,cmd,frame.color,frame.pictureState);
        const bool ran = DlssNr::ApplyNativeInput(queue, cmd, frame.color, frame.depth, frame.motion,
                                        frame.historyDistrust, frame.depthReversed, frame.continuityReset, type, frame.pictureState,
                                        epoch, false);
        if(protectUi)DrawUiMask::End(uiMask,DrawUiMask::Consumer::Nr,cmd,frame.color,frame.pictureState);
        // Capture on this exact producer list AFTER actual NR consumed its guides,
        // before NativeProducer closes/submits it. Missing observed camera refuses
        // only FG publication; it never installs a fabricated camera.
        if (frame.sceneCutResetAccepted) *frame.sceneCutResetAccepted = resetReceipt.Accepted();
        return ran;
    };

    // Model creation returns before Dispatch's ordinary late hook setup. Its
    // recording must already be observed when NativeProducer submits it, or the
    // creation-completion gate can never become ready in a fresh SR-only game.
    HoYoShade::ColorWriteScope colorWriter((beforeNr || beforeSr) && input.api == Api::D3D12,
        device, queue, input.picture);
    if ((beforeNr || beforeSr) && input.api == Api::D3D12 && !colorWriter.Acquired()) {
        returnInput(input, FrameOutput {}); // untouched: natural is not armed
        return out;
    }
    FrameOutput output;
    NativeProducer::Result result;
    {
        auto timingScope = timing.Measure(NativeProducerCpuTiming::Stage::Run);
        result = _producer->Run(queue, input, options, apply, output, &timing);
    }
    if(result.completedPrepGpu.known) {
        const auto& g=result.completedPrepGpu;
        LOG_INFO("{} prep GPU completed: frame={} source_epoch={} source_generation={} source_frame={} picture_serial={} "
            "picture={:X} queue={:X} fence={:X} target={} completed={} frequency={} flow_ms={:.4f} "
            "trust_cut_ms={:.4f} guides_ms={:.4f} same_input=true scope=producer_prep_only inclusive_elapsed=true "
            "exclusive_gpu=UNKNOWN consumer_model_excluded=true no_added_wait=true",
            _tag,g.input.frame,g.input.sourceEpoch,g.input.sourceGeneration,g.input.sourceFrame,g.input.pictureSerial,
            g.input.picture,g.queue,g.fence,g.target,g.completed,g.frequency,
            g.milliseconds[0],g.milliseconds[1],g.milliseconds[2]);
    }
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
    if (!useVirtualUpscaler && options.apply && true)
    {
        static thread_local NrInputObservation::Reporter producerReporter;
        const bool builtDepth = result.depthObservation.guide == NrInputObservation::GuideDepth::Supplied;
        if (producerReporter.Sample(result.depthObservation, builtDepth, result.nativeRan))
        {
            LOG_INFO("R849 NR producer input: pid={} frame={} generation={} picture={} source_depth={} "
                     "guide_depth={} acquired_count={} paired={} flow={} trust={} composed={} submitted={} stop={}",
                     GetCurrentProcessId(), input.frame, 0,
                     0,
                     NrInputObservation::Text(result.depthObservation.source),
                     NrInputObservation::Text(result.depthObservation.guide), result.depthObservation.acquiredCount,
                     result.depthObservation.paired, result.flowValid, result.trustRan, result.nativeRan,
                     result.submitted, result.stoppedAt ? result.stoppedAt : "none");
        }
    }
    out.colorUncertain = output.uncertain;
    returnInput(input, output);
    colorWriter.FinishAfterReturn(output.uncertain);
    // Return synchronously authenticates the producer fence and orders the real
    // native-B copy. Failed Return/CopyOrdered cannot publish even if Run submitted.
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
