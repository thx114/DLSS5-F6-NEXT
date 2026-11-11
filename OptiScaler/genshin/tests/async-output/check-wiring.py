"""Check output handoff and current-source wiring without building the product.

Run from any directory. --revision reads a fixed Git commit instead of the
working files, allowing the observed early-return regression to be reproduced.
This checks wiring, not SDK execution, GPU completion or visible pixels.
"""
from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--revision")
args = parser.parse_args()
checks = 0


def read(name):
    if args.revision:
        data = subprocess.check_output(
            ["git", "show", f"{args.revision}:{name}"], cwd=ROOT
        )
    else:
        data = (ROOT / name).read_bytes()
    return data.decode("utf-8-sig")


def check(condition, why):
    global checks
    checks += 1
    if not condition:
        raise SystemExit(f"FAIL: {why}")


def body(source, start, end):
    check(start in source, f"production seam exists: {start}")
    begin = source.index(start)
    check(end in source[begin:], f"production boundary exists: {end}")
    return source[begin:source.index(end, begin)]


wrapper = read("OptiScaler/with_dx12/dx11_with_dx12_sc.cpp")
wrapper = wrapper[wrapper.index("HRESULT Dx11wDx12SC::_PresentNativeVisible"):]
check(wrapper.count("_visibleChild->ServiceOutputReveal();") == 1,
      "one void output service precedes current input, without selecting a source-skip branch")
hold = wrapper.index("_visibleChild->ServiceOutputReveal();")
native_poll = wrapper.index("GenshinLegacy::OnPresent(_dx11Device)")
scope = wrapper.index("GenshinBridgeFg::PresentScope scope")
native_produce = wrapper.index("NativeMotionDx11::OnFGPresent(_real,_dx11Device)")
captured = wrapper.index("if (!scope.HasCapturedFrame())")
copy = wrapper.index("if (!_InitInteropObjects())")
sdk = wrapper.index("_fgSwapChain->Present(0, 0)")
check(hold < native_poll < scope < native_produce < captured < copy < sdk,
      "service prior output, produce/authenticate current source, then copy and enter SDK")
for forbidden in ("YieldForOutputReveal(", "outputDrain", "FinishOutputDrain(", "SdkOutputRevealDrain"):
    check(forbidden not in wrapper, f"output handoff cannot spend current source: {forbidden}")
check(wrapper[native_produce:captured].count("depthClosed = true") == 1,
      "current native producer closes depth exactly once")
gap = wrapper[captured:copy]
check('"NoAuthenticatedCameraOrGuides"' in gap and "return nativeIneligible();" in gap,
      "missing current guides retain their classified input-gap rejection before any copy")
check("snapshot=scope.ReturnedSnapshot()" in wrapper[copy:sdk] and
      "if(!snapshot)" in wrapper[copy:sdk] and "_nativeOriginal=snapshot" in wrapper[copy:sdk],
      "optional input consumes the authenticated current returned snapshot")
check("const auto* returned=scope.NativeReturn()" in wrapper[copy:sdk] and
      "const auto& ticket=returned->PictureTicket()" in wrapper[copy:sdk] and
      "visibleSource={s.device,s.context" in wrapper[copy:sdk],
      "SDK permit source still comes from this scope's exact returned picture ticket")
native_present = wrapper[wrapper.index("const auto nativePresent ="):wrapper.index("const auto nativeFallback =")]
check(native_present.count("nativeFrame.Present(") == 1,
      "natural forwarding retains one actual native Present call")
check("if (!depthClosed) GenericDepthDx11::OnPresent(_real);" in native_present,
      "natural parent does not close depth a second time")
check("capturePolled" in native_present,
      "natural forwarding retains the existing capture-polled witness")

bridge = read("OptiScaler/inputs/FG/GenshinBridge_Present_Dx12.h")

# Follow the actual producer path, rather than a simulated native-frame loop.
native = read("OptiScaler/native/NativeDriverDx11.cpp")
fg = native[native.index("ID3D12Resource* OnFGPresent("):]
check(fg.index("GenshinLegacy::TakePresentFg(real,device)") <
      fg.index("RunFrame(real,device,true,&sender)") < fg.rindex("wrapper->NativeProcessed()"),
      "native motion preparation still precedes the real producer and processed witness")
