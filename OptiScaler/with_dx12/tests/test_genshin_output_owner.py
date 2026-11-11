"""Source-wiring regression for input-gap/output-owner separation and handoff.

Checks production wiring and, optionally, the frozen PID34924 failure sequence.
Does not compile, execute SDK/DXGI code, simulate GPU completion, or prove FPS.
--revision HEAD demonstrates the missing input-gap boundary in the old source.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--revision")
parser.add_argument("--log", type=Path)
args = parser.parse_args()
checks = 0


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise SystemExit(f"FAIL: {message}")


def read(path):
    if args.revision:
        return subprocess.check_output(
            ["git", "show", f"{args.revision}:{path}"], cwd=ROOT
        ).decode("utf-8-sig")
    return (ROOT / path).read_text(encoding="utf-8-sig")


def body(source, start, end):
    check(start in source, f"production seam exists: {start}")
    return source[source.index(start):source.index(end, source.index(start))]


child = read("OptiScaler/with_dx12/GenshinVisibleChild.h")
wrapper = read("OptiScaler/with_dx12/dx11_with_dx12_sc.cpp")
check(child.count("static uintptr_t Key(IUnknown* object)") == 1 and
      child.index("static uintptr_t Key(IUnknown* object)") < child.index("public:"),
      "owner and nested permit share the outer private COM identity implementation")
fallback = body(child, "void SuspendDisplayForNativeFallback(", "void ServiceOutputReveal()")
check("RequestHide();" in fallback and "KeepOutputOwnerForInputGap" not in child,
      "native fallback cancels old display permits instead of preserving a fresh old lease")

current = body(child, "bool StreamCurrent(", "bool RequestReveal(")
for witness in (
    "content.StreamKnown()", "content.OutputLive()", "content.streamLive==display_->streamLive",
    "content.streamOwner==display_->binding.Owner()", "display_->binding.Matches(",
    "display_->scopeEpoch==content.epoch", "content.epoch==epoch_.load()",
    "!hideRequested_.load()", "display_->inputLease.Accepted()",
    "display_->inputLease.Current(GetTickCount64())",
):
    check(witness in current, f"existing stream rejection remains: {witness}")

ineligible = body(wrapper, "const auto nativeIneligible =", "const UINT supportedFlags")
check("_visibleChild->SuspendDisplayForNativeFallback()" in ineligible and
      ineligible.index("SuspendDisplayForNativeFallback()") < ineligible.index("return nativePresent()"),
      "missing-input fallback revokes display authority before the newer native picture")
check("PresentScope::Skip()" in ineligible and "GenshinBridgeFg::Revoke(false)" in ineligible,
      "missing input is still skipped and its unused recording follows normal retirement")
check(ineligible.count("return nativePresent();") == 1,
      "ineligible branch forwards one real natural parent Present")
gap = body(wrapper, "if (!scope.HasCapturedFrame())", "if (!_InitInteropObjects())")
check('"NoAuthenticatedCameraOrGuides"' in gap and "return nativeIneligible()" in gap,
      "NoAuth rejects current input through the bounded output-owner boundary")
visible = wrapper[wrapper.index("HRESULT Dx11wDx12SC::_PresentNativeVisible"):]
check("outputDrain" not in visible and "FinishOutputDrain(" not in visible and
      "YieldForOutputReveal(" not in visible,
      "output handoff cannot spend a legitimate current source as a drain frame")
check(visible.count("_visibleChild->ServiceOutputReveal();") == 1 and
      visible.index("_visibleChild->ServiceOutputReveal();") <
      visible.index("GenshinLegacy::OnPresent(_dx11Device)") <
      visible.index("GenshinBridgeFg::PresentScope scope") <
      visible.index("NativeMotionDx11::OnFGPresent(_real,_dx11Device)") <
      visible.index("if (!scope.HasCapturedFrame())") <
      visible.index("if (!_InitInteropObjects())") <
      visible.index("_fgSwapChain->Present(0, 0)"),
      "same boundary services old output, then produces/copies/submits the actual new source")
service = body(child, "void ServiceOutputReveal()", "void Hide()")
check("FramePipeline::OutputRevealHandoff revealWindow" in child,
      "display gate uses the production handoff state machine")
for witness in ("if(ownerOperationDepth_)return", "std::try_to_lock", "!display.owns_lock()",
                "!display_->sdkOwnerActivity.MayWaitForOwner(false)", "Current(snapshot_)",
                "StreamCurrent(pendingContent_)", "content=pendingContent_", "StreamCurrent(content)",
                "pendingContent_.sequence!=content.sequence", "pendingContent_.streamLive!=content.streamLive",
                "pendingContent_.outputLive!=content.outputLive",
                "alreadyVisible=snapshot_.visible", "OutputTailReady(content,alreadyVisible,completed)", "Step::TailPending",
                "!snapshot.owns_lock()", "RequestReveal(content.epoch)", "Step::OwnerPending"):
    check(witness in service, f"handoff preserves output observation, owner and actual tails: {witness}")
for forbidden in ("inputLease.Begin(", "inputLease.Accept(", "FinishOutputDrain(", "PresentScope::Skip(",
                  "GenshinBridgeFg::Revoke(", "showUntil_.store(", "pendingContent_.revealed=true",
                  "tail.inFlight=false", "tail.value=", "Signal("):
    check(forbidden not in service, f"handoff cannot grant source/ACK/GPU authority: {forbidden}")
check(service.count("std::unique_lock display(") == 1 and
      "std::unique_lock display(display_->mutex,std::try_to_lock)" in service and
      service.count("std::unique_lock snapshot(snapshotMutex_,std::try_to_lock)") == 2 and
      service.count("!snapshot.owns_lock()") == 2,
      "observer attempts display/snapshot locks without waiting or unlocked access")
check(service.count("revealWindow.Service(") == 1 and
      "display_->inputLease.Until()," in service,
      "one observation respects the existing output and input freshness leases")
request = body(child, "bool RequestReveal(", "void ObserveContent(")
check("std::unique_lock lock(snapshotMutex_,std::try_to_lock)" in request and
      "if(!lock.owns_lock())return false" in request and
      request.index("if(!lock.owns_lock())return false") < request.index("Current(snapshot_)") and
      "snapshot_.epoch!=epoch" in request and "return QueueCommand()" in request,
      "request attempts snapshot access once and only posts an exact owner evaluation")
queue = body(child, "bool QueueCommand()", "void Publish(")
check("commandPosted_.exchange(true)" in queue and "PostMessageW(ownedChild_,CommandMessage," in queue,
      "ordinary posted command remains coalesced on the actual child owner")
policy = read("OptiScaler/runtime/OutputRevealHandoff.h")
for seam, code in (("observer", service), ("request", request), ("handoff policy", policy)):
    for token in ("Await", "wait(", "wait_for(", "wait_until(", "condition_variable",
                  "try_lock_for(", "try_lock_until(", ".lock()", ".unlock()",
                  "PeekMessage", "MsgWait", "DispatchMessage", "SendMessage", "Sleep(",
                  "while(", "while (", "for(", "for (", "ApplyShow(", "ShowWindow(", "SetWindowPos(" ):
        check(token not in code, f"{seam} has no wait/relock/retry/message pump/direct reveal: {token}")
check(policy.count("poll()") == 1 and
      "if(now()>=until)return Result::Expired;" in policy and
      policy.rindex("if(now()>=until)return Result::Expired;") < policy.index("const auto step=poll();"),
      "freshness is checked before the sole poll; no post-deadline retry exists")
check("if(now()>=until)return Result::Expired;\n        const auto step=poll();" in policy.replace("\r\n", "\n") and
      "const auto until=outputUntil<inputUntil ? outputUntil:inputUntil" in policy,
      "the immediate pre-poll check uses the earlier output/input freshness deadline")
check("if(servicing_)return Result::NotStarted" in policy and
      "~Finish(){self.Release();self.servicing_=false;}" in policy,
      "nested service cannot steal the observation; every admitted exit releases scheduling state")
owner = body(child, "void OwnerCommand()", "UINT width_")
check("if(e && e==epoch_.load())ApplyShow(e)" in owner and
      "if(msg==CommandMessage){self->OwnerCommand();return 0;}" in child and
      "if(msg==WM_TIMER && wp==LeaseTimer" in child and "self->OwnerCommand();" in child,
      "actual posted messages and natural lease timer retain owner-only reveal and epoch validation")
bind = child[child.index("bool Bind(IDXGISwapChain3*"):child.index("bool HiddenOnly()")]
check('return refuse("OwnerRevealDrainPending")' not in bind and
      'return refuse("OwnerRevealHandoffIncomplete")' not in bind and
      "revealWindow.BlocksRaw(" not in bind,
      "observer scheduling does not create a raw admission hold or reveal-specific SDK refusal")
check("std::array<RawTail,4> hiddenTails" in child,
      "existing hidden tail capacity is preserved")
for witness in ("PhysicalWindowLifetimeChanged", "SdkOutputOwnerMismatch",
                "SynchronousSdkOutputOwnerMismatch", "PhysicalWindowLifetimeChangedAfterHide",
                "SdkOutputOwnerMismatchAfterHide", "PhysicalRawAlreadyInFlight",
                "gate->binding.BindPhysical(outputOwner,content_.Identity())",
                "gate->inputLease.Current(GetTickCount64())", "gate->owner->RequestHide()",
                "!gate->hiddenAcknowledged || (GetWindowLongPtrW(d.OutputWindow,GWL_STYLE)&WS_VISIBLE)",
                'return refuse("UnauthenticatedOutputWaitingForOwnerHide")'):
    check(witness in bind, f"raw keeps actual lifetime/identity/inflight and hidden ACK gates: {witness}")
may_present = body(child, "bool MayPresent() const noexcept", "bool RawEntered()")
raw_entered = body(child, "bool RawEntered()", "void Submitted(")
check("std::chrono::steady_clock::time_point rawAdmissionUntil_{}" in child and
      bind.count("rawAdmissionUntil_=") == 1 and
      "rawAdmissionUntil_=std::chrono::steady_clock::now()+std::chrono::milliseconds(LeaseMs)" in bind,
      "SDK raw initializes one monotonic absolute deadline using its existing admission allowance")
for seam, code in (("Bind", bind), ("MayPresent", may_present)):
    check("std::defer_lock" in code and "display.try_lock_until(rawAdmissionUntil_)" in code and
          "std::try_to_lock" not in code and "try_lock_for(" not in code,
          f"SDK raw {seam} absorbs brief observer contention within the same absolute deadline")
check("rawAdmissionUntil_=" not in may_present and "!MayPresent()" in raw_entered,
      "MayPresent and RawEntered cannot renew the raw's lock budget")
check(may_present.index("!display.try_lock_until(rawAdmissionUntil_)") < may_present.index("!display_->live") and
      "StreamCurrent(content_,false)" in may_present and "display_->hiddenAcknowledged" in may_present,
      "raw recheck reads state only after locking and retains real owner/hidden ACK rejection")
for start, end in (("void Hide()", "void ExpireVisibleOutput()"),
                   ("void ExpireVisibleOutput()", "void Invalidate()"),
                   ("bool Prepare()", "bool StreamCurrent("),
                   ("bool ApplyShow(", "class PresentPermit final")):
    check("OwnerOperation operation" in body(child,start,end),
          f"owner operation cannot recursively wait for the same display gate: {start}")
check("nativeIneligible(true)" not in wrapper and "KeepOutputOwnerForInputGap" not in wrapper,
      "no input-gap override retains stale output visibility")
check(wrapper.index("if (!scope.HasCapturedFrame())") <
      wrapper.index("if (!_InitInteropObjects())", wrapper.index("HRESULT Dx11wDx12SC::_PresentNativeVisible")),
      "missing input cannot copy or submit an old source to SDK")
check(wrapper.index("return nativeIneligible()") <
      wrapper.index("GenshinVisibleChild::PresentPermit permit"),
      "input-gap returns before SDK input permit construction")
show = body(child, "bool ApplyShow(", "class PresentPermit final")
tail_ready = body(child, "bool OutputTailReady(", "void ServiceOutputReveal()")
check("OutputTailReady(content,alreadyVisible,completed)" in show and
      "if(!alreadyVisible && !display_->HiddenReady())return false" in tail_ready and
      "return content.Ready(completed)" in tail_ready,
      "new HWND reveal drains all actual tails; visible renewal completes its exact new output")
for witness in ("lastRaw.queue.Get()!=content.queue.Get()", "lastRaw.fence.Get()!=content.fence.Get()",
                "lastRaw.uncertain", "lastRaw.value<content.value"):
    check(witness in tail_ready, f"renewal retains stream identity and uncertainty rejection: {witness}")
hide = body(child, "void ArmHideLocked()", "mutable std::atomic<unsigned>")
check("BumpEpoch()" in hide and "display_->RevokeStream()" in hide,
      "real invalidation still revokes epoch and the stream")

# Follow the real downstream input retirement and native-forwarding seams.
# External SDK work is not executed or certified by these source checks.
bridge = read("OptiScaler/inputs/FG/GenshinBridge_Present_Dx12.h")
revoke = body(bridge, "inline void Revoke(", "class Recording final")
check("if(resetHistory)" in revoke and "Retire(std::move(f));Collect()" in revoke,
      "Revoke(false) retains the submitted predecessor and retires only the unused recording")
retire = body(bridge, "inline void ReleaseLocked(", "inline void Collect()")
for seam, code in (("Revoke(false)", revoke), ("input retirement", retire)):
    check(all(token not in code for token in ("Hide(", "RequestHide(", "RevokeStream(",
                                              "streamLive", "DisplayGate")),
          f"{seam} cannot indirectly revoke the independent display token")
check("parentContent_" not in current and "sdkSource_" not in current,
      "output-stream validity does not depend on a source receipt that input retirement closes")
provider = read("OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp")
suspend = body(provider, "void DLSSG_Dx12::SuspendGenshinPresent()", "bool DLSSG_Dx12::SetGenshinUiMask(")
off = body(provider, "bool DLSSG_Dx12::DisableRetainedProvider()", "void DLSSG_Dx12::RetryReflexOff()")
for seam, code in (("Skip's ordinary suspension", suspend), ("SDK Off+retain", off)):
    check(all(token not in code for token in ("Hide(", "RequestHide(", "RevokeStream(",
                                              "ReleaseSwapchain(", "ProviderRetired(", "BuffersRetired(")),
          f"{seam} has no unconditional output owner hide/retirement")
native = body(wrapper, "const auto nativePresent =", "const auto nativeFallback =")
check('if(_visibleReceipt.nativeResult!=S_OK)' in native and
      native.index('if(_visibleReceipt.nativeResult!=S_OK)') < native.index("_visibleChild->Hide()"),
      "native forwarding hides on a real non-S_OK result, not a successful input-gap Present")

if args.log:
    data = args.log.read_bytes()
    check(hashlib.sha256(data).hexdigest() ==
          "79222dbe19ac4cf276ab3e1efdc410be1a4f7ea5bf823f7b46cebd2f2d16505c",
          "failure evidence is the fixed PID34924 log, not another run")
    # The vendor's localized error bytes are not UTF-8. ASCII identities/order
    # remain exact; decode errors are never used to infer an error result.
    lines = data.decode("utf-8", errors="replace").splitlines()
    for row, tokens in (
        (11043, ("stream=2", "generation=1", "last_raw_inflight=true", "last_raw_tail=54")),
        (11056, ("event=hide-request", "epoch=10")),
        (11062, ("epoch=9", "reason=NoAuthenticatedCameraOrGuides")),
        (20296, ("event=hide-request", "epoch=14")),
        (20301, ("epoch=13", "reason=NoAuthenticatedCameraOrGuides")),
        (20302, ("reason=UnauthenticatedOutputWaitingForOwnerHide", "stream=2", "generation=1")),
        (20303, ("Async present failed (0x887A000A", "will be reported to the host")),
        (14389, ("Reporting deferred present error (0x887A000A", "host frame 86")),
    ):
        check(all(token in lines[row - 1] for token in tokens), f"fixed failure boundary at log line {row}")
    for token, count in (("F5 FG teardown requested", 2), ("F5 FG teardown complete", 2),
                         ("NoAuthenticatedCameraOrGuides", 43), ("F6 owner-output:", 166),
                         ("event=shown-after-physical-tail", 132)):
        check(sum(token in line for line in lines) == count, f"bounded evidence count: {token}")

print(f"PASS {checks} CPU source-wiring/evidence checks; SDK/GPU/FPS untested")
