// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <chrono>
#include <condition_variable>
#include <mutex>

class DLSSG_Dx12;
namespace GenshinTransport { class PreparedDepthPacket; }

namespace GenshinBridgeFg {
// Scalar ordering only. The GPU adapter owns/authenticates all resources and
// signals; advancing this object never grants resource or semantic authority.
struct Key {
    uint64_t device=0,context=0,deviceEpoch=0,ownerEpoch=0,configEpoch=0;
    uint64_t instance=0,generation=0,sourceFrame=0,picture=0,present=0;
    uint64_t selected=0,backbuffer=0,swapchain=0;
    uint32_t thread=0,width=0,height=0,selectedSubresource=0;
    uint64_t submitSerial=0,copyOrderedSerial=0,historyEpoch=0;
    bool Known() const noexcept {
        return device && context && deviceEpoch && ownerEpoch && configEpoch && instance && generation &&
            sourceFrame && picture && present && selected && backbuffer && swapchain && thread && width && height &&
            submitSerial && copyOrderedSerial && historyEpoch;
    }
    bool operator==(const Key&) const = default;
    // Temporal scalar ordering only, after independent authentication of both
    // frames. A producer may rotate output allocations. Same-frame lifecycle
    // identity above still binds selected/backbuffer and all submission serials.
    // Camera predecessor, guide epoch/cut/kind and actual child commit remain
    // the adapter's separate prerequisites; this helper grants no authority.
    bool TemporalSuccessorOf(const Key& a) const noexcept {
        const auto& b=*this;
        return a.present<UINT64_MAX && b.present==a.present+1 && b.picture>a.picture &&
            b.sourceFrame>=a.sourceFrame && b.generation>=a.generation &&
            a.device==b.device && a.context==b.context && a.deviceEpoch==b.deviceEpoch &&
            a.ownerEpoch==b.ownerEpoch && a.configEpoch==b.configEpoch && a.instance==b.instance &&
            a.historyEpoch==b.historyEpoch && a.swapchain==b.swapchain && a.thread==b.thread &&
            a.width==b.width && a.height==b.height && a.selectedSubresource==b.selectedSubresource;
    }
};
// Stream ownership deliberately excludes the per-input frame/token/picture.
// It grants no source-to-output join and no GPU retirement authority.
struct SdkStreamOwner {
    uint64_t provider=0,outputOwner=0,device=0;
    uint32_t width=0,height=0;
    bool Known() const noexcept {return provider && outputOwner && device && width && height;}
    bool operator==(const SdkStreamOwner&) const = default;
};
// Exact physical identity captured by FinalOutputEndpoint. These scalars grant
// no completion: only the endpoint's real queue/fence can retire their lifetime.
struct PhysicalStream {
    uint64_t stream=0,generation=0,chain=0,queue=0,fence=0;
    bool Known() const noexcept {return stream && generation && chain && queue && fence;}
    bool operator==(const PhysicalStream&) const = default;
};
// One SDK creation owner and one physical buffer lifetime. Shared by synchronous
// and worker output admission; neither path can relabel a new queue/generation.
// Call retirement methods only at the endpoint's positively drained seams.
class SdkOutputBinding final {
    SdkStreamOwner owner_{};
    uint64_t device12_=0;
    PhysicalStream physical_{};
    bool buffersRetired_=false;
public:
    const SdkStreamOwner& Owner() const noexcept {return owner_;}
    uint64_t Device12() const noexcept {return device12_;}
    const PhysicalStream& Physical() const noexcept {return physical_;}
    bool BindOwner(const SdkStreamOwner& owner,uint64_t device12) noexcept {
        if(!owner.Known() || !device12)return false;
        if(owner_.Known() && (owner_.provider!=owner.provider || owner_.outputOwner!=owner.outputOwner ||
            owner_.device!=owner.device || device12_!=device12 || (owner_!=owner && !buffersRetired_)))return false;
        owner_=owner;device12_=device12;buffersRetired_=false;return true;
    }
    bool BindPhysical(uint64_t outputOwner,const PhysicalStream& physical) noexcept {
        if(!owner_.Known() || outputOwner!=owner_.outputOwner || !physical.Known() ||
            (physical_.Known() && physical_!=physical))return false;
        physical_=physical;return true;
    }
    bool Matches(uint64_t outputOwner,const PhysicalStream& physical) const noexcept {
        return owner_.Known() && outputOwner==owner_.outputOwner && physical.Known() && physical_==physical;
    }
    bool RetireBuffers(uint64_t outputOwner,uint64_t stream,uint64_t generation) noexcept {
        if(!physical_.Known() || outputOwner!=owner_.outputOwner ||
            physical_.stream!=stream || physical_.generation!=generation)return false;
        physical_={};buffersRetired_=true;return true; // next input may bind the resized extent, not a new provider
    }
    bool RetireProvider(uint64_t outputOwner) noexcept {
        if(!owner_.Known() || !outputOwner || outputOwner!=owner_.outputOwner)return false;
        owner_={};device12_=0;physical_={};buffersRetired_=false;return true; // also clears a scope with no raw binding
    }
};
// A real authenticated SDK call may take longer than a display lease. Keep its
// stream scope alive until that call's postvalidation finishes; elapsed CPU time
// must not revoke a worker halfway through the call. This proves no GPU work.
class SdkStreamLease final {
    uint64_t until_=0;
    bool open_=false,accepted_=false;
public:
    bool Begin(uint64_t now,uint64_t duration) noexcept {
        if(open_ || !duration || now>UINT64_MAX-duration)return false;
        open_=true;until_=now+duration;return true;
    }
    bool Accept(uint64_t now,uint64_t duration) noexcept {
        if(!open_ || !duration || now>UINT64_MAX-duration)return false;
        accepted_=true;until_=now+duration;return true;
    }
    void End(bool accepted) noexcept {
        open_=false;
        if(!accepted)Revoke();
    }
    void Revoke() noexcept {until_=0;open_=accepted_=false;}
    bool Current(uint64_t now) const noexcept {return until_ && (open_ || now<until_);}
    bool Accepted() const noexcept {return accepted_;}
    uint64_t Until() const noexcept {return until_;}
};
// The wrapper pauses NEW INPUTS between SDK calls to let an output tail drain.
// Already scheduled worker raws must wait for that owner handoff, not return a
// synthetic DXGI error: Streamline defers that error to the next host Present,
// which correctly tears down the provider. Neither a wake nor a deadline proves
// GPU completion; callers recheck the original stream/queue/fence identities.
class OutputRevealWindow final {
    uint64_t sequence_=0,until_=0;
    std::condition_variable_any ownerChanged_;
public:
    bool Hold(uint64_t sequence,uint64_t now,uint64_t outputUntil,uint64_t inputUntil) noexcept {
        if(!sequence)return false;
        if(sequence_!=sequence) {
            sequence_=sequence;until_=outputUntil<inputUntil ? outputUntil : inputUntil;
        }
        return BlocksRaw(now);
    }
    bool BlocksRaw(uint64_t now) const noexcept {return sequence_ && now<until_;}
    uint64_t Remaining(uint64_t now) const noexcept {return BlocksRaw(now)?until_-now:0;}
    template<class Mutex,class Ready>
    bool AwaitOwner(std::unique_lock<Mutex>& lock,uint64_t budgetMs,Ready ready) noexcept {
        // A recursive mutex must be held exactly once here. Bind's two callers
        // have returned from all recursive helpers before entering this wait.
        try{return ownerChanged_.wait_for(lock,std::chrono::milliseconds(budgetMs),std::move(ready));}
        catch(...){return false;} // no exception may escape the SDK Present hook
    }
    void OwnerChanged() noexcept {ownerChanged_.notify_all();}
    void Release() noexcept {sequence_=until_=0;OwnerChanged();}
};
// CPU source identity only. The real provider seals it after the SDK accepts
// constants with this exact FrameToken value. It keeps the immutable packet;
// SDK acceptance, GPU retirement and physical output are separate facts.
// Joining an async raw to THIS receipt needs a documented callback carrying it;
// the independent output-stream scope deliberately makes no such source join.
// Neither an input fence nor an NVAPI present counter can select a source.
class SdkInputSource final {
    friend class ::DLSSG_Dx12;
    const Key key_;
    const uint64_t frame_,token_,provider_,epoch_,outputOwner_;
    const std::shared_ptr<const GenshinTransport::PreparedDepthPacket> packet_;
    SdkInputSource(const Key& key,uint64_t frame,uint64_t token,uint64_t provider,uint64_t epoch,
        uint64_t outputOwner,std::shared_ptr<const GenshinTransport::PreparedDepthPacket> packet)
        :key_(key),frame_(frame),token_(token),provider_(provider),epoch_(epoch),outputOwner_(outputOwner),packet_(std::move(packet)){}
public:
    const Key& Identity()const noexcept{return key_;}
    uint64_t SdkFrame()const noexcept{return token_;} // copied from the actual SetConstants FrameToken
    const auto& Packet()const noexcept{return packet_;}
    SdkStreamOwner StreamOwner() const noexcept {
        return {provider_,outputOwner_,key_.device,key_.width,key_.height};
    }
    bool Matches(const Key& key,uint64_t frame)const noexcept {
        return packet_ && provider_ && epoch_ && outputOwner_ && frame_ && frame_<=UINT32_MAX && token_==frame_ &&
            key_.Known() && key_==key && frame_==frame;
    }
};
// Saved input-call bracket, independent of Reflex marker policy. Callers must
// authenticate the current dispatch/thread/chain before Enter and Return. The
// exact HRESULT is saved once; S_OK permits an input query, never output reveal
// or GPU retirement. Keeping this scalar contract portable permits CPU tests.
class SdkInputCall final {
    std::shared_ptr<const SdkInputSource> source_;
    bool returned_=false;
    std::int32_t result_=static_cast<std::int32_t>(0x8000000Au); // E_PENDING
public:
    SdkInputCall()=default;
    SdkInputCall(const SdkInputCall&)=delete;
    SdkInputCall& operator=(const SdkInputCall&)=delete;
    bool Enter(std::shared_ptr<const SdkInputSource> source,const Key& key,uint64_t frame) noexcept {
        if(source_ || returned_ || !source || !source->Matches(key,frame))return false;
        source_=std::move(source);return true;
    }
    bool Return(std::int32_t result) noexcept {
        if(!Entered())return false;
        result_=result;returned_=true;return true;
    }
    bool Entered() const noexcept {return source_ && !returned_;}
    bool Returned() const noexcept {return returned_;}
    bool Succeeded() const noexcept {return returned_ && source_ && result_==0;}
    std::int32_t Result() const noexcept {return result_;}
    const auto& Source() const noexcept {return source_;}
};
// Provider-history policy is independent of HWND visibility. An ordinary
// ineligible frame cannot certify continuity and cannot erase the last real
// submission. Once a NEW fully authenticated current frame is available, a
// stale/nonadjacent submitted predecessor starts a fresh provider history.
// This never replays an old picture and never relaxes TemporalSuccessorOf for
// a continuing history. Reject is reserved for an inconsistent policy state.
enum class ContinuityAction : unsigned char { Seed, Continue, Reject, Reset };
inline ContinuityAction DecideContinuity(bool known,bool continues,bool /*resetRequested*/) noexcept {
    if(continues)return known?ContinuityAction::Continue:ContinuityAction::Reject;
    if(!known)return ContinuityAction::Seed;
    return ContinuityAction::Reset;
}
enum class Phase : unsigned char { Empty, Recorded, Returned, Copied, Tagged, Dispatched, InputAccepted, Revoked };
class Lifecycle final {
    Key key_{};
    Phase phase_=Phase::Empty;
public:
    bool Record(const Key& key) noexcept {
        if(phase_!=Phase::Empty || !key.Known())return false;
        key_=key;phase_=Phase::Recorded;return true;
    }
    bool Advance(const Key& key,Phase from,Phase to) noexcept {
        const bool edge=(from==Phase::Recorded && to==Phase::Returned) ||
            (from==Phase::Returned && to==Phase::Copied) || (from==Phase::Copied && to==Phase::Tagged) ||
            (from==Phase::Tagged && to==Phase::Dispatched) || (from==Phase::Dispatched && to==Phase::InputAccepted);
        if(!edge || phase_!=from || key!=key_)return false;
        phase_=to;return true;
    }
    // A native-only drain may discard exactly its Returned recording. This
    // revokes CPU admission only; the Frame must retain its producer queue/fence
    // and leases until the ordinary real GPU retirement path observes completion.
    bool DiscardReturned(const Key& key,uint64_t present,uint32_t thread,uint64_t swapchain,uint64_t context) noexcept {
        if(phase_!=Phase::Returned || !key.Known() || key!=key_ || key.present!=present ||
            key.thread!=thread || key.swapchain!=swapchain || key.context!=context)return false;
        phase_=Phase::Revoked;return true;
    }
    void Revoke() noexcept {phase_=Phase::Revoked;}
    Phase State() const noexcept {return phase_;}
    const Key& Identity() const noexcept {return key_;}
};
}
