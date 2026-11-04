"""Check the real native/drain call ordering without building the product.

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


wrapper = read("OptiScaler/with_dx12/dx11_with_dx12_sc.cpp")
wrapper = wrapper[wrapper.index("HRESULT Dx11wDx12SC::_PresentNativeVisible"):]
hold = wrapper.index("YieldForOutputReveal()")
native_poll = wrapper.index("GenshinLegacy::OnPresent(_dx11Device)")
scope = wrapper.index("GenshinBridgeFg::PresentScope scope")
native_produce = wrapper.index("NativeMotionDx11::OnFGPresent(_real,_dx11Device)")
check("return nativePresent();" not in wrapper[hold:native_produce],
      "reveal drain must not return before native camera/guide production")
drain = wrapper.index("if(outputDrain)")
finish = wrapper.index("scope.FinishOutputDrain()")
copy = wrapper.index("if (!_InitInteropObjects())")
sdk = wrapper.index("_fgSwapChain->Present(0, 0)")
check(hold < native_poll < scope < native_produce < drain < finish < copy < sdk,
      "close admission, produce native once, finish exact drain, then optional SDK copy")
check(wrapper[native_produce:drain].count("depthClosed = true") == 1,
      "native producer closes depth exactly once before drain")
check(wrapper.index("if (!scope.HasCapturedFrame())") < drain,
      "drain cannot bypass current camera/guide qualification")
drain_branch = wrapper[drain:copy]
check(drain_branch.count("return nativePresent();") == 1,
      "drain forwards the actual natural parent Present once")
check("nativeIneligible()" in drain_branch,
      "receipt mismatch retains the real native fallback")
native_present = wrapper[wrapper.index("const auto nativePresent ="):wrapper.index("const auto nativeFallback =")]
check("if (!depthClosed) GenericDepthDx11::OnPresent(_real);" in native_present,
      "natural parent does not close depth a second time")
check("capturePolled" in native_present,
      "natural forwarding retains the existing capture-polled witness")

bridge = read("OptiScaler/inputs/FG/GenshinBridge_Present_Dx12.h")
finish_method = bridge[bridge.index("bool FinishOutputDrain()"):bridge.index("bool MatchesNative(")]
for witness in ("!nativeProcessed_", "nativeObserved_", "prepared_", "attempted_",
                "sdkCall_.Entered()", "sdkCall_.Returned()", "!returned.depthCaptured_",
                "MakeKey(f->ticket)!=key", "key=MakeKey(returned.ticket_)",
                "f->producer!=returned.producer_", "f->snapshotLease!=returned.snapshotLease_",
                "f->producerQueue.Get()!=returned.producerQueue_.Get()",
                "f->produced.Get()!=returned.produced_.Get()",
                "f->producedValue!=returned.producedValue_", "lifecycle.DiscardReturned("):
    check(witness in finish_method, f"drain retains exact scope witness: {witness}")
check(finish_method.index("lifecycle.DiscardReturned(") < finish_method.index("std::move(pool.ready)"),
      "authenticate before removing the single ready recording")
check("Retire(std::move(discarded))" in finish_method,
      "discarded CPU receipt retains ordinary real-fence retirement")
for forbidden in ("CloseProvider(", "SuspendGenshinPresent(", "Revoke(", "pool.presented.", "ReleaseLocked("):
    check(forbidden not in finish_method, f"drain cannot synthesize retirement/history or Off: {forbidden}")

# Follow the actual producer path, rather than a simulated native-frame loop.
native = read("OptiScaler/native/NativeDriverDx11.cpp")
fg = native[native.index("ID3D12Resource* OnFGPresent("):]
check(fg.index("GenshinLegacy::TakePresentFg(real,device)") <
      fg.index("RunFrame(real,device,true,&sender)") < fg.rindex("wrapper->NativeProcessed()"),
      "native motion preparation still precedes the real producer and processed witness")
parent = native[native.index("void OnPresent("):native.index("ID3D12Resource* OnFGPresent(")]
check(parent.index("p->NativeWasProcessed(swapChain,device)") <
      parent.index("GenshinLegacy::TakePresentNr(swapChain,device)"),
      "natural forwarding cannot process the drain picture a second time")
producer = read("OptiScaler/native/NativeDriver.cpp")
check(producer.index("source.Return(input, output)") <
      producer.index("fgRecording->Returned()") < producer.index("fgRecording->Publish()") <
      producer.index("GenshinBridgeFg::PresentScope::NativeReturned(*fgSender,queue)"),
      "only actual Return and publication sign the current native receipt")
offer = bridge[bridge.index("bool Seal()"):bridge.index("using SenderInput=")]
check("producer->PrepareGameMotion(ticket.context.Get(),ticket.depth,camera->predecessor,transfer_)" in offer,
      "drain reaches the original exact camera predecessor preparation")
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
check("lastRaw.value<content.value || !display_->HiddenReady()" in child,
      "last actual raw target still guards owner reveal")
check("bool ready=streamTail.Complete()" in child,
      "CPU inFlight clearing still cannot substitute for GPU completion")
check("revealWindow.BlocksRaw(GetTickCount64())" in child,
      "new worker raw remains bounded by the real admission window")
bind = child[child.index("bool Bind(IDXGISwapChain3*"):child.index("bool HiddenOnly()")]
check('return refuse("OwnerRevealDrainPending")' not in bind,
      "ordinary owner reveal cannot inject a deferred SDK Present error")
check("revealWindow.AwaitOwner(display,budget" in bind and
      "revealWindow.Remaining(GetTickCount64())" in bind,
      "queued worker waits outside the display lock only until the original reveal deadline")
check("gate->owner->OnOwnerThread())gate->revealWindow.Release()" in bind,
      "synchronous raw cannot wait on its own HWND owner stack")
check("revealWindow.AwaitOwner(display,HandoffBudget()" in bind and
      "gate->hiddenAcknowledged" in bind,
      "source-less raw waits for actual bounded hidden publication")
check("handoffUntil_=GetTickCount64()+LeaseMs" in bind and
      "(std::min)(HandoffBudget(),gate->revealWindow.Remaining(" in bind,
      "one raw shares its scheduling budget across lock, reveal, and hide")
for witness in ("PhysicalWindowLifetimeChangedAfterHandoff", "SdkOutputOwnerMismatchAfterHandoff",
                "PhysicalWindowLifetimeChangedAfterHide", "SdkOutputOwnerMismatchAfterHide",
                "gate->binding.BindPhysical(outputOwner,content_.Identity())",
                "gate->inputLease.Current(GetTickCount64())"):
    check(witness in bind, f"post-wait admission retains real authority: {witness}")
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
check(bind.count("std::unique_lock display(") == 1 and bind.count("revealWindow.AwaitOwner(") == 2,
      "both waits share the sole depth-one Bind lock")
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
check("display_->revealWindow.OwnerChanged()" in child,
      "real owner snapshot publication wakes asynchronous hide waiters")
print(f"PASS {checks} production wiring checks (no compile/GPU/display claim)")
