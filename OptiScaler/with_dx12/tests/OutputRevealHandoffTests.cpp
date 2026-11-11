// SPDX-License-Identifier: GPL-3.0-or-later
// CPU fixtures execute the production single-poll handoff and input lifecycle.
// No SDK, GPU, HWND creation, message dispatch or completion proof is executed.
// Including the real owner header also type-checks the Windows adapter.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
template<class... Args> void IgnoreLog(const char*,Args&&...) noexcept {}
#define LOG_INFO(...) IgnoreLog(__VA_ARGS__)
#include "../GenshinVisibleChild.h"
#include "../../runtime/FenceCompletion.h"

// Seed only CPU control state in the real adapter. No HWND or COM object is
// created. The provider/endpoint still owns all simulated in-flight work.
struct GenshinVisibleChildTestAccess {
    template<class Check> static void NativeFallback(Check check,bool otherThread) {
        GenshinVisibleChild owner(nullptr);
        auto& gate=*owner.display_;
        const auto now=GetTickCount64();
        auto oldStream=std::make_shared<std::atomic<bool>>(true);
        gate.streamLive=oldStream;
        check(gate.inputLease.Begin(now,250) && gate.inputLease.Accept(now,250),"seed fresh accepted SDK lease");
        gate.inputLease.End(true);
        gate.sdkOwnerActivity.Enter(&owner);
        gate.streamTail.value=19;gate.streamTail.inFlight=true;
        gate.hiddenTails[0].value=17;gate.hiddenTails[0].uncertain=true;
        owner.pendingContent_.sequence=7;owner.pendingContent_.streamLive=oldStream;
        owner.pendingContent_.until=now+250;
        owner.showEpoch_=owner.Epoch();owner.showUntil_=now+250;
        const auto epoch=owner.Epoch();
        // A late worker captured this permit before the newer native fallback.
        auto lateOutput=owner.pendingContent_;
        if(otherThread)std::async(std::launch::async,[&]{owner.SuspendDisplayForNativeFallback();}).get();
        else owner.SuspendDisplayForNativeFallback();
        check(!oldStream->load() && !gate.streamLive,"native fallback revokes captured late-worker display permits immediately");
        check(!gate.inputLease.Accepted() && !gate.inputLease.Current(now),"fresh deadline does not authorize old pixels after native fallback");
        check(!owner.showEpoch_.load() && !owner.showUntil_.load() && !owner.pendingContent_.sequence,
              "native fallback cancels queued reveal and pending output observations");
        check(owner.Epoch()!=epoch,"native fallback invalidates queued owner command epoch");
        check(gate.streamTail.inFlight && gate.streamTail.value==19 &&
              gate.hiddenTails[0].uncertain && gate.hiddenTails[0].value==17,
              "display cancellation never completes or removes real output tails");
        check(!gate.sdkOwnerActivity.MayWaitForOwner(false),"display cancellation does not end an active SDK call");
        // Even a later fresh input cannot revive an already captured stream.
        gate.streamLive=std::make_shared<std::atomic<bool>>(true);
        check(!lateOutput.StreamKnown(),"new input cannot revive the captured old worker permit");
        check(!owner.StreamCurrent(lateOutput),"late output remains ineligible after new display generation");
        gate.sdkOwnerActivity.Leave(&owner);
        owner.ServiceOutputReveal();
        check(!owner.showEpoch_.load(),"late service cannot queue an old reveal after SDK returns");
    }
    template<class Check> static void LateRawCannotCancelNewStream(Check check,bool submitted) {
        GenshinVisibleChild owner(nullptr);
        auto& gate=*owner.display_;
        gate.owner=&owner;
        auto oldStream=std::make_shared<std::atomic<bool>>(true);
        {
            GenshinVisibleChild::PresentPermit::PhysicalOutput raw;
            raw.display_=owner.display_;raw.scoped_=true;raw.rawEntered_=true;
            raw.content_.epoch=owner.Epoch();raw.content_.streamLive=oldStream;
            gate.streamLive=oldStream;gate.scopeEpoch=owner.Epoch();
            owner.SuspendDisplayForNativeFallback();
            gate.streamLive=std::make_shared<std::atomic<bool>>(true);
            gate.scopeEpoch=owner.Epoch();
            const auto now=GetTickCount64();
            check(gate.inputLease.Begin(now,250) && gate.inputLease.Accept(now,250),"seed replacement stream after fallback");
            gate.inputLease.End(true);
            gate.streamTail.inFlight=true;
            owner.showEpoch_=owner.Epoch();owner.showUntil_=now+250;
            if(submitted)raw.Submitted(true,0,S_OK,false,false,nullptr,0);
        }
        check(gate.streamLive && gate.streamLive->load() && gate.inputLease.Accepted(),
              "old raw return or destructor cannot cancel the replacement stream");
        check(owner.showEpoch_==owner.Epoch(),"old cleanup preserves replacement display authorization");
        check(!gate.streamTail.inFlight && gate.streamTail.uncertain,
              "old raw cleanup still records uncertain actual output custody");
    }
};

