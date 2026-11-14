// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <utility>

// CPU association only. Fence/resource owners live in the DX12 adapter. This
// binds the exact SDK invocation and query; it cannot imply GPU completion.
namespace framegen::input_present
{
struct Binding
{
    uint64_t provider=0, sdkEpoch=0, bufferEpoch=0, inputEpoch=0;
    uintptr_t chain=0, buffer=0, api=0;
    uint32_t viewport=0, thread=0;
    bool Known() const noexcept
    {
        return provider && provider!=UINT64_MAX && sdkEpoch && sdkEpoch!=UINT64_MAX &&
            bufferEpoch && bufferEpoch!=UINT64_MAX && inputEpoch && inputEpoch!=UINT64_MAX &&
            chain && buffer && api && thread;
    }
    bool SameInput(const Binding& other) const noexcept
    {
        return Known() && other.Known() && provider==other.provider && sdkEpoch==other.sdkEpoch &&
            bufferEpoch==other.bufferEpoch && inputEpoch==other.inputEpoch && chain==other.chain && buffer==other.buffer &&
            api==other.api && viewport==other.viewport;
    }
    bool operator==(const Binding&) const = default;
};

class Domain final
{
  public:
    struct Attempt
    {
        uint64_t serial=0;
        Binding binding {};
        explicit operator bool() const noexcept { return serial!=0; }
    };
  private:
    Attempt active_ {};
    uint64_t serial_=0;
    bool ambiguous_=false;
  public:
    // The provider execution guard serializes different threads. Same-thread
    // SDK callbacks may reenter: revoke the association before refusing them.
    Attempt Begin(const Binding& binding) noexcept
    {
        if (active_) { ambiguous_=true; return {}; }
        if (!binding.Known() || serial_>=UINT64_MAX-1) return {};
        ambiguous_=false;
        active_={++serial_,binding};
        return active_;
    }
    bool Current(const Attempt& attempt,const Binding& current) const noexcept
    {
        return attempt && active_ && !ambiguous_ && attempt.serial==active_.serial &&
            attempt.binding==active_.binding && current==attempt.binding;
    }
    bool Complete(const Attempt& attempt,const Binding& current,bool sdkSucceeded,
                  bool querySucceeded,uint64_t querySequence,uint32_t queryThread) noexcept
    {
        const bool matches=Current(attempt,current) && sdkSucceeded && querySucceeded &&
            querySequence && queryThread==attempt.binding.thread;
        if (attempt && attempt.serial==active_.serial) { active_={}; ambiguous_=false; }
        return matches;
    }
    void Cancel(const Attempt& attempt) noexcept
    {
        if (attempt && attempt.serial==active_.serial) { active_={}; ambiguous_=false; }
    }
    void Revoke() noexcept { if (active_) ambiguous_=true; }
    bool Active() const noexcept { return bool(active_); }
};

enum class Api { Present, Present1 };

// An SDK's original Present may forward once to the other DXGI entry point.
// The ticket belongs to this thread, chain and invocation, not a global skip
// flag. A repeated/same-entry callback revokes the outer query association.
class Forwarding final
{
    inline static thread_local Forwarding* current_=nullptr;
    Forwarding* previous_;
    Domain& domain_;
    uintptr_t chain_;
    Api api_;
    bool consumed_=false,presenting_=false;
  public:
    class PresentScope final
    {
        friend class Forwarding;
        Forwarding& owner_;
        bool previous_;
        explicit PresentScope(Forwarding& owner) noexcept
            : owner_(owner),previous_(std::exchange(owner.presenting_,true)) {}
      public:
        PresentScope(const PresentScope&)=delete;
        PresentScope& operator=(const PresentScope&)=delete;
        ~PresentScope() {owner_.presenting_=previous_;}
    };
    Forwarding(Domain& domain,uintptr_t chain,Api api) noexcept
        : previous_(current_),domain_(domain),chain_(chain),api_(api) {current_=this;}
    Forwarding(const Forwarding&)=delete;
    Forwarding& operator=(const Forwarding&)=delete;
    ~Forwarding() {current_=previous_;}
    static bool Active() noexcept {return current_!=nullptr;}
    PresentScope EnterPresent() noexcept {return PresentScope(*this);}
    static bool Admit(uintptr_t chain,Api api) noexcept
    {
        for (auto* entry=current_;entry;entry=entry->previous_)
        {
            if (entry->chain_!=chain) continue;
            if (!entry->presenting_ || entry->api_==api || entry->consumed_)
            {
                entry->domain_.Revoke();
                return false;
            }
            entry->consumed_=true;
            return true;
        }
        return true;
    }
    static void Revoke(uintptr_t chain) noexcept
    {
        for (auto* entry=current_;entry;entry=entry->previous_)
            if (entry->chain_==chain) entry->domain_.Revoke();
    }
};
}
