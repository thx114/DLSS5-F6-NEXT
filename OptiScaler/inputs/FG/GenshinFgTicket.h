// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <utility>

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
// CPU source identity only. The real provider seals it before tagging this
// input. It keeps the shared immutable source packet, never a global/latest
// token. SDK acceptance, GPU retirement and physical output are separate facts.
// An async raw call still needs a documented callback carrying THIS receipt;
// neither an input fence nor an NVAPI present counter can select it.
class SdkInputSource final {
    friend class ::DLSSG_Dx12;
    const Key key_;
    const uint64_t frame_,provider_,epoch_;
    const std::shared_ptr<const GenshinTransport::PreparedDepthPacket> packet_;
    SdkInputSource(const Key& key,uint64_t frame,uint64_t provider,uint64_t epoch,
        std::shared_ptr<const GenshinTransport::PreparedDepthPacket> packet)
        :key_(key),frame_(frame),provider_(provider),epoch_(epoch),packet_(std::move(packet)){}
public:
    const Key& Identity()const noexcept{return key_;}
    uint64_t SdkFrame()const noexcept{return frame_;}
    const auto& Packet()const noexcept{return packet_;}
    bool Matches(const Key& key,uint64_t frame)const noexcept {
        return packet_ && provider_ && epoch_ && frame_ && frame_<=UINT32_MAX &&
            key_.Known() && key_==key && frame_==frame;
    }
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
    void Revoke() noexcept {phase_=Phase::Revoked;}
    Phase State() const noexcept {return phase_;}
    const Key& Identity() const noexcept {return key_;}
};
}
