"""Integration guards for the real NGX -> final-picture -> FG boundary.

Static source checks only: no SDK execution or visual acceptance.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
late = (ROOT / "OptiScaler/shaders/dlssnr/DlssNr_Late.inl").read_text(encoding="utf-8")
interop = (ROOT / "OptiScaler/with_dx12/dx11_with_dx12_sc.cpp").read_text(encoding="utf-8")
fg = (ROOT / "OptiScaler/framegen/dlssg/DLSSG_Dx12_Inputs.cpp").read_text(encoding="utf-8")
backend = (ROOT / "OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp").read_text(encoding="utf-8")
feature = (ROOT / "OptiScaler/upscalers/IFeature_Dx11wDx12.cpp").read_text(encoding="utf-8")


def block(text, marker):
    marker_pos = text.index(marker)
    start = marker_pos + re.search(r"\n[ \t]*\{", text[marker_pos:]).end() - 1
    depth = 1
    pos = start + 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos]


checks = 0


def require(value, why):
    global checks
    checks += 1
    assert value, why


route = block(late, "bool DirectDlssSceneBridgeEnabled()")
require("F6InputPolicy::DirectDlssInputs" in route, "route follows product NGX input policy")
require("GenshinSrReceipt" not in feature and "GenshinTransport" not in feature, "external NGX uses the ordinary resource view")
require("!native::OpticalFgNrEnabled()" in route, "explicit optical producer keeps its own contract")
require("DirectDlssSceneBridgeEnabled()" in block(late, "bool BridgeContinuationEnabled()"),
        "generic NGX bridge advances a source clock independently of physical FG")
capture = block(late, "void Capture(ID3D12GraphicsCommandList*")
require("dx11Bridge && (DirectDlssSceneBridgeEnabled() || IsStarRailProcess())" in capture,
        "current capture attempts are recorded for generic direct NGX")
require("slot.nativeCaptureAttempt = attempt" in capture, "attempt belongs to this capture")
require("frame.SubmissionEpoch = dx11Bridge ? CaptureEpoch(true)" in capture,
        "DX11 capture identity remains current even when the global DX12 Present clock is zero")
require("slot.motion.Get()" in capture and "NVSDK_NGX_Parameter_MotionVectors" in capture,
        "real NGX motion is cloned at its defined lifetime boundary")
require("(!bridgeScene || (nativeIdentity && slot.serial == latestNativeSerial))" in late,
        "direct source consumes only the newest exact joined capture")
scene = block(late, "if (bridgeScene)\n        {")
require("frame.NativeSceneRoute = true" in scene and "PopulateNativeSceneContract" in scene,
        "source carries scene identity to actual consumer validation")
flow_condition = re.search(r"if \(\(\(!bridgeScene.*?PictureHistoryReady", late, re.S)
require(flow_condition is not None, "ordinary NGX scene route cannot silently replace model MV with optical flow")
require("if (bridgeScene || (nativeFreshColorRoute" in late,
        "scene endpoint commits its own owner/shape so history does not reset every frame")
require("if (g_nr.feature == nullptr && !g_nrGpuWork.Empty())\n        Late::RetireCompletedPrivateRecordings();" in late,
        "every NR endpoint progresses completed idle private recordings during rebuild")
require(re.search(r"if\s*\(!freshColorFallback\s*&&\s*\(DetailReuse::PendingRecordings\(\)\s*!=\s*0\s*\|\|\s*"
                  r"DetailHistory::PendingRecordings\(\)\s*!=\s*0\)\)\s*Late::RetireCompletedPrivateRecordings\(&slot\)",late),
        "source rollover progresses both private histories without resetting the current recording")
require("if (Late::DirectDlssSceneBridgeEnabled() || Late::IsStarRailProcess())" in late,
        "a rejected new NGX offer revokes the prior capture attempt")
present = block(interop, "HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(")
require(present.index("_CopyDx11SharedToDx12FGBackBuffer") < present.index("DlssNr::ApplyToFinishedPictureDx11Bridge"),
        "NR operates on the uploaded current provider color")
require(present.index("DlssNr::ApplyToFinishedPictureDx11Bridge") < present.index("_fgSwapChain->Present"),
        "source NR completes its queue ordering before SDK FG Present")

# Genshin owns NR at successful source output. These are wiring guards;
# actual-DLL pixel tests independently exercise copyback and repeated Present.
evaluate = block(backend, "void EvaluateInternal(")
require("if (UsesSourceOutput() && !sourceOutput) return;" in evaluate,
        "ordinary callbacks cannot duplicate the selected source writer")
source = block(backend, "source_colour::Result EvaluateSourceOutput(")
require("GetResource(params, NVSDK_NGX_Parameter_Output, \"DLSSD.Output\") != recordedOutput" in source,
        "only the provider's actual current output can enter NR")
require("recording.resolved" in source and "return recording.Outcome();" in source and "successfulDispatches" not in source,
        "the recorded final resolve, rather than SDK success, identifies a write")
require("frame.SourceOutput = sourceOutput" in evaluate and "frame.FullModelRequired = sourceOutput" in evaluate,
        "scene output has explicit placement and requires the current complete model")
require("frame.FinishedPicture =" not in evaluate and "frame.IndependentCommands =" not in evaluate,
        "source recording cannot impersonate a finished picture or a private NR list")
require("source.ownedPreviewRecording == cmdList && cmdList &&" in evaluate and
        "cmdList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT" in evaluate,
        "preview authority requires this exact caller-owned DIRECT recording")
source_feature = block(feature, "bool IFeature_Dx11wDx12::EvaluateWithOutput(")
preview_lease = re.search(r"nrSource\.ownedPreviewRecording\s*=\s*sourceOutputNr\s*\?\s*cmdList\s*:\s*nullptr\s*;", source_feature)
source_call = re.search(r"sourceNrResult\s*=\s*DlssNr::EvaluateSourceOutput\([\s\S]*?\bnrSource\s*\);", source_feature)
require(preview_lease is not None and source_call is not None and preview_lease.end() < source_call.start(),
        "source-output bridge passes its current private recording rather than a retained unrelated list")
require("if (pass==0 && frame.CanRecordPreview())" in backend and "if(frame.CanRecordPreview())" in backend,
        "actual model input and raw input preview accept preview-only caller ownership")
require("frame.SourceCaptureSerial = submissionEpoch" in evaluate and "frame.SourceCaptureEpoch = source.virtualGeneration" in evaluate,
        "history uses real input attempts and the stable live feature generation")
require("EvaluateGuides::Read(frame, *params, source, depth, motion)" in evaluate,
        "the exact source boundary carries original guide metadata")
require("recordedOutput && recordedOutput == outputCopy.source" in feature,
        "SR output must be proved against the real copyback source")
require(source_feature.index("sourceNrResult = DlssNr::EvaluateSourceOutput") < source_feature.index("const auto closeResult = cmdList->Close()") <
        source_feature.index("Dx12CommandQueue->ExecuteCommandLists") < source_feature.index("if (!CopyBackOutput(outputCopy, canDeliver))"),
        "NR records before the same source Execute and original DX11 handoff")
require("(sourceOutputNr || orderedOutput) ? &recordedOutput : nullptr" in feature,
        "legacy and receipt ABI both require the provider's current written output")
require("currentSourceNr = {};" in feature and feature.index("if (!CopyBackOutput(outputCopy, canDeliver))") <
        feature.index("currentSourceNr = { sourceNrResult"),
        "failed attempts revoke old colour; only actual successful copyback publishes source mutation")
require("Dx12CommandAllocatorFenceValue[commandFrame]" in feature,
        "source receipt preserves the real source submission fence")
require("DlssNr_GenshinPresentation.inl" not in late and
        not (ROOT / "OptiScaler/shaders/dlssnr/DlssNr_GenshinPresentation.inl").exists(),
        "the old separate Present writer and state machine were removed")
require("if (UsesSourceOutput())" in late and "Scene NR was already" in late,
        "the current Present endpoint reports the copy without running NR")
require("!DlssNr::UsesSourceOutput()" in present,
        "the source route cannot fall into a second optical NR adapter")
require("CombinedMutation(source->sourceNr," in fg and "source-nr-receipt-mismatch" in fg,
        "FG freezes the real source mutation independently of diagnostic metadata")
require("(frame.SourceOutput && frame.Reset)" in backend,
        "source cuts invalidate residual history independently of model reset policy")
require("if (!DispatchPass(cmdList, encodeParams" in backend and "if (!DispatchPass(cmdList, down, modelInput" in backend,
        "failed encode/resize cannot read old scratch as a current model input")
require("composed = DispatchPass(cmdList, resolveParams" in backend and "if (composed && !targetSupportsUav)" in backend,
        "only a recorded current resolve authorizes final copyback")
require("GameRouting::UseGenshinSourceOutput(" in backend and "DlssNrGenshinPresentPipeline.value_or_default()" in backend,
        "the persisted compatibility setting maps to the tested source-output placement")
print(f"Direct DLSS scene wiring: {checks} checks passed (static only)")
