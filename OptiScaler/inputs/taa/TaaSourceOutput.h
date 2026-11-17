// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace taa::source_output {
// A synchronous producer capability, separate from UI/API names and diagnostic
// metadata. It never owns resources or certifies GPU completion.
struct Call {
    uintptr_t context=0, parameters=0, color=0, depth=0, motion=0, output=0;
    unsigned handle=0;
    uint64_t request=0;
    bool Valid() const noexcept {
        return context && parameters && color && depth && motion && output && handle && request && request!=UINT64_MAX;
    }
    bool Matches(const Call& other) const noexcept {
        return Valid() && other.Valid() && context==other.context && parameters==other.parameters &&
            color==other.color && depth==other.depth && motion==other.motion && output==other.output &&
            handle==other.handle && request==other.request;
    }
};
inline thread_local uintptr_t creationContext=0,creationParameters=0;
class CreationScope final {
    uintptr_t context_=creationContext,parameters_=creationParameters;
public:
    CreationScope(uintptr_t context,uintptr_t parameters) noexcept {creationContext=context;creationParameters=parameters;}
    ~CreationScope(){creationContext=context_;creationParameters=parameters_;}
    CreationScope(const CreationScope&)=delete;
    CreationScope& operator=(const CreationScope&)=delete;
};
inline bool Creating(uintptr_t context,uintptr_t parameters) noexcept {
    return context && parameters && context==creationContext && parameters==creationParameters;
}
inline thread_local const Call* current=nullptr;
inline thread_local bool bridgeCall=false;
inline thread_local bool matchedCall=false;
inline std::atomic_bool bridgeSelected{false};
class Scope final {
    const Call* previous_=current;
    bool previousBridge_=bridgeCall,previousMatched_=matchedCall;
    bool previousSelection_=bridgeSelected.load(std::memory_order_acquire);
    Call call_;
public:
    explicit Scope(Call call) noexcept : call_(call) {current=&call_;bridgeCall=false;matchedCall=false;}
    ~Scope(){current=previous_;bridgeCall=previousBridge_;matchedCall=previousMatched_;if(previous_)bridgeSelected.store(previousSelection_,std::memory_order_release);}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
inline bool SelectBackend(const Call& actual,bool bridge) noexcept {
    const bool match=current && current->Matches(actual);
    matchedCall=match;
    bridgeCall=match && bridge;
    // An unrelated NGX call must not acquire or revoke the TAA owner's route.
    if(current)bridgeSelected.store(bridgeCall,std::memory_order_release);
    return bridgeCall;
}
// PassDx11 has decoded these guides into colour-row order and pixel vectors.
// This conveys coordinates only, never optical-flow or GPU-completion authority.
inline bool PreparedRasterGuides(unsigned handle) noexcept {
    return current && current->Valid() && matchedCall && current->handle==handle;
}
inline bool Authorized(unsigned long long generation) noexcept {
    return current && current->Valid() && bridgeCall && current->handle==generation;
}
inline bool Selected(bool ownsInput) noexcept {
    return bridgeSelected.load(std::memory_order_acquire) && (ownsInput || (current && bridgeCall));
}
inline void Reset() noexcept {bridgeSelected.store(false,std::memory_order_release);}
} // namespace taa::source_output
