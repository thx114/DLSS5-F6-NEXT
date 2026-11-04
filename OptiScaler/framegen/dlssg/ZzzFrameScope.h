#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <sl_core_types.h>
#include "DlssgInputContinuity.h"

// ZZZ synchronous SL DLSS -> selected NGX input only. These CPU receipts grant
// consumption, never resource retirement. No token objects, COM owners or GPU
// locks cross into an SDK callback.
namespace dlssg::zzz
{
inline std::atomic<uintptr_t> gameApi { 0 };

// Multiple/virtual inheritance gives one feature different interface addresses.
// Bind both selected entry and publication through the same explicit base type;
// comparing raw addresses of different interfaces rejects the very same object.
template<class CommonBase, class Feature>
uintptr_t SelectedFeatureIdentity(Feature* feature)
{
    return reinterpret_cast<uintptr_t>(static_cast<CommonBase*>(feature));
}
struct Origin
{
    uintptr_t api=0, list=0;
    uint32_t feature=0, viewport=0, frame=0;
    bool frameKnown=false, viewportKnown=false;
};

struct GameViewport
{
    uint32_t value=0;
    bool known=false;
};

// SL inputs may be arrays of chained structures. The whole bounded traversal
// must identify one real viewport; absence, cycles and ambiguity grant nothing.
inline GameViewport ParseGameViewport(const sl::BaseStructure* const* inputs, uint32_t count)
{
    if (!inputs || !count || count>64) return {};
    std::array<const sl::BaseStructure*,128> visited {};
    size_t used=0;
    GameViewport result;
    for (uint32_t i=0;i<count;++i) for (auto* node=inputs[i];node;node=node->next)
    {
        if (used==visited.size()) return {};
        for (size_t j=0;j<used;++j) if (visited[j]==node) return {};
        visited[used++]=node;
        if (node->structType==sl::ViewportHandle::s_structType)
        {
            if (result.known || node->structVersion<sl::kStructVersion1) return {};
            result={static_cast<uint32_t>(*static_cast<const sl::ViewportHandle*>(node)),true};
        }
    }
    return result;
}

enum class Outcome { Pending, Succeeded, Failed };
struct Receipt
{
    const Origin origin;
    std::atomic<Outcome> upstream { Outcome::Pending };
    explicit Receipt(Origin value) : origin(value) {}
    bool Preparing() const { return upstream.load(std::memory_order_acquire)!=Outcome::Failed; }
    bool Consumable(bool selectedSucceeded) const
    { return selectedSucceeded && upstream.load(std::memory_order_acquire)==Outcome::Succeeded; }
};

class Selection;
class Scope
{
    friend class Selection;
    inline static thread_local Scope* active=nullptr;
    Scope* previous;
    bool claimed=false;
    std::shared_ptr<Receipt> receipt;
  public:
    Scope(Origin origin, bool actualGameDlss)
        : previous(active)
    {
        // Every invocation shadows its parent, including invalid/non-DLSS and
        // local backend calls. A valid inner call has its own origin/receipt;
        // it cannot borrow its parent's authorization or completion result.
        active=this;
        if (actualGameDlss && origin.api && origin.list && origin.frameKnown &&
            origin.viewportKnown && origin.api==gameApi.load(std::memory_order_acquire))
            receipt=std::make_shared<Receipt>(origin);
    }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    void Finish(bool success)
    {
        if (!receipt) return;
        auto pending=Outcome::Pending;
        receipt->upstream.compare_exchange_strong(pending,success?Outcome::Succeeded:Outcome::Failed,
                                                  std::memory_order_release,std::memory_order_relaxed);
    }
    ~Scope() { Finish(false); active=previous; }
};

class Selection
{
    inline static thread_local Selection* active=nullptr;
    Selection* previous;
    uintptr_t selected=0;
    uintptr_t provider=0;
    uint32_t providerViewport=0;
    std::shared_ptr<Receipt> receipt;
  public:
    Selection(uintptr_t api, uint32_t feature, uint32_t localViewport, uintptr_t list,
              uintptr_t selectedFeature, uintptr_t providerOwner)
        : previous(active), selected(selectedFeature), provider(providerOwner), providerViewport(localViewport)
    {
        active=this; // Invalid nested selected entries also shadow, never walk parents.
        auto* scope=Scope::active;
        if (!scope || scope->claimed || !scope->receipt || !selected || !provider) return;
        const auto& o=scope->receipt->origin;
        if (api!=o.api || api!=gameApi.load(std::memory_order_acquire) || feature!=o.feature ||
            !list || list!=o.list ||
            scope->receipt->upstream.load(std::memory_order_acquire)!=Outcome::Pending) return;
        scope->claimed=true;
        receipt=scope->receipt;
    }
    Selection(const Selection&)=delete;
    Selection& operator=(const Selection&)=delete;
    ~Selection() { active=previous; }
    static std::shared_ptr<Receipt> Current(uintptr_t list, uintptr_t selectedFeature,
                                            uint32_t localViewport, uintptr_t providerOwner)
    {
        if (!active || !active->receipt || !Scope::active ||
            Scope::active->receipt!=active->receipt || !active->receipt->Preparing()) return {};
        const auto& o=active->receipt->origin;
        return list==o.list && selectedFeature==active->selected && localViewport==active->providerViewport &&
            providerOwner==active->provider ? active->receipt : nullptr;
    }
};

// SDK acknowledgement only, under the provider's existing continuity lock.
// No GPU owner is released by Off. Every actual options attempt invalidates
// this proof first, including reentrant calls while an Off is in progress.
class GapOffProof
{
    InputOwner owner {};
    uint32_t viewport=0;
    bool accepted=false, pending=false;
  public:
    void Invalidate() { accepted=false; pending=false; }
    bool Current(const InputOwner& value, uint32_t localViewport) const
    { return accepted && !pending && owner==value && viewport==localViewport; }
    template<class SetOff, class StillSafe>
    bool Ensure(const InputOwner& value, uint32_t localViewport, SetOff setOff, StillSafe stillSafe)
    {
        if (!stillSafe()) { Invalidate(); return false; }
        if (Current(value,localViewport)) return true;
        Invalidate();
        pending=true;
        const bool succeeded=setOff();
        const bool unchanged=pending;
        pending=false;
        accepted=unchanged && succeeded && stillSafe();
        if (accepted) { owner=value; viewport=localViewport; }
        return accepted;
    }
};

enum class PresentAction { PassThrough, Consume, KnownOffSafeBasePresent, UnsafeFailure };

inline bool ConsumePresentOnce(bool& presented)
{
    if (presented) return false;
    presented=true;
    return true;
}

// Logical inactivity cannot erase a failed SDK Off. The provider supplies its
// existing teardown/owner state; this grants no GPU retirement authority.
inline bool BasePresentSafe(bool teardownPending, bool teardownOffAccepted, bool ownerUnsafe)
{
    return !ownerUnsafe && (!teardownPending || teardownOffAccepted);
}

// attempted is latched in the actual Dispatch branch before vendor callbacks.
// An inactive path still checks unresolved SDK state from earlier Presents.
template<class BeginSource, class EnsureOff>
PresentAction ResolvePresent(bool attempted, bool dispatched, bool unattemptedSafe,
    BeginSource beginSource, EnsureOff ensureOff)
{
    if (!attempted) return unattemptedSafe ? PresentAction::PassThrough : PresentAction::UnsafeFailure;
    if (dispatched && beginSource()) return PresentAction::Consume;
    return ensureOff() ? PresentAction::KnownOffSafeBasePresent : PresentAction::UnsafeFailure;
}

template<class Result, class BasePresent>
Result RunPresent(PresentAction action, Result unsafeResult, BasePresent basePresent)
{
    return action==PresentAction::UnsafeFailure ? unsafeResult : basePresent();
}

// The ID is copied before the vendor call; even a recursive callback cannot
// rewrite a source receipt through the frameIndex argument.
template<class Token, class GetToken>
bool TokenFor(uint32_t frame, Token*& token, GetToken getToken)
{
    token=nullptr;
    if (!getToken) return false;
    const uint32_t requested=frame;
    return getToken(token,&requested)==decltype(getToken(token,&requested))::eOk && token &&
        static_cast<uint32_t>(*token)==frame;
}
}
