"""Lead-owned static regression: real FG observation seams, no SDK/game execution."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / "framegen/dlssg/DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")
dx11 = (ROOT / "inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp").read_text(encoding="utf-8-sig")
dx12 = (ROOT / "inputs/FG/Upscaler_Inputs_Dx12.cpp").read_text(encoding="utf-8-sig")
ngx = (ROOT / "inputs/NVNGX_DLSS_Dx12.cpp").read_text(encoding="utf-8-sig")
ngx11 = (ROOT / "inputs/NVNGX_DLSS_Dx11.cpp").read_text(encoding="utf-8-sig")
present_hook = (ROOT / "hooks/FG_Hooks.cpp").read_text(encoding="utf-8-sig")
bridge = (ROOT / "with_dx12/dx11_with_dx12_sc.cpp").read_text(encoding="utf-8-sig")
header = (ROOT / "framegen/dlssg/DLSSG_Dx12.h").read_text(encoding="utf-8-sig")


def body(signature):
    start = source.index("{", source.index(signature))
    depth = 1
    for end in range(start + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError(f"unterminated function: {signature}")


observe = body("void DLSSG_Dx12::ObserveUpscalerGuideSource(")
capture = body("void DLSSG_Dx12::CaptureAcceptedGuides(")
query = body("sl::Result DLSSG_Dx12::QueryProviderState(")
poll = body("void DLSSG_Dx12::PollGuideObservations(")
submit = body("void DLSSG_Dx12::ObserveGuideSubmission(")
dispatch = body("bool DLSSG_Dx12::Dispatch(")
pair = body("bool DLSSG_Dx12::SetUpscalerGuidePair(")
publish = body("void DLSSG_Dx12::PublishGuideObservation(")

# Distinct source and provider clocks: never promote SDK/Reflex to source.
assert "snapshot.source=sourceFrame" in observe
assert "snapshot.source=_frameCount" not in source
assert "nativeSource.sdkFrame : static_cast<uint32_t>(sourceFrame)" in source
assert "GetLastPreparedUpscalerFrameId(),colour" in dx11
assert "const auto source=feature->FrameCount()" in dx12
assert "snapshot.configuration=_guideObservationConfiguration" in observe
assert "owner==_guideObservationOwner" in observe
assert "featureId!=_guideObservationFeature" in observe

# Missing scalar metadata stays unknown and is individually named; fallback
# values used by the existing FG path must not become observed Evaluate facts.
for field in ("RenderWidth", "RenderHeight", "DepthX", "DepthY", "MotionX", "MotionY",
              "ScaleX", "ScaleY", "JitterX", "JitterY"):
    assert f"scalar(Field::{field}," in observe
assert "GetFeatureFlagsKnown()" in observe
assert "GuideDiagnostics::FieldNames[field]" in publish
assert "Origin::ApiDefault" not in observe

# Sampler can only mutate the exact independent provider recording. Sample the
# accepted transformed resources, real rects/states and the SDK's UV scales.
assert "depth.input.cmdList==_uiCommandList[slot]" in capture
assert "motion.input.cmdList==_uiCommandList[slot]" in capture
assert "if (!colourOnly || (offline && ownedGuides))" in capture
assert "if (!ownedGuides)" in capture
assert "if (offline && ownedGuides)" in capture
assert "inputs.independentRecording=true" in capture
for field in ("depthState", "motionState", "depthRect", "motionRect", "motionToUvX", "motionToUvY"):
    assert f"inputs.{field}=" in capture
assert "depth.input.copy?depth.input.copy:depth.input.resource" in capture
assert "motion.input.copy?motion.input.copy:motion.input.resource" in capture
assert "GuideCapture::Stream::Fg,_uiCommandList[slot],inputs,snapshot" in capture
assert "CaptureAcceptedGuides(" not in pair # wait until final colour is joined
assert "std::try_to_lock" in capture
assert "final->source==_upscalerColourSources[slot]" in capture
assert "UpscalerColourIdentity(colour)==final->endpoint.colour" in capture
assert "ready!=_resourceReady[slot].end() && ready->second" in capture
assert "inputs.colour=colour;inputs.colourState=actual.state" in capture

# Fresh FG-only preview cannot depend on NR or native ZZZ having installed the
# actual Execute/Reset hooks. Request once on the source/Present thread before
# recording, outside the per-slot resource lock; a request is not readiness.
assert "RequestGuideHooks();" in capture, "FG-only sampling must request the real late hooks"
request_hooks = body("void DLSSG_Dx12::RequestGuideHooks(")
request = capture.index("RequestGuideHooks();")
assert capture.index("GuideCapture::TryBeginSample(GuideCapture::Stream::Fg,sample)") < request
assert capture.index("GuideCapture::TryBeginSample(") < capture.index("const auto& depth=")
assert "GuideCapture::OfflineWanted(GuideCapture::Stream::Fg)" in capture
assert "GuideCapture::CancelSample(token)" in capture
assert capture.index("snapshot.epoch!=_inputContinuity.SourceGeneration()") < request
assert capture.index("!std::isfinite(inputs.motionToUvY)") < request
assert request < capture.index("std::shared_lock resources(") < capture.index("GuideCapture::TryRecord(")
assert "void RequestGuideHooks();" in header
assert "bool _guideHooksRequested=false;" in header
guard = request_hooks.index("if (_guideHooksRequested) return;")
install = request_hooks.index("ResTrack_Dx12::HookLateNrQueue(_device);")
latched = request_hooks.index("_guideHooksRequested=true;")
assert guard < install < latched
assert source.count("ResTrack_Dx12::HookLateNrQueue(") == 1
assert source.count("_guideHooksRequested") == 2  # no per-frame latch reset
assert "_nativeGuideHooksRequested" not in source + header
assert "RequestGuideHooks();" in pair  # retain the existing native entry too
assert pair.index("std::lock_guard continuityLock(_inputContinuityMutex)") < pair.index("RequestGuideHooks();")
for forbidden in ("GuideCapture::", "std::thread", "std::async", "detach(", "IsNr", "OwnsNativeZzzSource"):
    assert forbidden not in request_hooks
# Bind the request semantics to the actual existing void installer, including
# its serialized Detours current-thread update; do not invent a bool/HRESULT.
tracking_header = (ROOT / "resource_tracking/ResTrack_dx12.h").read_text(encoding="utf-8-sig")
tracking_source = (ROOT / "resource_tracking/ResTrack_dx12.cpp").read_text(encoding="utf-8-sig")
assert "static void HookLateNrQueue(ID3D12Device* device);" in tracking_header
installer = tracking_source[tracking_source.index("void ResTrack_Dx12::HookLateNrQueue("):
                            tracking_source.index("void ResTrack_Dx12::HookDevice(")]
assert "std::lock_guard<std::mutex> lock(hookMutex);" in installer
assert "HookToQueue(device);" in installer
assert installer.count("DetourUpdateThread(GetCurrentThread());") == 2
assert "o_LateReset = nullptr;" in installer and "o_ExecuteCommandLists = nullptr;" in installer

# The common hook owns exactly-once sampler notifications. Telemetry neither
# submits nor resets anything, and a completed producer is not SDK/scanout proof.
assert "GuideCapture::Submitted(" not in source
assert "GuideCapture::Reset(" not in source
for forbidden in ("->Signal(", "->Wait(", "ExecuteCommandLists(", "WaitForSingleObject(", "->Reset("):
    assert forbidden not in observe + capture + poll + submit + publish
assert "done==UINT64_MAX || done<observation.value" in poll
assert "observation.submitted" in poll and "observation.fence->GetCompletedValue()" in poll
assert "depth.frame!=observation.frame" in submit and "motion.frame!=observation.frame" in submit
assert "depth.generation!=observation.snapshot.epoch" in submit
assert "observation.frame!=_frameCount" not in submit
assert "snapshot.sdkStateKnown=false" in publish
assert "snapshot.generatedIdentityKnown=false" in publish
# Source freshness is independent of when a deferred completion is polled.
assert "GuideDiagnostics::Now()" not in poll
assert "snapshot.tick=observation.sourceTick" in poll
assert "snapshot.tick=observation.sourceTick" in publish

# Only a deliberate atomic change adds the SDK's supported debug option. It
# remains in the normal acknowledgement path and never persists to Config.
assert "GuideDiagnostics::OnlyGenerated().load(std::memory_order_acquire)" in dispatch
assert "options.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame" in dispatch
assert "SetDispatchOptions(_dlssgOptionsAck" in dispatch
assert not re.search(r"Config::Instance\(\)->[^;\n]*OnlyGenerated", source)
assert re.search(r"_guideUiTagGeneration\[fIndex\]\s*==\s*sourceGeneration", dispatch)
assert "snapshot.uiProtected=options.enableUserInterfaceRecomposition==sl::eTrue" in dispatch
assert "tagAccepted(FG_ResourceType::HudlessColor), tagAccepted(FG_ResourceType::UIColor), currentColourPair" in dispatch
assert not re.search(r"if\s*\([^\n]*(?:uiProtected|sdkEnabled)[^\n]*\)[^\n]*return false", dispatch)

# Actual non-StarRail submit reserves its target immediately before the real
# Execute, so out-of-order ring recording cannot certify unsubmitted commands.
present = body("bool DLSSG_Dx12::Present(")
ui_submit = body("bool DLSSG_Dx12::SubmitUICommandList(")
reservation = ui_submit.index("_uiAllocatorFenceValues[index]=++_uiFenceValue")
execute = ui_submit.index("_gameCommandQueue->ExecuteCommandLists(1,(ID3D12CommandList**)&_uiCommandList[index])")
signal = ui_submit.index("_gameCommandQueue->Signal(_uiFence,_uiAllocatorFenceValues[index])")
receipt = ui_submit.index("ObserveGuideSubmission(index,_uiFence")
assert present.index("CaptureAcceptedGuides(static_cast<UINT>(fIndex))") < present.index("SubmitUICommandList(")
assert "if (!SubmitUICommandList(static_cast<UINT>(fIndex))) return failSubmission();" in present
assert present.index("std::lock_guard continuityLock(_inputContinuityMutex)") < present.index("CaptureAcceptedGuides(static_cast<UINT>(fIndex))")
assert reservation < execute < signal < receipt
prehook = ui_submit.index("DlssNr::FinishedPictureSubmitting(_gameCommandQueue,1,")
assert reservation < prehook < execute
assert "if (FAILED(" in ui_submit[execute:signal]
assert "return false;" in ui_submit[signal:receipt]
assert execute < ui_submit.index("_uiCommandListResetted[index]=false") < signal

# Real successful Evaluate precedes every explicit HUDless capture. The same
# immutable source is held across Evaluate, and failure leaves no candidate.
evaluate = ngx.index("evalSuccess = feature->Evaluate(InCmdList, InParameters)")
complete = ngx.index("fg->CompleteNativeGuideSource(InCmdList,fgSource,actualOutput,evalSuccess)")
end = ngx.index("UpscalerInputsDx12::UpscaleEnd(InCmdList, InParameters, feature,evalSuccess,colourSource,actualOutput)")
assert ngx.index("const auto colourSource=") < evaluate < complete < end
assert "if (!succeeded) return" in dx12
assert "colourSource ? colourSource->Frame()" in dx12
assert "own->CaptureUpscalerHudless(colourSource,InCmdList,evaluatedOutput" in dx12
assert "uintptr_t evaluatedOutput=0" in dx11
assert "const dlssg::colour::Source key" in header
assert "const GuideDiagnostics::Snapshot snapshot" in header
assert "const dlssg::colour::Endpoint endpoint" in header
assert "const dlssg::colour::Mutation mutation" in header

join = body("void DLSSG_Dx12::JoinUpscalerFinalColour(")
assert join.index("FrameTrace::Kind::NrOutcome") < join.index(
    "if (outcome.result==DlssNr::FinishedPictureResult::FailedUncertain) return;")
assert "facts.nrReason=static_cast<int>(outcome.reason)" in join
assert "FrameTrace::Kind::FinalColourJoined" in join
assert "facts.colour=endpoint.colour" in join
assert "FrameTrace::Kind::ConstantsAccepted" in dispatch
assert "facts.token=static_cast<uint64_t>(*frameToken);facts.tokenKnown=true" in dispatch
assert "facts.constantsKnown=true" in dispatch
assert "pixelPhaseVerified=true" not in join + dispatch
assert "generatedIdentityKnown=true" not in source
assert "StreamlineHooks::QueryDlssgState(" in query
assert "sample.counterIntervalKnown" in query and "sample.previousQueryEndNs" in query
assert "framegen::DlssgStateQueryReason::CompletionFence" in source
assert "FrameTrace::Api::D3D12, handleId" in ngx and "InCmdList), nrUpscale)" in ngx
assert "evaluateObservation.Result(optiResult)" in ngx and "evaluateObservation.Result(result)" in ngx
assert ngx11.index("GuideDiagnostics::EvaluationScope evaluateObservation(") > ngx11.index(
    "if (handleId < DLSS_MOD_ID_OFFSET)", ngx11.index("NVSDK_NGX_D3D11_EvaluateFeature("))
assert "evaluateObservation.Result(NVSDK_NGX_Result_Success)" in ngx11
set_resource = body("bool DLSSG_Dx12::SetResource(")
current = body("bool DLSSG_Dx12::UpscalerColourCurrent(")
hudless_capture = body("void DLSSG_Dx12::CaptureUpscalerHudless(")
assert "source==_upscalerColourSources[slot]" in current
assert "_inputContinuity.Current(slot,source->key.frame,source->owner)" in current
assert "!_upscalerColourSucceeded[GetIndex()]" in hudless_capture
assert "source->key.output!=reinterpret_cast<uintptr_t>(output)" in hudless_capture
assert "output->GetDesc()" not in hudless_capture
assert "source->outputDescription" in hudless_capture
# Ambient captures never publish a colour tag. Only final NR outcome + exact
# endpoint may promote a staged current-source candidate to the SDK.
staging = set_resource.index("_upscalerPendingHudless[captureSlot]={source,*inputResource}")
assert staging < set_resource.index("StreamlineProxy::SetTagForFrame()")
assert "return true; // No SDK colour tag" in set_resource[staging:]
assert "FailedUncertain" in join and "SameEndpoint(endpoint,observed)" in join
assert "copyFence->GetCompletedValue()==UINT64_MAX" in join
assert "copiedColour,colour.Get()" in join
assert "!outcome.fence" in join and "!outcome.submissionSerial" in join
assert "dlssg::colour::CanUseHudless(source->key,candidate.source->key,mutation)" in join
assert "snapshot.fields=source->snapshot.fields" in join
assert "snapshot.picture=endpoint.present" in join
assert "candidate.input.cmdList=nullptr" in join
# A joined UntilPresent HUDless candidate must reach the real SDK call now;
# the legacy Dispatch retag is deliberately bypassed for direct inputs.
assert "if (joinedColour || (type == FG_ResourceType::Depth" in set_resource
assert "if (!joinedColour && _inputContinuity.Held())" in set_resource
failure = set_resource[set_resource.index("if (result != sl::Result::eOk)"):]
assert failure.index("if (!joinedColour)") < failure.index("Deactivate();")
assert "!_noHudless[fIndex] && !OwnsUpscalerDepthContract()" in dispatch
assert "Deactivate(" not in join  # Optional-colour refusal never disables FG.
# The bridge's nested generic Present cannot erase its outer route receipt.
assert join.index("outcome.bridgeEndpoint!=source->bridge") < join.index("new UpscalerFinalColour")
assert "_upscalerFinalColours[slot].reset()" not in join

nr = present_hook.index("const auto finishedPicture = DlssNr::ApplyToFinishedPicture(This, gameQueue)")
assert present_hook.index("const auto colourSource=colourOwner") < nr
assert nr < present_hook.index("colourOwner->JoinUpscalerFinalColour(colourSource,This,gameQueue,finishedPicture)")
assert "copiedValue==_lastInteropCopyFenceValue" in bridge
copy = bridge.index("bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(")
copy_body = bridge[copy:bridge.index("void Dx11wDx12SC::_ApplyNativeInputToFGBackBuffer",copy)]
assert copy_body.index("colorQueue->Signal(_copyFence, signalValue)") < copy_body.index("*copiedColour=fgBackBuffer.Get()")
print("FG guide diagnostic wiring verified; no compiler/SDK/game was invoked")
