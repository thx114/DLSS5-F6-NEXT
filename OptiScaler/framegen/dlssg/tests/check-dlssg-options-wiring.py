"""Static adapter checks only; these do not execute the C++ contracts or SDK."""
from pathlib import Path
import re
import subprocess

directory = Path(__file__).resolve().parents[1]
root = directory.parents[2]
relative = "OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp"
source = (root / relative).read_text(encoding="utf-8-sig")
header = (directory / "DLSSG_Dx12.h").read_text(encoding="utf-8-sig")
policy = (directory / "DlssgOptionsPolicy.h").read_text(encoding="utf-8-sig")
residency = (root / "OptiScaler/runtime/StarRailResidencyPolicy.h").read_text(encoding="utf-8-sig")
menu = (root / "OptiScaler/menu/menu_common.cpp").read_text(encoding="utf-8-sig")
nvapi = (root / "OptiScaler/nvapi/NvApiHooks.cpp").read_text(encoding="utf-8-sig")
config = (root / "OptiScaler/Config.cpp").read_text(encoding="utf-8-sig")
baseline = subprocess.check_output(["git", "show", f"HEAD:{relative}"], cwd=root).decode("utf-8-sig").replace("\r\n", "\n")
checks = 0


def check(condition, message):
    global checks
    if not condition:
        raise AssertionError(message)
    checks += 1


