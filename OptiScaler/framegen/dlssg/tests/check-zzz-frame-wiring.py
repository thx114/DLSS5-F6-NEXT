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
check("reinterpret_cast<uintptr_t>(feature)" in start and "own->NativeZzzViewport()" in start and "reinterpret_cast<uintptr_t>(own)" in start, "exact selected owner/provider viewport not bound")
check("async_cross_thread=unsupported" in start and 'RejectUpscalerGuideSource("native-frame-scope' in start, "missing scope silently accepted")
pair = body(provider, "bool DLSSG_Dx12::SetUpscalerGuidePair(")
check(pair.index("Selection::Current") < pair.index("++use.value"), "GPU receipt reserved without selected join")
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
check("GuideTagsCurrent" in present and "use->presented" in present, "actual Present lacks one-shot consumption gate")
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
check("nativeZzzSourceFailed=!dispatched || tokenResult!=sl::Result::eOk" in fg, "failed dispatch falls through to SDK Present")
check("nativeZzzDlssg && willPresent && (!nativeZzzMarkerSource" in fg, "no-source SDK boundary silently succeeds")
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
