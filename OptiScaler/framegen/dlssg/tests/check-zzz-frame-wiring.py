"""Static production call-site checks, complementary to the executable contract tests.

This checks real Windows adapters; it does not pretend to execute those TUs/GPU.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
fg = (root / "hooks/FG_Hooks.cpp").read_text(encoding="utf-8-sig")
sl = (root / "hooks/Streamline_Hooks.cpp").read_text(encoding="utf-8-sig")
reflex = (root / "hooks/Reflex_Hooks.cpp").read_text(encoding="utf-8-sig")
inputs = (root / "inputs/FG/Upscaler_Inputs_Dx12.cpp").read_text(encoding="utf-8-sig")
provider = (root / "framegen/dlssg/DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")
header = (root / "framegen/dlssg/DLSSG_Dx12.h").read_text(encoding="utf-8-sig")
contract = (root / "framegen/dlssg/ZzzFrameScope.h").read_text(encoding="utf-8-sig")
checks = 0


def check(value, message):
    global checks
    assert value, message
    checks += 1


def body(text, signature):
    start = text.index("{", text.index(signature))
    depth = 1
    pos = start + 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos]


evaluate = body(sl, "sl::Result StreamlineHooks::hkslEvaluateFeature(")
check("feature==sl::kFeatureDLSS" in evaluate, "actual DLSS feature gate missing")
check("api!=reinterpret_cast<uintptr_t>(State::Instance().optiSlInterposer)" in evaluate, "own API exclusion missing")
check("caller!=dllModule" in evaluate and "caller!=State::Instance().optiSlCommon" in evaluate, "local caller may authorize game input")
check("ParseGameViewport(inputs,numInputs)" in evaluate and "origin.viewportKnown=gameViewport.known" in evaluate, "real chained viewport missing")
parser = body(contract, "inline GameViewport ParseGameViewport(")
check("node=node->next" in parser and "sl::ViewportHandle::s_structType" in parser, "viewport chains not walked")
check("visited[j]==node" in parser and "visited.size()" in parser and "result.known" in parser, "cycle/bounds/ambiguous viewport guard missing")
check("NativeObject(" in evaluate and "nativeCommand.As(&commandIdentity)" in evaluate, "proxy/native canonical identity missing")
check(evaluate.index("Scope frameScope") < evaluate.index("if (!tokenValid)"), "invalid call does not shadow parent")
check(evaluate.index("auto result = o_slEvaluateFeature") < evaluate.index("frameScope.Finish(result==sl::Result::eOk)"), "actual upstream result not published")
check("GetNewFrameToken" not in evaluate, "upstream scope must copy numeric only, not mint SDK token")
check("mutex" not in evaluate and "lock_guard" not in evaluate, "scope must not hold lock over game/vendor")
install = body(sl, "void StreamlineHooks::hookInterposer(")
check("slInterposer==State::Instance().optiSlInterposer" in install and "Util::IsSubpath" in install, "independent SDK can be registered as game")
check("zzz && detourResult==NO_ERROR && o_slEvaluateFeature" in install, "API registration precedes successful hook commit")
check("gameApi.store(reinterpret_cast<uintptr_t>(slInterposer)" in install, "actual API instance not registered")
check("gameApi.store(0" in body(sl, "void StreamlineHooks::unhookInterposer("), "stale API after unhook")

start = body(inputs, "dlssg::NativeSource UpscalerInputsDx12::UpscaleStart(")
check(start.index("Selection selectedEntry") < start.index("BeginNativeUpscalerInputs"), "selected scope not at input entry")
check(start.index("Selection selectedEntry") < start.index("StartNewFrame()"), "frame advanced before identity gate")
check("NativeObject(InCmdList)" in start and "selectedList.As(&selectedIdentity)" in start, "selected list not canonical")
check("SelectedFeatureIdentity<IFeature>(feature)" in start and "selectedFeatureId,reinterpret_cast<uintptr_t>(own)" in start and "selectedFeatureId,own->NativeZzzViewport()" in start, "selected entry must use common IFeature base identity")
identity = body(contract, "uintptr_t SelectedFeatureIdentity(")
check("static_cast<CommonBase*>(feature)" in identity, "feature identity omits compiler base adjustment")
check("own->SetUpscalerGuidePair(static_cast<IFeature*>(feature),commandList" in start, "selected caller feature not passed to publication")
check("async_cross_thread=unsupported" in start and 'RejectUpscalerGuideSource("native-frame-scope' in start, "missing scope silently accepted")
pair = body(provider, "bool DLSSG_Dx12::SetUpscalerGuidePair(")
check(pair.index("Selection::Current") < pair.index("++use.value"), "GPU receipt reserved without selected join")
check("auto* feature=selectedFeature;" in pair and "State::Instance().currentFeature" not in pair, "guide publication re-queries mutable global feature")
check("SelectedFeatureIdentity<IFeature>(feature),static_cast<uint32_t>(viewport)" in pair, "publication uses a different feature base identity")
check("SetUpscalerGuidePair(IFeature* selectedFeature" in header and "SetUpscalerGuidePair(IFeature* selectedFeature" in provider, "provider does not require the actual caller feature")
interop = (root / "inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp").read_text(encoding="utf-8-sig")
check("own->SetUpscalerGuidePair(static_cast<IFeature*>(feature),cmdList" in interop, "interop caller not updated to explicit feature contract")
check("NativeObject(list)" in pair and "canonicalList.As(&listIdentity)" in pair, "guide pair did not normalize proxy/native")
check("_zzzFrameAdmission" not in provider + header and "class Admission" not in contract, "forbidden lifetime ledger remains")
check("frameScope->origin.frame,(uintptr_t)list" in pair, "SDK ID is not copied from synchronous real origin")
check("use.frameScope=frameScope" in pair, "upstream result not carried with full GPU owner")
check("_nativeGuideSdkFrame" not in provider + header, "independent SDK counter remains")
check("NativeGuideRetired(use)" in pair and "CreateFence" in pair, "existing GPU ownership guards disappeared")
current = body(provider, "bool DLSSG_Dx12::NativeGuideSourceCurrent(")
check("source.sdkFrameKnown" in current and "source.providerViewport==static_cast<uint32_t>(viewport)" in current, "source lacks known frame/local viewport check")
check("source.upstreamViewport==use->frameScope->origin.viewport" in current and "source.provider==reinterpret_cast<uintptr_t>(this)" in current, "game viewport or exact provider association missing")
check("frameScope->origin.viewport,static_cast<uint32_t>(viewport),true" in pair, "both viewport namespaces not recorded")
check("source.sdkFrame==use->frameScope->origin.frame" in current and "use->frameScope->Preparing()" in current, "source callback can relabel/revive failed scope")
check("Consumable(use.srSucceeded)" in body(provider, "bool DLSSG_Dx12::OrderNativeGuides("), "pending upstream/failed SR enters ordered consumer")
tags = body(provider, "bool DLSSG_Dx12::GuideTagsCurrent(")
check("Consumable(native.srSucceeded)" in tags, "tags consumable after upstream/selected failure")
present = body(provider, "bool DLSSG_Dx12::BeginNativeGuidePresent(")
check("GuideTagsCurrent" in present and "ConsumePresentOnce(use->presented)" in present, "actual Present lacks tested one-shot consumption gate")
once = body(contract, "inline bool ConsumePresentOnce(")
check("if (presented) return false;" in once and "presented=true;" in once, "production one-shot helper permits replay")
token = body(provider, "bool DLSSG_Dx12::NativeGuideToken(")
check("TokenFor(source.sdkFrame,token,getToken)" in token, "independent token does not verify source ID")
check("NativeGuideSourceCurrent(source)" in token, "vendor callback result not revalidated")
complete = body(provider, "void DLSSG_Dx12::CompleteNativeGuideSource(")
check("use->srSucceeded=succeeded" in complete and "RefuseDepthContract" in complete, "actual selected result does not revoke source")
check("use.output.Reset" not in complete and "use->output.Reset" not in complete, "SR failure destroys GPU owner")
for callback in ("NativeGuidesSubmitting", "NativeGuidesSubmitted", "NativeGuidesReset", "CompleteNativeGuideSource"):
    section = body(provider, f"void DLSSG_Dx12::{callback}(")
    check("sdkFrame=" not in section and "source={" not in section, f"{callback} rewrites source numeric identity")
refusal = body(provider, "bool DLSSG_Dx12::RefuseDepthContract(")
check("frameKnown &&" in refusal and "source.sdkFrameKnown" in refusal, "null-tag refusal invents unknown frame ID")
check("nativeZzzSourceFailed=!dispatched || tokenResult!=sl::Result::eOk" in fg, "failed dispatch can acquire a stale consumption receipt")
fg_present = body(fg, "HRESULT FGHooks::FGPresent(")
dispatch = body(fg_present, "else if (fgPresentAdmitted && !starRailSourceFallback)")
check(dispatch.index("nativeZzzSourceAttempted=nativeZzzDlssg!=nullptr") < dispatch.index("const bool dispatched=fg->Present()"), "attempt not latched in admitted Dispatch branch before vendor callbacks")
check(fg_present.count("nativeZzzSourceAttempted=") == 2 and "nativeZzzSourceAttempted=false" in fg_present, "attempt latch reset/recomputed or not initialized false")
check("if (dispatched && state.currentFG==nativeZzzDlssg)" in dispatch and "if (nativeZzzSourceFailed && state.currentFG==nativeZzzDlssg)" in dispatch, "changed provider dereferenced after Dispatch callback")
decision = body(contract, "PresentAction ResolvePresent(")
check(decision.index("if (!attempted) return unattemptedSafe ? PresentAction::PassThrough : PresentAction::UnsafeFailure;") < decision.index("beginSource()"), "unattempted Present bypasses pending Off failure or invokes source")
check("if (dispatched && beginSource()) return PresentAction::Consume;" in decision and "ensureOff() ? PresentAction::KnownOffSafeBasePresent : PresentAction::UnsafeFailure" in decision, "normal source gap is a hard failure or lacks real Off proof")
run_present = body(contract, "Result RunPresent(")
check("action==PresentAction::UnsafeFailure ? unsafeResult : basePresent()" in run_present, "safe base path does not preserve actual original Present result")
gate_start = fg_present.index("const auto nativeZzzAction=dlssg::zzz::ResolvePresent(")
gate = fg_present[gate_start:fg_present.index("const bool collectSdkPresent=", gate_start)]
check("nativeZzzSourceAttempted,!nativeZzzSourceFailed," in gate and "nativeZzzBasePresentSafe,[&]" in gate, "actual SDK boundary omits attempt, Dispatch result or pending Off state")
base_gate = fg_present[fg_present.index("const bool nativeZzzBasePresentSafe="):gate_start]
check("state.currentFG==nativeZzzDlssg && nativeZzzDlssg->NativeGuideBasePresentSafe()" in base_gate, "inactive base path does not inspect exact provider before SDK Present")
base_safe = body(provider, "bool DLSSG_Dx12::NativeGuideBasePresentSafe(")
check("BasePresentSafe(_teardownPending,_teardownOffAccepted," in base_safe, "inactive provider loses actual pending Off acknowledgement")
check(all(flag in base_safe for flag in ("_nativeGuideFailed", "_zzzSCFailed", "_teardownQuarantined", "use.executing", "use.submitted && (!use.signalled")), "inactive provider bypasses uncertain GPU ownership")
check("!ownerUnsafe && (!teardownPending || teardownOffAccepted)" in body(contract, "inline bool BasePresentSafe("), "inactive decision clears failed Off or quarantine without proof")
disable = body(provider, "bool DLSSG_Dx12::DisableRetainedProvider(")
check("_teardownOffAccepted = result == sl::Result::eOk;" in disable, "teardown Off acknowledgement is not its actual SDK result")
check("return nativeZzzMarkerSource && state.currentFG==nativeZzzDlssg && fgPacingActive() &&" in gate and "nativeZzzDlssg->BeginNativeGuidePresent(nativeZzzMarkerSource)" in gate, "attempted source not revalidated against exact current provider/state")
check("state.currentFG==nativeZzzDlssg && nativeZzzDlssg->PrepareNativeGuideGapPresent()" in gate, "gap fallback does not verify current actual provider Off")
check("nativeZzzSourceFailed=nativeZzzAction==dlssg::zzz::PresentAction::UnsafeFailure" in gate and "nativeZzzDlssg && willPresent && (!nativeZzzMarkerSource" not in fg_present, "blanket provider/Dispatch-failure gate remains")
check("nativeZzzSourceAttempted && nativeZzzAction!=dlssg::zzz::PresentAction::Consume" in gate and all(value in gate for value in ("nativeZzzMarkerSource={};", "nativeZzzStartCalled=false;", "localToken=nullptr;", "tokenResult=sl::Result::eErrorInvalidState;")), "safe base or failure retains source/log/End/Sleep authority")
passthrough = body(fg_present, "else if (willPresent && fg != nullptr)")
check("pass-through present only" in passthrough and "nativeZzzSourceAttempted" not in passthrough and "BeginNativeGuidePresent" not in passthrough, "inactive branch attempts source consumption")
check("const bool fgPresentAdmitted = willPresent && fg != nullptr &&" in fg_present and "fgFeatureActive || starRailSourceFallback" in fg_present, "test/non-admitted Present enters Dispatch")
check("config->FGEnabled.value_or_default() &&" in fg_present and "fg->IsActive() && !fg->IsPaused()" in fg_present, "FG Off/paused/warmup no longer skip active admission")
original_present_paths = """if (starRailSourceFailed)
        result = DXGI_ERROR_INVALID_CALL; // Unknown input cannot consume stale guides.
    else
        result = dlssg::zzz::RunPresent(nativeZzzAction,DXGI_ERROR_INVALID_CALL,[&]() -> HRESULT {
            return ObserveFgSdkPresent(collectSdkPresent,[&]() -> HRESULT {
                if (pPresentParameters == nullptr)
                    return o_FGSCPresent(This, SyncInterval, Flags);
                return o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, effectivePresentParameters);
            },&starRailSdkWall);
        });"""
check(original_present_paths in fg_present, "unattempted Present no longer follows original Present/Present1 paths")
gap = body(provider, "bool DLSSG_Dx12::PrepareNativeGuideGapPresent(")
safe = body(provider, "bool DLSSG_Dx12::NativeGuideGapSafe(")
check("_nativeGapOffProof.Ensure(owner,localViewport" in gap and
      "setOptions && StreamlineHooks::SubmitDlssgOptions(setOptions,viewport,options)==sl::Result::eOk" in gap,
      "Off success is fabricated rather than actual serialized SDK result")
submit_options = body(sl, "sl::Result StreamlineHooks::SubmitDlssgOptions(")
check("result=setOptions(viewport,options);" in submit_options and
      "g_dlssgStateQueries.SubmitOptions(" in submit_options and "return result;" in submit_options,
      "serialized Off path does not return the actual SDK result")
check("options.mode=sl::DLSSGMode::eOff;" in gap and "eRetainResourcesWhenOff" in gap, "gap does not actually send Off+retain")
check("CurrentInputOwner()==owner" in gap and "static_cast<uint32_t>(viewport)==localViewport" in gap, "Off publication not bound to current owner and provider viewport")
check("OwnsNativeZzzSource()" in safe and "!CanSubmitInputs()" in safe and "_nativeGuideFailed" in safe and "_zzzSCFailed" in safe, "Off hides failed/teardown/foreign native route")
check("use.executing" in safe and "use.submitted && (!use.signalled" in safe and "GetCompletedValue()==UINT64_MAX" in safe, "unsafe Execute/Signal/fence state can use base fallback")
can_submit = body(provider, "bool DLSSG_Dx12::CanSubmitInputs(")
check(all(flag in can_submit for flag in ("_hookRetiring", "_ownerChanging", "_teardownCompleting", "_teardownPending", "_teardownQuarantined")), "existing teardown/owner guards missing")
check("_nativeGuideDispatch={};" in gap and "accepted=false" in gap and "_depthSourceRejected=true" in gap, "gap retains stale consumer qualification")
check(all(value not in gap for value in (".Reset(", "SAFE_RELEASE", "FreeResources", "CompletePendingTeardown", "WaitFor")), "gap proof retires/replaces GPU owners")
check("_managedRetained=true;" in gap and "Deactivate();" in gap, "Off failure loses safe retained teardown boundary")
check("_haveRunningOptions" not in gap and "if (!_depthSourceGap)" not in gap, "old inactive/gap booleans used as actual Off proof")
check(refusal.index("if (OwnsNativeZzzSource())") < refusal.index("if (!_haveRunningOptions) return false;"), "first native gap skips real Off because no running options were cached")
check("PrepareNativeGuideGapPresent();" in refusal, "normal production refusal not connected to native Off proof")
proof = body(contract, "class GapOffProof")
check("accepted && !pending && owner==value && viewport==localViewport" in proof, "stale owner/viewport Off proof accepted")
check("const bool unchanged=pending;" in proof and "accepted=unchanged && succeeded && stillSafe();" in proof, "reentrant options can publish stale Off success")
check(proof.index("Invalidate();\n        pending=true;") < proof.index("const bool succeeded=setOff();"), "Off publication does not invalidate old proof first")
provider_dispatch = body(provider, "bool DLSSG_Dx12::Dispatch(")
options_start = provider_dispatch.index("const auto outcome = SetDispatchOptions(_dlssgOptionsAck,")
options_call = provider_dispatch[options_start:provider_dispatch.index("const bool optionsSubmitted = outcome.submitted;", options_start)]
check(options_call.index("_nativeGapOffProof.Invalidate();") < options_call.index("return setOptions ? StreamlineHooks::SubmitDlssgOptions(setOptions,viewport,options)"), "On/options call can reuse stale Off through failed constants/partial options")
check("_nativeGapOffProof.Invalidate();" in body(provider, "void DLSSG_Dx12::Deactivate(") and "_nativeGapOffProof.Invalidate();" in body(provider, "bool DLSSG_Dx12::DisableRetainedProvider("), "owner teardown/options changes preserve stale native gap proof")
submitted = body(provider, "void DLSSG_Dx12::NativeGuidesSubmitted(")
check("FAILED(queue->Signal(use.fence.Get(),use.value))" in submitted and "_nativeGuideFailed=true;" in submitted and "_managedRetained=true;" in submitted, "native Signal failure loses actual full owner retention")
retired = body(provider, "bool DLSSG_Dx12::NativeGuideRetired(")
check("use.fence->GetCompletedValue()" in retired and "done>=use.value" in retired and "if (!use.signalled" in retired, "native owner retirement no longer requires actual source fence")
check("BeginNativeGuidePresent(nativeZzzMarkerSource)" in fg and "DXGI_ERROR_INVALID_CALL" in fg, "actual SDK Present not gated")
check("NativeGuidePresentToken(localToken,nativeZzzMarkerSource)" in fg, "End/Sleep use stale/latest token")
check("static_cast<uint32_t>(*frameToken)!=frameCount" in reflex, "PCL forwarding accepts aliased independent token")
check("F6 ZZZ actual PCL forward" in reflex and "source_mutated=false" in reflex, "actual PCL receipt missing")
check(reflex.index("const auto actualPclFrame=") < reflex.index("const auto markerResult=StreamlineProxy::PCLSetMarker()"), "actual PCL ID captured after vendor reentry")
finish = body(contract, "void Finish(bool success)")
check("compare_exchange_strong" in finish and "mutex" not in finish, "SDK/upstream result callback takes continuity lock")
check("const Origin origin" in contract, "upstream numeric identity is mutable")
check("if (!previous &&" not in contract and "if (previous ||" not in contract, "blanket nested-scope refusal remains")
check("scope->claimed" in contract and "Scope::active->receipt!=active->receipt" in contract, "same-scope claim or invalid-shadow isolation missing")
check("localViewport==active->providerViewport" in contract and "providerOwner==active->provider" in contract, "selection local viewport/provider association missing")
print(f"PASS {checks} ZZZ actual production wiring checks (static only, no Windows/GPU execution)")