parent = native[native.index("void OnPresent("):native.index("ID3D12Resource* OnFGPresent(")]
check(parent.index("p->NativeWasProcessed(swapChain,device)") <
      parent.index("GenshinLegacy::TakePresentNr(swapChain,device)"),
      "natural forwarding cannot process the current picture a second time")
producer = read("OptiScaler/native/NativeDriver.cpp")
check(producer.index("source.Return(input, output)") <
      producer.index("fgRecording->Returned()") < producer.index("fgRecording->Publish()") <
      producer.index("GenshinBridgeFg::PresentScope::NativeReturned(*fgSender,queue)"),
      "only actual Return and publication sign the current native receipt")
offer = bridge[bridge.index("bool Seal()"):bridge.index("using SenderInput=")]
check("producer->PrepareGameMotion(ticket.context.Get(),ticket.depth,camera->predecessor,transfer_)" in offer,
      "current source reaches the original exact camera predecessor preparation")
motion = read("OptiScaler/genshin/GenshinFgGameMotion.h")
prepare = motion[motion.index("inline std::shared_ptr<Lease> Prepare("):]
check(prepare.index("Snapshot previous=pool.previous;pool.previous=current;") <
      prepare.index("return refuse(Reason::PredecessorGap)"),
      "current native packet advances independently of SDK input acceptance")
complete = bridge[bridge.index("bool Complete() const noexcept"):bridge.index("struct PresentedGuides")]
check("recorded && !Done(produced.Get(),producedValue)" in complete and
      "snapshotLease->CopiesComplete()" in complete,
      "revoked recording still waits for its actual producer fence and snapshot readers")

child = read("OptiScaler/with_dx12/GenshinVisibleChild.h")
service = body(child, "void ServiceOutputReveal()", "void Hide()")
check("FramePipeline::OutputRevealHandoff revealWindow" in child,
      "display gate uses the production handoff state machine")
