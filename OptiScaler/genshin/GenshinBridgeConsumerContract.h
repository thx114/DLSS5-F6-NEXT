// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinOutputContract.h"
#include <array>
#include <utility>

namespace GenshinCaller {
// CPU vocabulary only. Validating this snapshot cannot authenticate a GPU observation
// or guide semantics. Only BridgeAfterUpscale reads the corresponding live objects.
struct BindingSnapshot {
    GenshinGuides::Owner owner;
    std::array<uint64_t,4> source{}, imported{}, bound{};
    uint64_t parameters=0, queue=0, commandList=0, consumerDevice=0;
};
enum class BindingReason : unsigned char { None, Owner, Resources, Consumer, Changed };
inline BindingReason ValidateBinding(const BindingSnapshot& s) noexcept {
    const auto& o=s.owner;
    // sourceFrame==0 is a valid first frame, never an "unknown" sentinel.
    if(!o.device || !o.context || !o.deviceEpoch || !o.producerEpoch || !o.configEpoch ||
       !o.instance || !o.generation || !o.historyEpoch || !o.thread)return BindingReason::Owner;
    if(!s.parameters || !s.queue || !s.commandList || !s.consumerDevice)return BindingReason::Consumer;
    for(size_t i=0;i<s.source.size();++i) {
        if(!s.source[i] || !s.imported[i] || s.bound[i]!=s.imported[i])return BindingReason::Resources;
        for(size_t j=0;j<i;++j)
            if(s.source[j]==s.source[i] || s.imported[j]==s.imported[i])return BindingReason::Resources;
    }
    return BindingReason::None;
}
inline BindingReason SameBinding(const BindingSnapshot& a,const BindingSnapshot& b) noexcept {
    if(ValidateBinding(b)!=BindingReason::None)return ValidateBinding(b);
    return GenshinGuides::SameOwner(a.owner,b.owner) && a.source==b.source && a.imported==b.imported &&
        a.bound==b.bound && a.parameters==b.parameters && a.queue==b.queue &&
        a.commandList==b.commandList && a.consumerDevice==b.consumerDevice
        ? BindingReason::None:BindingReason::Changed;
}
inline GenshinGuides::OutputRequest AfterUpscaleNrRequest(const BindingSnapshot& s) noexcept {
    using namespace GenshinGuides;
    return {{Consumer::NoiseReduction,PicturePoint::AfterUpscale,ConsumerApi::BridgeDx12,
             FgInput::NoFG,FgOutput::NoFG},s.bound[3],s.consumerDevice,s.commandList};
}
// Records a real invocation made by this object, not a caller supplied "evaluated" flag.
// This witnesses provider return only; no provider/NR/GPU/presentation success is implied.
class ProviderInvocation final {
    unsigned calls_=0;
    bool succeeded_=false,consumerOffered_=false,invalid_=false,consumerAbandoned_=false;
public:
    template<class Evaluate> bool Run(Evaluate&& evaluate) {
        if(calls_ || invalid_){invalid_=true;return false;}
        ++calls_;
        // On an exception the attempt stays failed and cannot be retried.
        succeeded_=static_cast<bool>(std::forward<Evaluate>(evaluate)());
        return succeeded_;
    }
    bool Succeeded()const noexcept{return !invalid_ && calls_==1 && succeeded_;}
    // A failed observation removes consumer authority without suppressing SR.
    void AbandonConsumer()noexcept{consumerAbandoned_=true;}
    bool BeginConsumer()noexcept {
        if(!Succeeded() || consumerOffered_ || consumerAbandoned_)return false;
        consumerOffered_=true;return true;
    }
    unsigned Calls()const noexcept{return calls_;}
    bool ConsumerOffered()const noexcept{return consumerOffered_;}
};
}
