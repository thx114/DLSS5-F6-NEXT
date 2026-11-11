// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <array>

namespace FramePipeline {
// CPU producer progress only. A Render observation/target match is never an
// SDK acceptance, display receipt, GPU completion or permission to reuse input.
class ProducerProgress final {
public:
    struct Owner {
        uintptr_t pipeline=0,device=0,context=0;
        uint64_t deviceEpoch=0,ownerEpoch=0,configEpoch=0,instance=0;
        uint32_t thread=0;
        bool Valid() const noexcept {
            return pipeline && device && context && deviceEpoch && ownerEpoch && configEpoch && instance && thread;
        }
        bool operator==(const Owner&) const noexcept=default;
    };
    struct Production {Owner owner{};uint64_t generation=0;};
    enum class Phase {Idle,Tracking,Stalled,Recovering};
    static constexpr uint64_t StallGenerations=4,ProbeCooldown=16;
    static constexpr unsigned ProbeBudget=8;
private:
    Owner owner_{};
    uint64_t rendered_=0,anchor_=0,produced_=0,lastProbe_=0;
    unsigned probes_=0,offers_=0;
    Phase phase_=Phase::Idle;
    bool stalled_=false;
public:
    void Reset() noexcept {*this={};}
    const Owner& Identity() const noexcept {return owner_;}
    Phase State() const noexcept {return phase_;}
    uint64_t Rendered() const noexcept {return rendered_;}
    uint64_t Produced() const noexcept {return produced_;}
    unsigned Probes() const noexcept {return probes_;}
    void Observe(const Owner& owner,uint64_t rendered,const Production& target) noexcept {
        if(!owner.Valid() || !rendered || rendered==UINT64_MAX){phase_=Phase::Idle;return;}
        const bool replacement=owner!=owner_;
        if(replacement){Reset();owner_=owner;rendered_=anchor_=rendered;}
        // A regression is an unknown interval, not wraparound progress or a new
        // probe allowance. Only a genuinely different owner resets the budget.
        if(rendered<rendered_){phase_=Phase::Idle;return;}
        const bool advanced=rendered>rendered_;
        rendered_=rendered;
        const bool matching=target.owner==owner && target.generation && target.generation<=rendered;
        if(matching && target.generation>produced_){
            produced_=target.generation;anchor_=produced_;
            phase_=stalled_?Phase::Recovering:Phase::Tracking;stalled_=false;return;
        }
        if(!advanced){phase_=Phase::Idle;return;}
        if(rendered>=anchor_ && rendered-anchor_>=StallGenerations){phase_=Phase::Stalled;stalled_=true;}
        else phase_=Phase::Tracking;
    }
    bool ProbeDue(const Owner& nativeOwner) const noexcept {
        return phase_==Phase::Stalled && owner_==nativeOwner &&
            probes_<ProbeBudget && (!lastProbe_ || (rendered_>=lastProbe_ && rendered_-lastProbe_>=ProbeCooldown));
    }
    // Eligible draw offsets after each cooldown, accumulated across Render
    // boundaries if a frame is short. Never repeatedly select just its first PS.
    inline static constexpr std::array<unsigned,ProbeBudget> DrawOffsets{0,7,31,127,3,15,63,255};
    bool OfferProbe(const Owner& nativeOwner) noexcept {
        if(!ProbeDue(nativeOwner))return false;
        if(offers_++<DrawOffsets[probes_])return false;
        offers_=0;++probes_;lastProbe_=rendered_;return true;
    }
};
}