for witness in ("if(ownerOperationDepth_)return", "std::try_to_lock", "!display.owns_lock()",
                "!display_->sdkOwnerActivity.MayWaitForOwner(false)", "Current(snapshot_)",
                "StreamCurrent(pendingContent_)", "content=pendingContent_", "StreamCurrent(content)",
                "pendingContent_.sequence!=content.sequence", "pendingContent_.streamLive!=content.streamLive",
                "pendingContent_.outputLive!=content.outputLive", "lastRaw.queue.Get()!=content.queue.Get()",
                "lastRaw.fence.Get()!=content.fence.Get()", "lastRaw.uncertain", "lastRaw.value<content.value",
                "!display_->HiddenReady() || !content.Ready(completed)", "Step::TailPending",
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
current = body(child, "bool StreamCurrent(", "bool RequestReveal(")
for witness in ("content.StreamKnown()", "content.OutputLive()", "content.streamLive==display_->streamLive",
                "content.streamOwner==display_->binding.Owner()", "display_->binding.Matches(",
                "display_->scopeEpoch==content.epoch", "content.epoch==epoch_.load()",
                "!hideRequested_.load()", "display_->inputLease.Accepted()",
                "display_->inputLease.Current(GetTickCount64())"):
    check(witness in current, f"output owner/stream identity rejection remains: {witness}")
raw_tail = body(child, "struct RawTail", "std::array<RawTail,4> hiddenTails")
for witness in ("if(inFlight || uncertain)return false", "!queue || !fence || !device || value==UINT64_MAX",
                "fence->GetCompletedValue()", "completed!=UINT64_MAX && completed>=value",
                "SUCCEEDED(device->GetDeviceRemovedReason())"):
    check(witness in raw_tail, f"raw retirement still requires real healthy queue/fence completion: {witness}")
show = body(child, "bool ApplyShow(", "class PresentPermit final")
for witness in ("if(!OnOwnerThread())return false", "if(!StreamCurrent(content))", "content.Ready(completed)",
                "lastRaw.queue.Get()!=content.queue.Get()", "lastRaw.fence.Get()!=content.fence.Get()",
                "SetWindowPos(child_,HWND_TOP", "SWP_SHOWWINDOW", "Publish(true)"):
    check(witness in show, f"real owner reveal retains its existing proof: {witness}")
check("lastRaw.value<content.value || !display_->HiddenReady()" in child,
      "last actual raw target still guards owner reveal")
check("bool ready=streamTail.Complete()" in child,
      "CPU inFlight clearing still cannot substitute for GPU completion")
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
arm = child[child.index("bool ArmStream()"):child.index("PresentPermit(const PresentPermit&)")]
check(arm.index("source_!=owner_.parentContent_") < arm.index("gate.inputLease.Begin("),
      "only the exact fresh dispatch/parent may open or renew a stream lease")
accept = child[child.index("bool AcceptStream()"):child.index("class PhysicalOutput final")]
check("actualResult_!=S_OK" in accept and "gate.inputLease.Accept(" in accept,
      "acceptance still requires saved original SDK S_OK")
cleanup = child[child.index("~PresentPermit()"):child.index("~GenshinVisibleChild()")]
check("inputLease.End(accepted)" in cleanup,
      "open SDK authorization ends at the actual scope lifetime")
check("streamLive==streamScope_" in cleanup and "owner_.Epoch()==epoch_ && (!streamScope_ || sameScope)" in cleanup,
      "late old-scope cleanup cannot close or hide a replacement epoch")
endpoint = read("OptiScaler/framegen/FinalOutputEndpoint.cpp")
check(endpoint.count("physical.Bind(") == 1 and
      endpoint.index("physical.Bind(") < endpoint.index("const auto actualPresent="),
      "only endpoint admission calls Bind before any raw Present callback")
check(bind.count("std::unique_lock display(") == 1,
      "raw identity and hidden ACK checks remain under one display gate acquisition")
for owner_method in ("void OwnerCommand()", "void Hide()", "void ExpireVisibleOutput()", "bool ApplyShow("):
    start = child.index(owner_method)
    end = child.index("\n    }",start)
    check("AwaitOwner(" not in child[start:end] and "PresentGenshin(" not in child[start:end],
          f"window owner does not recursively enter a waiting SDK raw: {owner_method}")
expiry = child[child.index("void ExpireVisibleOutput()"):child.index("void Invalidate()")]
check("ShowWindow(child_,SW_HIDE)" in expiry and "Publish(true)" in expiry,
      "expired visible output still hides and publishes actual HWND state")
check("inputLease.Current(" in expiry and "RevokeStream()" not in expiry and "BumpEpoch()" not in expiry,
      "fresh input owner survives output-only expiry; true expiry still calls normal Hide")
check(expiry.index("PhysicalContent discard") < expiry.index("std::lock_guard display"),
      "output-only expiry releases discarded COM observations after display unlock")
publish = body(child, "void Publish(", "bool RefusePreparation()")
check("display_->hiddenAcknowledged=display_->live && !(GetWindowLongPtrW(child_,GWL_STYLE)&WS_VISIBLE)" in publish,
      "only actual child visibility publication acknowledges hidden bootstrap")
before = child[child.index("static void BeforePresent("):child.index("bool AcceptStream()")]
check("p->owner_.OnOwnerThread()" in before and "sdkOwnerActivity.Enter(p)" in before and
      "p->ReleaseSdkOwnerActivity()" in before,
      "actual SDK entry and return bracket the HWND-owner activity")
check("ReleaseSdkOwnerActivity();" in cleanup,
      "exception cleanup also ends the owner SDK activity")
revoke = child[child.index("void RevokeStream()"):child.index("struct RawTail")]
check("sdkOwnerActivity" not in revoke,
      "revoking a stream cannot erase a still-active SDK owner call")
check("!display_->sdkOwnerActivity.MayWaitForOwner(false)" in service and
      service.index("!display.owns_lock()") < service.index("!display_->sdkOwnerActivity.MayWaitForOwner(false)"),
      "observer skips active SDK owner activity and never reads the gate after failed try-lock")
# Static checks verify shared raw deadline wiring; actual SDK error
# propagation and display liveness still require runtime verification.
print(f"PASS {checks} production wiring checks (no compile/GPU/display claim)")