namespace {
using Handoff=FramePipeline::OutputRevealHandoff;
using Step=Handoff::Step;
using Result=Handoff::Result;
using namespace std::chrono_literals;
unsigned checks=0;
void Check(bool ok,const char* why) {
    ++checks;
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::abort();}
}
GenshinBridgeFg::Key Source(uint64_t n) {
    GenshinBridgeFg::Key key;
    key.device=1;key.context=2;key.swapchain=3;key.backbuffer=4;
    key.deviceEpoch=key.ownerEpoch=key.configEpoch=key.historyEpoch=1;
    key.instance=1;key.generation=n;key.sourceFrame=n;key.picture=n;key.present=n;
    key.selected=5;key.thread=6;key.width=1920;key.height=1080;
    key.submitSerial=key.copyOrderedSerial=n;
    return key;
}
void Alternating335(unsigned pendingKind) {
    Handoff handoff;
    uint64_t clock=1000,predecessor=0,tail=0,completed=0;
    bool rawInFlight=false,ownerAck=false;
    unsigned handoffs=0,nativePresents=0,totalPolls=0;
    std::vector<uint64_t> submitted;
    // 671 native sources: the old outputDrain branch discarded 335 even ones.
    // Three runs cover raw in flight, pending GPU tail, and owner ACK pending.
    for(uint64_t source=1;source<=671;++source) {
        const auto key=Source(source);
        if(source%2==0) {
            ++handoffs;tail=source*10;completed=pendingKind==1 ? tail-1:tail;
            rawInFlight=pendingKind==0;ownerAck=false;
        }
        if(source>1) {
            unsigned polls=0;
            const auto result=handoff.Service(clock+50,clock+60,[&]{return clock;},[&] {
                ++polls;++totalPolls;
                if(rawInFlight || FramePipeline::ObserveFence(completed,tail)!=FramePipeline::FenceCompletion::Complete)
                    return Step::TailPending;
                return ownerAck ? Step::Revealed:Step::OwnerPending;
            });
            const auto expected=source%2 ? Result::Revealed:
                (pendingKind<2 ? Result::TailPending:Result::OwnerPending);
            Check(result==expected && polls==1,"each boundary polls once and returns pending without a wait");
        }
        Check(handoff.State()==Handoff::Phase::Idle,"no scheduling state leaks into current SDK input");
        using Phase=GenshinBridgeFg::Phase;
        GenshinBridgeFg::Lifecycle lifecycle;
        Check(lifecycle.Record(key),"record this source, not the pending output source");
        for(auto edge:{std::pair{Phase::Recorded,Phase::Returned},
                      std::pair{Phase::Returned,Phase::Copied},std::pair{Phase::Copied,Phase::Tagged},
                      std::pair{Phase::Tagged,Phase::Dispatched},std::pair{Phase::Dispatched,Phase::InputAccepted}})
            Check(lifecycle.Advance(key,edge.first,edge.second),"current source passes exact production identity edges");
        Check(lifecycle.Identity()==key && predecessor+1==key.sourceFrame,"SDK predecessor remains adjacent");
        Check(!lifecycle.Advance(Source(source+1),Phase::InputAccepted,Phase::Dispatched),"different source cannot reuse accepted guides");
        predecessor=source;submitted.push_back(key.sourceFrame);++nativePresents;
        if(source%2==0) {
            // Fixture natural callbacks occur AFTER current input consumption.
            // Returning from service itself never completes or ACKs this output.
            Check(!ownerAck,"pending service cannot manufacture owner ACK");
            if(rawInFlight) {
                rawInFlight=false;++tail;
                Check(completed<tail,"prior raw return advances its real tail, not completion");
            }
            completed=tail; // fixture actual-output queue observation, not an input fence
            Check(FramePipeline::ObserveFence(completed,tail)==FramePipeline::FenceCompletion::Complete,
                  "natural owner needs the latest actual output tail before ACK");
            ownerAck=true;
        }
        ++clock;
    }
    Check(handoffs==335 && totalPolls==670,"335 pending/revealed pairs, exactly one poll per input boundary");
    Check(nativePresents==671 && submitted.size()==671 && submitted.front()==1 && submitted.back()==671,
          "all current inputs and native Presents survive the alternating output schedule");
}
void DeadlineBeforePoll() {
    Handoff handoff;unsigned polls=0;
    auto pending=Step::TailPending;
    auto poll=[&]{++polls;return pending;};
    uint64_t now=15;
    Check(handoff.Service(15,50,[&]{return now;},poll)==Result::Expired && polls==0,
          "already expired observation cannot poll");
    unsigned reads=0;
    Check(handoff.Service(30,15,[&]{return ++reads==1 ? uint64_t{14}:uint64_t{15};},poll)==Result::Expired && polls==0,
          "deadline crossed after admission is checked before the sole poll");
    now=10;
    Check(handoff.Service(15,30,[&]{return now;},[&]{++polls;now=100;return pending;})==Result::TailPending && polls==1,
          "overrunning poll cannot trigger a second poll or wait");
    Check(handoff.Service(15,30,[&]{return now;},poll)==Result::Expired && polls==1,
          "next boundary cannot repoll the expired observation");
    Check(handoff.State()==Handoff::Phase::Idle,"all expiry/pending exits leave scheduling idle");
}
void ReentryAndSdkOwner() {
    Handoff handoff;uint64_t now=10;unsigned polls=0;
    auto pending=Step::OwnerPending;
    auto nestedPoll=[&]{++polls;return pending;};
    const auto cancelled=handoff.Service(30,40,[&]{return now;},[&] {
        Check(handoff.Service(100,100,[&]{return now;},nestedPoll)==Result::NotStarted,
              "reentry cannot steal the active observation");
        handoff.Release();
        Check(handoff.Service(100,100,[&]{return now;},nestedPoll)==Result::NotStarted,
              "cancellation cannot reopen service before outer return");
        return pending;
    });
    Check(cancelled==Result::Cancelled && polls==0,"cancel only CPU scheduling, not output ACK or completion");
    GenshinBridgeFg::SdkOwnerActivity activity;
    const int sdkCall=1,foreignCall=2;
    Check(activity.Enter(&sdkCall),"actual owner SDK entry marks its call");
    auto observe=[&] {
        if(!activity.MayWaitForOwner(false))return Result::NotStarted;
        return handoff.Service(30,40,[&]{return now;},nestedPoll);
    };
    Check(observe()==Result::NotStarted && polls==0,"SDK owner activity skips service without any worker/owner wait");
    activity.Leave(&foreignCall);
    Check(!activity.MayWaitForOwner(false),"foreign cleanup cannot erase the real active SDK call");
    activity.Leave(&sdkCall);
    Check(observe()==Result::OwnerPending && polls==1,"exact SDK return permits one runtime pending poll");
}
void InvalidTailNeverReveals() {
    constexpr auto lost=UINT64_MAX;
    for(auto completed:{uint64_t{0},uint64_t{8},lost}) {
        Handoff handoff;uint64_t now=1;bool shown=false;unsigned polls=0;
        const auto result=handoff.Service(4,10,[&]{return now;},[&] {
            ++polls;
            if(FramePipeline::ObserveFence(completed,9)!=FramePipeline::FenceCompletion::Complete)return Step::TailPending;
            shown=true;return Step::Revealed;
        });
        Check(result==Result::TailPending && !shown && polls==1,
              "pending/device-lost tail remains pending without wait, reveal or fake completion");
    }
}
void BusyGateAndNoReacquire() {
    std::recursive_timed_mutex mutex;
    Handoff handoff;uint64_t now=1;unsigned polls=0;
    auto pending=Step::TailPending;
    std::unique_lock owner(mutex);
    auto busy=std::async(std::launch::async,[&] {
        std::unique_lock display(mutex,std::try_to_lock);
        if(!display.owns_lock())return Result::NotStarted;
        return handoff.Service(20,20,[&]{return now;},[&]{++polls;return pending;});
    });
    Check(busy.wait_for(1s)==std::future_status::ready && busy.get()==Result::NotStarted && polls==0,
          "busy gate returns before owner releases it, with no unlocked display read");
    // Try-only observation holds its gate continuously: there is no release or
    // hidden CV/display.lock reacquire that could escape a nominal deadline.
    Check(handoff.Service(20,20,[&]{return now;},[&] {
        auto other=std::async(std::launch::async,[&] {
            std::unique_lock raw(mutex,std::try_to_lock);return raw.owns_lock();
        });
        Check(other.wait_for(1s)==std::future_status::ready && !other.get(),"raw cannot cross the observation's held gate");
        ++polls;return pending;
    })==Result::TailPending && polls==1,"pending callback returns without any relock or worker wait");
    owner.unlock();
    auto next=std::async(std::launch::async,[&] {
        std::unique_lock display(mutex,std::try_to_lock);return display.owns_lock();
    });
    Check(next.get(),"natural output callback can acquire the gate after service ends");
}
}
int main() {
    Alternating335(0);Alternating335(1);Alternating335(2);
    DeadlineBeforePoll();ReentryAndSdkOwner();InvalidTailNeverReveals();BusyGateAndNoReacquire();
    GenshinVisibleChildTestAccess::NativeFallback(Check,false);
    GenshinVisibleChildTestAccess::NativeFallback(Check,true);
    GenshinVisibleChildTestAccess::LateRawCannotCancelNewStream(Check,false);
    GenshinVisibleChildTestAccess::LateRawCannotCancelNewStream(Check,true);
    std::printf("PASS %u CPU checks; 3 x 335 alternating handoffs, 671 adjacent inputs each; try-only single poll; SDK/GPU/display untested\n",checks);
}