def body(text, signature):
    start = text.index("{", text.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


dispatch = body(source, "bool DLSSG_Dx12::Dispatch()")
apply = body(source, "dlssg::options::Submission DLSSG_Dx12::SetDispatchOptions(")
scope = body(source, "dlssg::options::Scope DLSSG_Dx12::CurrentDispatchScope(")
query = body(source, "sl::Result DLSSG_Dx12::QueryProviderState(")
drs_get = body(nvapi, "NvApiHooks::hkNvAPI_DRS_GetSetting(")
check("SetDispatchOptions(_dlssgOptionsAck" in dispatch, "DLSSG does not consume common policy")
check("SetDispatchOptions(_reflexOptionsAck" in dispatch, "Reflex does not consume common policy")
check("dlssg::options::ProviderReflexOptions(" in dispatch and
      "reflexConst.useMarkersToOptimize = ReflexHooks::gameIsSendingMarkers()" not in dispatch,
      "observing markers must not opt the provider into unqualified Reflex marker tuning")
check("StarRail" not in apply and "Genshin" not in apply and "Zzz" not in apply, "cache eligibility is game-specific")
check("EveryDispatch" in scope and "IsStarRailF5DsOwner" in scope, "F5-DS per-dispatch policy lost")
check("_dispatchActive || _providerStateQueryActive || _dispatchOptions.InFlight()" in dispatch, "recursive SDK entry is allowed")
check("_generationState.Select(CurrentProviderOwner()" in dispatch, "selection is not bound to current raw capability")
check("UnlockedMax" not in source, "local raw cap is raised by an unlock hint")
check("_framesToInterpolate = appliedInterpolationCount" not in dispatch, "request published as accepted count")
check("_generationState.Record(optionsScope, willDispatchFrame" in dispatch, "accepted configuration lacks source identity")
check("_generationState.Accepted(optionsScope, willDispatchFrame)" in dispatch, "final source admission lacks accepted config")
check("nullptr" in query and "&dlssgOptions" not in source, "capability query can mutate SDK options")
check("getState(viewport" not in source, "unmanaged SDK state reader remains")
check("StreamlineProxy::DLSSGGetState()(viewport" not in source and
      query.count("StreamlineHooks::QueryDlssgState(") == 1, "state queries bypass shared serial owner checks")
check('static const char* srModelLabels[] = { "默认", "J", "K", "L", "M" };' in menu,
      "SR model choices J/K/L/M are missing")
for preset in ("J", "K", "L", "M"):
    check(f"NVSDK_NGX_DLSS_Hint_Render_Preset_{preset}" in menu,
          f"SR preset {preset} is not mapped to its NGX hint")
check('static const char* modelPresets[] = { "驱动默认", "A", "B", "C" };' in menu and
      "config->FGDLSSGModelPreset = static_cast<uint32_t>(modelPreset)" in menu,
      "DLSS-G model choices are not persisted from the menu")
check('readUInt("DLSSG", "ModelPreset")' in config and
      'target.SetValue("DLSSG", "ModelPreset"' in config,
      "DLSS-G model choice does not round-trip through the config")
check("NGX_DLSS_FG_OVERRIDE_ID" in drs_get and "NGX_DLSS_FG_OVERRIDE_ON" in drs_get and
      "NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID" in drs_get and
      all(f"NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_RENDER_PRESET_{preset}" in drs_get
          for preset in ("A", "B", "C")) and "presetValues[fgModelPreset - 1]" in drs_get,
      "FG model A/B/C is not mapped to the process-local NVAPI driver-setting query")
check("NvAPI_DRS_SetSetting" not in drs_get,
      "model selection must not write the user's NVIDIA driver profile")
check("CanRecomposeUserInterface(" in dispatch and
      "tagAccepted(FG_ResourceType::HudlessColor), tagAccepted(FG_ResourceType::UIColor), currentColourPair" in dispatch and
      "_guideUiTagFrame[fIndex] == willDispatchFrame" in dispatch and
      "_guideUiTagGeneration[fIndex] == sourceGeneration" in dispatch,
      "UI recomposition requires both successfully tagged inputs from this source generation")
check("snapshot.uiProtected=options.enableUserInterfaceRecomposition==sl::eTrue" in dispatch,
      "one HUD-less tag must not claim that independent UI recomposition is enabled")
check("framegen::DlssgStateQueryReason::CompletionFence" in source and
      "actualState.inputsProcessingCompletionFence" in source,
      "central observation removed the real SDK completion-fence read")
check("sourceGeneration != _inputContinuity.SourceGeneration()" in dispatch, "query callback can replace the source")
check("sourceFrameCount != _frameCount" in dispatch, "query callback can replace the source clock")
check("_generationState.acceptedGenerated" in dispatch and "requestedInterpolationCount, options.numFramesToGenerate" in dispatch,
      "request and accepted option telemetry are conflated")
check("_starRailDlssgOptions" not in source + header and "StarRailOptionsCache" not in source + header,
      "old private cache remains")
check("std::isfinite" in policy and "onErrorCallback" in policy and "Schema" in policy,
      "unknown option payload is eligible for caching")
release = body(source, "dlssg::options::Submission DLSSG_Dx12::SetReleaseOptions(")
disable = body(source, "bool DLSSG_Dx12::DisableRetainedProvider(")
reflex_off = body(source, "void DLSSG_Dx12::RetryReflexOff(")
deactivate = body(source, "void DLSSG_Dx12::Deactivate(")
complete = body(source, "bool DLSSG_Dx12::CompletePendingTeardown(")
check("_dispatchOptions.Release(options, current" in release, "release bypasses dispatch transaction guard")
for invariant in ("owner == CurrentProviderOwner()", "inputEpoch == _inputOwnerEpoch",
                  "!selected || selected == this",
                  "selected == State::Instance().currentFG",
                  "reflexPlugin == State::Instance().optiSlReflex",
                  "reflexApi == StreamlineProxy::ReflexSetOptions()"):
    check(invariant in release, f"release acknowledgement lacks owner gate: {invariant}")
check("return CanSubmitInputs()" not in release and "_isActive &&" not in release,
      "logical FG Off blocks SDK mode release")
check("SetReleaseOptions(options" in disable and "outcome.Result()" in disable,
      "teardown accepts a recursive or superseded DLSSG Off")
check("SetReleaseOptions(options" in reflex_off and "outcome.Result()" in reflex_off,
      "Reflex release accepts a recursive or superseded SDK Off")
check("DisableRetainedProvider();" in deactivate and "RetryReflexOff();" in deactivate,
      "retained teardown bypasses guarded release paths")
check("_dispatchOptions.Deactivate(wasActive || _haveRunningOptions || releaseOwnerChanged" in deactivate and
      deactivate.index("_dispatchOptions.Deactivate(") < deactivate.index("++_inputOwnerEpoch"),
      "duplicate Deactivate advances the input generation")
check("_deactivationOwner != CurrentProviderOwner()" in deactivate and
      "_deactivationInputEpoch != _inputOwnerEpoch" in deactivate and
      "_teardownOffAccepted = false;" in deactivate,
      "real provider/input identity change inherits a stale release acknowledgement")
check("InvalidateDispatchOptions();" not in disable + reflex_off,
      "recursive release invalidates the successful outer transaction")
release_policy = body(policy, "Submission Release(")
check("if (inFlight) return {};" in release_policy and
      release_policy.index("if (inFlight) return {};") < release_policy.index("return Apply("),
      "no-call duplicate release poisons the outer serial")
unretained = body(source, "void DLSSG_Dx12::RetryUnretainedProviderOff(")
check("SetReleaseOptions(options" in unretained and "outcome.confirmed" in unretained,
      "non-retained SDK Off bypasses shared gate or loses pending state")
check("_unretainedOffPending = true;" in deactivate and
      "RetryUnretainedProviderOff();" in complete and
      "StreamlineProxy::ReflexSetOptions()(" not in deactivate and
      "StreamlineProxy::DLSSGSetOptions()(" not in deactivate,
      "non-Genshin Deactivate bypasses shared release/retry path")
check("_reflexOwnership = ReflexOwnership::Running;" in dispatch,
      "non-Genshin Reflex enable lacks a release owner")
check(complete.index("_dispatchOptions.InFlight()") < complete.index("RetryReflexOff();") and
      complete.index("_providerStateQueryActive") < complete.index("RetryReflexOff();"),
      "SDK callback can enter physical drain/free")

# Residency intentionally changes final gap admission. Check the actual adapter
# calls and their safety gates here; the standalone C++ residency suite executes
# the production action/transaction policy and real InputContinuity type.
forced = body(source, "bool DLSSG_Dx12::ForceStarRailFgResident(")
gap = body(source, "bool DLSSG_Dx12::PresentStarRailInputGap(")
suppress = body(source, "bool DLSSG_Dx12::SuppressStarRailGapForPresent(")
suspend = body(source, "bool DLSSG_Dx12::SuspendForInputGap(")
prepare = body(source, "DLSSG_Dx12::StarRailPresent DLSSG_Dx12::PrepareStarRailPresent(")
retain = body(source, "bool DLSSG_Dx12::CanRetainForInputGap(")
action = body(residency, "constexpr FgGapAction SelectFgGapAction(")
transaction = body(residency, "bool SuppressFgGap(")
check("StarRailForceFGResident.ObservedValue()" in forced and "IsStarRailF5DsOwner()" in forced,
      "forced FG residency lacks atomic configuration or exact producer admission")
check("value_or_default" not in forced and "StarRailForceNRResident" not in forced,
      "FG residency uses unsafe configuration reads or depends on NR")
check("!state.fgChanged" in retain and "!state.isShuttingDown" in retain,
      "residency swallows a real rebuild or shutdown")
check("IsStarRailF5DsOwner()" in gap and "CurrentInputOwner() == _runningInputOwner" in gap,
      "gap action is detached from its real running owner")
check("SelectFgGapAction(ForceStarRailFgResident(), _inputContinuity.Held()" in gap,
      "live final admission does not consume residency policy")
check("case FgGapAction::Suspend: return SuspendForInputGap()" in gap and
      "case FgGapAction::SuppressHeld: return SuppressStarRailGapForPresent()" in gap,
      "gap actions bypass the real retained-session path")
check("case FgGapAction::CountPresent:" in gap and "++_fgFramePresentId" in gap and
      "_starRailPresentCounted = true" in gap, "default Present counter was removed")
check(action.index("if (forced)") < action.index("if (counted)") and
      "presentsSinceDispatch > 3" in action, "forced immediate suppression or legacy threshold lost")
check("beforeSdkPresent && !PresentStarRailInputGap()" in prepare and
      "return StarRailPresent::NativeFallback" in prepare and "return StarRailPresent::Failed" in prepare,
      "final SDK boundary no longer distinguishes safe original fallback from unsafe failure")
check("_inputContinuity.Current(" in prepare and "_starRailDispatchFrame == _frameCount" in prepare and
      "_starRailDispatchGeneration == _inputContinuity.SourceGeneration()" in prepare,
      "final generated-frame admission can consume an older source")
check("_inputContinuity.Hold(_frameCount, CurrentInputOwner())" in suspend and
      "return SuppressStarRailGapForPresent()" in suspend,
      "suspension no longer preserves its exact source/owner")
check("StarRailResidency::SuppressFgGap(_starRailGapOffAccepted, current," in suppress,
      "SDK suppression bypasses the tested reentrant transaction")
for invariant in ("_inputContinuity.OwnerMatches(owner)", "owner == CurrentInputOwner()",
                  "owner == _runningInputOwner", "frame == _frameCount",
                  "generation == _inputContinuity.SourceGeneration()",
                  "lifecycle == _providerOptionsLifecycle", "!_teardownPending",
                  "!_ownerChanging", "!_starRailUiFailed"):
    check(invariant in suppress, f"Off acknowledgement lacks current snapshot gate: {invariant}")
check("options.mode = sl::DLSSGMode::eOff" in suppress and
      "options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff" in suppress,
      "gap releases SDK allocations or leaves generation running")
check("frame > UINT32_MAX" in suppress and "getToken(token, &id)" in suppress and
      "static_cast<uint32_t>(*token) != id || !current()" in suppress,
      "null-tag token can wrap, alias a different frame or outlive its source")
check("sl::ResourceTag tags[4] {}" in suppress and
      "setTags(*token, viewport, tags, 4, nullptr)" in suppress,
      "gap retags old resources instead of null tags")
check(transaction.index("if (!current())") < transaction.index("if (!acknowledged") and
      "!off() || !current()" in transaction and "clearTags() && current()" in transaction,
      "SDK callbacks or cached acknowledgement can supersede the owner/source")
check("if (acknowledged) return true" not in transaction and
      suppress.index("StarRailResidency::SuppressFgGap(") < suppress.index("if (_starRailGapOffAccepted) return true"),
      "session Off acknowledgement incorrectly caches frame-specific null tags")
check("_dispatchOptions.InFlight() || _starRailGapSuppressing" in dispatch and
      "if (_starRailGapSuppressing) return false" in suppress,
      "recursive dispatch can send On inside Off/null-tag suppression")
check(suppress.index("StarRailResidency::SuppressFgGap(") < suppress.index("_starRailGapOffAccepted = true"),
      "Off acknowledgement is published before transaction success")
for forbidden in ("FreeResources(", "ReleaseObjects(", "Deactivate(", "WaitForSingleObject(",
                  "ExecuteCommandLists(", "->Signal(", "Reset("):
    check(forbidden not in gap + suppress, f"input gap changes GPU lifecycle: {forbidden}")
check("_inputContinuity.Fresh(" in dispatch and "GuideTagsCurrent(fIndex,willDispatchFrame)" in dispatch and
      "_inputContinuity.Resume()" in dispatch and "constData.reset = sl::Boolean::eTrue" in dispatch,
      "recovery lost fresh guides, exact source admission or history reset")

# These native contracts remain outside residency/options changes. Compare
# actual bodies against HEAD to preserve the queue/fence and source gates.
unchanged = [
    "bool DLSSG_Dx12::WaitForStarRailF5DsUIAllocator(",
    "bool DLSSG_Dx12::WaitForTeardownTarget(",
    "int DLSSG_Dx12::SelectStarRailF5DsFrame(",
    "bool DLSSG_Dx12::GenshinSourceCurrent(",
    "bool DLSSG_Dx12::SetGenshinResource(",
    "bool DLSSG_Dx12::NativeGuideRetired(",
    "void DLSSG_Dx12::CollectGenshinInputs(",
]
for signature in unchanged:
    check(body(source, signature) == body(baseline, signature), f"unrelated native contract changed: {signature}")

# A function-pointer getter such as ReflexSleep() is not a blocking invocation.
# Preserve actual waits and fixed pool uses independently of pacing admission.
before = [line for line in baseline.splitlines() if re.search(r"WaitForSingleObject|(?<!\w)Sleep\(|SetEventOnCompletion|\bBUFFER_COUNT\b", line)]
after = [line for line in source.splitlines() if re.search(r"WaitForSingleObject|(?<!\w)Sleep\(|SetEventOnCompletion|\bBUFFER_COUNT\b", line)]
check(before == after, "wait or pool contract changed")
deactivate = body(source, "void DLSSG_Dx12::Deactivate(")
check(deactivate.index("_reflexOwnership = ReflexOwnership::ReleasePending") <
      deactivate.index("DisableRetainedProvider()") and
      deactivate.index("_reflexOwnership = ReflexOwnership::ReleasePending") <
      deactivate.index("RetryUnretainedProviderOff()"),
      "Reflex release intent must survive an exception in either FG Off call")
print(f"DLSSG options static adapter checks: {checks} passed; C++/SDK/GPU not executed")
