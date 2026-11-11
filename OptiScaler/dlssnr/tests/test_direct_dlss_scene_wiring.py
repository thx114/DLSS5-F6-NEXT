"""Integration guards for the real NGX -> final-picture -> FG boundary.

Static source checks only: no SDK execution or visual acceptance.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
late = (ROOT / "OptiScaler/shaders/dlssnr/DlssNr_Late.inl").read_text(encoding="utf-8")
interop = (ROOT / "OptiScaler/with_dx12/dx11_with_dx12_sc.cpp").read_text(encoding="utf-8")
presentation = (ROOT / "OptiScaler/shaders/dlssnr/DlssNr_GenshinPresentation.inl").read_text(encoding="utf-8")
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
require("!F6InputPolicy::FsrBridgeEnabled" in route, "internal FSR source cannot claim NGX identity")
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
require("if (g_nr.feature == nullptr && !g_nrRetired.empty())\n        Late::RetireCompletedPrivateRecordings();" in late,
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

# The fixed Genshin route is wired independently of the preserved generic path.
# CPU source tests and DX12 output tests exercise the corresponding real helpers.
evaluate = block(backend, "void EvaluateInternal(")
require("if (UsesGenshinPresentPipeline()) return;" in evaluate,
        "fixed presentation route cannot also run NR at Evaluate")
generic = block(late, "FinishedPictureOutcome ApplyToFinishedPicture(")
require("UsesGenshinPresentPipeline()" in generic and "return result;" in generic,
        "physical/generated Present cannot enter fixed source NR")
require("BeginGenshinPresentSource()" in feature and "CompleteGenshinPresentSource(presentSourceAttempt" in feature,
        "real Evaluate entry and successful copyback delimit the current source")
require("slot.genshinAttempt == attempt" in presentation and "slot.frame.RawSourceMetadata == currentMetadata" in presentation,
        "guide capture belongs to this attempt and immutable Evaluate metadata")
require("selectedSource && selectedSource != currentMetadata" in presentation,
        "selected FG source cannot be mixed with a different NR source")
require(presentation.index("sourceClock.Claim(attempt, source)") < presentation.index("g_compose->Dispatch("),
        "source is claimed before any model recording")
require(presentation.count("NrPresentation::RecordPublish(") == 1,
        "only one final publication site exists")
require("pq ? slot.presentWork.Get() : color.Get()" in presentation and
        "frame.InputArrivalState" in presentation and
        presentation.index("g_compose->Dispatch(") <
        presentation.index("NrPresentation::RecordPublish("),
        "current read-only input precedes NR and one final publication")
require(not any(name in presentation for name in ("pictureFlow->", "ApplyNativeInput(", "CaptureResidual(")),
        "fixed path has no optical or reused-colour writer")
require("colourSource ? colourSource->SourceMetadata()" in present,
        "actual selected FG metadata reaches the NR endpoint")
require("(frame.GenshinPresentPipeline && frame.Reset)" in backend,
        "source cuts invalidate presentation detail independently of the model-reset option")
require("if (!DispatchPass(cmdList, encodeParams" in backend and
        "if (!DispatchPass(cmdList, down, modelInput" in backend,
        "failed encode/resize cannot fall through to a model reading previous scratch")
require("composed = DispatchPass(cmdList, resolveParams" in backend and
        "if (composed && !targetSupportsUav)" in backend,
        "only a recorded current resolve can authorize copyback and successful publication")
require("else if (!targetSupportsUav)" in backend and
        "if (readOnlyInput) TransitionTarget((D3D12_RESOURCE_STATES)frame.InputArrivalState);" in backend,
        "failed separate-input and non-UAV paths also restore their resource states")
require("GameRouting::UseGenshinPresentPipeline(" in backend and
        "DlssNrGenshinPresentPipeline.value_or_default()" in backend,
        "Evaluate bypass obeys the tested placement policy rather than executable alone")
print(f"Direct DLSS scene wiring: {checks} checks passed (static only)")
