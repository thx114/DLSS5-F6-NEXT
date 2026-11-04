// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <new>
#include "GenshinNativeQueryProvenance.h"

namespace GenshinQueryScope {
// Fresh is a creation-time witness, never inferred from Install, Present,
// ClearState, a matched End, NR settings, or depth evidence. The existing
// normal Install API supplies no witness: those states start UnknownPast.
// Only the private, one-use factory witness can seed a previously absent
// context state as fresh. There is no promotion/reset of an existing state.
enum class Origin { UnknownPast, FreshBeforeExposure };
class Ranges final {
    std::array<uint64_t,64> active_{};
    std::array<uintptr_t,64> callers_{}; // Return-address observation, no authority.
    std::array<unsigned,64> types_{}; // Observation only, never query admission.
    unsigned count_=0;
    const bool complete_;
    bool invalid_=false;
    const char* invalidReason_="none";
public:
    explicit Ranges(Origin origin=Origin::UnknownPast)noexcept:complete_(origin==Origin::FreshBeforeExposure){}
    void Invalidate(const char* reason="opaque-boundary")noexcept {if(!invalid_)invalidReason_=reason;invalid_=true;}
    void Begin(uint64_t object,unsigned query=UINT32_MAX,uintptr_t caller=0)noexcept {
        if(!object || count_==active_.size()){Invalidate(!object?"zero-query-cookie":"range-capacity");return;}
        for(unsigned i=0;i<count_;++i)if(active_[i]==object){Invalidate("duplicate-begin");return;}
        callers_[count_]=caller;types_[count_]=query;active_[count_++]=object;
    }
    void End(uint64_t object)noexcept {
        for(unsigned i=0;i<count_;++i)if(object && active_[i]==object){active_[i]=active_[--count_];types_[i]=types_[count_];callers_[i]=callers_[count_];return;}
        Invalidate("end-without-begin"); // Includes End whose Begin preceded attachment.
    }
    bool KnownQuiescent()const noexcept {return complete_ && !invalid_ && !count_;}
    bool KnownHistory()const noexcept {return complete_ && !invalid_;}
    unsigned Active()const noexcept {return count_;}
    uintptr_t FirstCaller()const noexcept {return count_?callers_[0]:0;}
    uint64_t ActiveTypes()const noexcept {
        uint64_t mask=0;for(unsigned i=0;i<count_;++i)mask|=uint64_t(1)<<(types_[i]<63?types_[i]:63);return mask;
    }
    bool Complete()const noexcept {return complete_;}
    const char* InvalidReason()const noexcept {return invalidReason_;}
};
using Microsoft::WRL::ComPtr;
inline constexpr GUID StateKey{0xa8115081,0x6d31,0x4c4c,{0x81,0x55,0xa4,0x72,0xc2,0xc6,0x08,0x11}};
inline constexpr GUID AsyncKey{0xa8115082,0x6d31,0x4c4c,{0x81,0x55,0xa4,0x72,0xc2,0xc6,0x08,0x11}};
inline constexpr GUID StateModeKey{0xa8115121,0x6d31,0x4c4c,{0x81,0x55,0xa4,0x72,0xc2,0xc6,0x08,0x11}};
// Serialize private-data creation/updates by canonical object identity.
// Fixed try-only stripes never wait; a collision or concurrent alias access
// loses observation coverage globally (optional mapping only), permanently.
inline std::array<std::atomic_flag,64> objectBusy{};
inline std::atomic<bool> coverageLost{false};
// R790: bounded immutable diagnostic receipts. They never grant coverage or
// create context state; publication has one winner and readers never wait.
struct CoverageLossReceipt {
    std::atomic<unsigned> ready{0};
    const char* reason="none";uintptr_t object=0;DWORD thread=0;
};
inline CoverageLossReceipt firstCoverageLoss;
inline void LoseCoverage(const char* reason,const void* object=nullptr)noexcept {
    coverageLost=true;unsigned expected=0;
    if(firstCoverageLoss.ready.compare_exchange_strong(expected,1)){
        firstCoverageLoss.reason=reason;firstCoverageLoss.object=reinterpret_cast<uintptr_t>(object);
        firstCoverageLoss.thread=GetCurrentThreadId();firstCoverageLoss.ready.store(2);
    }
}

// Even nonzero values are stable publications; odd values are in progress.
// Every revoke/republication advances the generation, including equal targets.
inline std::atomic<uint64_t> hooksEpoch{2};
inline std::atomic<bool> hooksComplete{false};
// R587: ordering samples only. Neither attach time nor a returned object mints
// a certificate; only CreationWitness + exact complete hook coverage may seed.
inline std::atomic<uint64_t> factoryAttachTick{0}, contextHooksCommitTick{0};
inline constexpr GUID ContextBirthKey{0xa8115587,0x6d31,0x4c4c,{0x81,0x55,0xa4,0x72,0xc2,0xc6,0x08,0x11}};
enum class ContextBirthKind:unsigned { Device, NullSwap, NestedDevice, Deferred, Deferred1, Deferred2, Deferred3, UnwitnessedReturn };
struct ContextBirthObservation {
    uint64_t entryTick=0,returnTick=0,factoryAttach=0,contextHookCommit=0;
    uintptr_t device=0,context=0,caller=0,executed=0;
    DWORD thread=0;ContextBirthKind kind=ContextBirthKind::UnwitnessedReturn;
};
// Context-owned scalar metadata, no retained resource/COM owner and no registry.
// Diagnostic storage is write-once, also for an unwitnessed/proxy return. It is
// never read by QueryHooksCover, Matches, Seed, or private-work admission.
inline HRESULT NoteContextBirth(ID3D11Device* device,ID3D11DeviceContext* context,
    ContextBirthKind kind,uint64_t entryTick,uint64_t returnTick,const void* caller,const void* executed)noexcept {
    if(!device || !context)return E_INVALIDARG;
    const auto error=GetLastError();struct RestoreError {DWORD value;~RestoreError(){SetLastError(value);}} restore{error};
    ContextBirthObservation existing{};UINT size=sizeof(existing);
    const auto found=context->GetPrivateData(ContextBirthKey,&size,&existing);
    if(found!=DXGI_ERROR_NOT_FOUND)return SUCCEEDED(found)?S_FALSE:found;
    const ContextBirthObservation row{entryTick,returnTick,factoryAttachTick.load(),contextHooksCommitTick.load(),
        reinterpret_cast<uintptr_t>(device),reinterpret_cast<uintptr_t>(context),
        reinterpret_cast<uintptr_t>(caller),reinterpret_cast<uintptr_t>(executed),GetCurrentThreadId(),kind};
    return context->SetPrivateData(ContextBirthKey,sizeof(row),&row);
}
// Hook installation lifetime is shared with the CPU binding cache. GPU query
// authority additionally retains the sticky execution-state refusal; splitting
// it must not let an existing range/history certificate survive that refusal.
inline bool InstallHookEpochCurrent(uint64_t epoch)noexcept {
    return epoch && !(epoch&1) && hooksComplete.load() && hooksEpoch.load()==epoch;
}
inline bool CurrentHookEpoch(uint64_t epoch)noexcept {
    return InstallHookEpochCurrent(epoch) &&
        !GenshinNativeQuery::detail::contextExecutionRefused.load(std::memory_order_acquire);
}
class ObjectAccess final {
    std::atomic_flag* flag_=nullptr;
public:
    explicit ObjectAccess(IUnknown* identity)noexcept {
        auto& flag=objectBusy[(reinterpret_cast<uintptr_t>(identity)>>4)%objectBusy.size()];
        if(!identity || flag.test_and_set()){LoseCoverage("object-stripe-busy-or-null",identity);return;}flag_=&flag;
    }
    ~ObjectAccess(){if(flag_)flag_->clear();}
    ObjectAccess(const ObjectAccess&)=delete;ObjectAccess& operator=(const ObjectAccess&)=delete;
    explicit operator bool()const noexcept {return flag_!=nullptr;}
};
inline std::atomic<uint64_t> nextIdentity{1};
inline uint64_t NewIdentity()noexcept {
    auto value=nextIdentity.load();
    for(unsigned attempt=0;attempt<8 && value && value!=UINT64_MAX;++attempt)
        if(nextIdentity.compare_exchange_weak(value,value+1))return value;
    return 0; // Saturates or gives up contention; never wraps/reuses a cookie.
}
// Owned by the exact context's private-data store, not a global context map.
// No context/device/query COM references are held, so there is no ownership
// cycle and no unbounded retain list. Object destruction discards its state.
class CreationWitness;
// Stack-only diagnostic state. No COM ownership, admission result cache, retry
// or callback lifetime beyond the existing factory/prepare call is introduced.
struct BirthAttempt {
    enum class Reason:unsigned {
        NotReached, Prepared, NoCreationWitness, FactoryEntered, FactorySkipped, FactoryIneligible, FactoryResult, FactoryDeviceOutput,
        CallUnauthenticated, NativeDevice, NativeImmediate, NativeReturned,
        WitnessConsumed, WitnessSource, WitnessAuthentication, WitnessThread,
        WitnessObjects, WitnessNativeDevice, WitnessNativeContext, WitnessImmediateType,
        WitnessDeviceIdentity, WitnessContextIdentity, WitnessOwnerIdentity, WitnessImmediateIdentity,
        PrepareHooksNotPublished, SeedEpochBefore, CoverageEpochBefore, CoverageNative,
        CoverageTargets, CoverageEpochAfter, SeedEpochAfterCoverage, SeedCoverageLost,
        SeedIdentity, SeedObjectAccess, SeedExistingState, SeedPrivateDataResult,
        SeedAllocation, SeedIdentityExhausted, SeedEpochBeforeStore, SeedCoverageBeforeStore,
        SeedStore, SeedEpochAfterStore, SeedCoverageAfterStore, DeferredBirthObserved, DeferredBirthUnproved, Count
    };
    Reason reason=Reason::NotReached;bool refused=false,seedEntered=false,witnessMatched=false;
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;ID3D11DeviceContext* returned=nullptr;
    HRESULT privateDataResult=S_OK,storeResult=S_OK,factoryResult=E_UNEXPECTED;UINT privateDataSize=0;
    const char* nativePredicate="none";
    uint64_t nativeGenerationBegin=GenshinNativeQuery::detail::store.generation.load(),hookEpochBegin=hooksEpoch.load();
    uint64_t entryTick=GetTickCount64(),returnTick=0,prepareTick=0,refusalTick=0;
    DWORD entryThread=GetCurrentThreadId();
    uint64_t nativeGenerationAtRefusal=0,hookEpochAtRefusal=0;
    void Reject(Reason why)noexcept {if(!refused){reason=why;refused=true;refusalTick=GetTickCount64();
        nativeGenerationAtRefusal=GenshinNativeQuery::detail::store.generation.load();hookEpochAtRefusal=hooksEpoch.load();}}
    static const char* Name(Reason reason)noexcept {
        switch(reason){
#define BIRTH_REASON(value) case Reason::value:return #value
        BIRTH_REASON(NotReached);BIRTH_REASON(Prepared);BIRTH_REASON(NoCreationWitness);
        BIRTH_REASON(FactoryEntered);BIRTH_REASON(FactorySkipped);BIRTH_REASON(FactoryIneligible);
        BIRTH_REASON(FactoryResult);BIRTH_REASON(FactoryDeviceOutput);BIRTH_REASON(CallUnauthenticated);
        BIRTH_REASON(NativeDevice);BIRTH_REASON(NativeImmediate);BIRTH_REASON(NativeReturned);
        BIRTH_REASON(WitnessConsumed);BIRTH_REASON(WitnessSource);BIRTH_REASON(WitnessAuthentication);
        BIRTH_REASON(WitnessThread);BIRTH_REASON(WitnessObjects);BIRTH_REASON(WitnessNativeDevice);
        BIRTH_REASON(WitnessNativeContext);BIRTH_REASON(WitnessImmediateType);BIRTH_REASON(WitnessDeviceIdentity);
        BIRTH_REASON(WitnessContextIdentity);BIRTH_REASON(WitnessOwnerIdentity);BIRTH_REASON(WitnessImmediateIdentity);
        BIRTH_REASON(PrepareHooksNotPublished);BIRTH_REASON(SeedEpochBefore);BIRTH_REASON(CoverageEpochBefore);
        BIRTH_REASON(CoverageNative);BIRTH_REASON(CoverageTargets);BIRTH_REASON(CoverageEpochAfter);
        BIRTH_REASON(SeedEpochAfterCoverage);BIRTH_REASON(SeedCoverageLost);BIRTH_REASON(SeedIdentity);
        BIRTH_REASON(SeedObjectAccess);BIRTH_REASON(SeedExistingState);BIRTH_REASON(SeedPrivateDataResult);
        BIRTH_REASON(SeedAllocation);BIRTH_REASON(SeedIdentityExhausted);BIRTH_REASON(SeedEpochBeforeStore);
        BIRTH_REASON(SeedCoverageBeforeStore);BIRTH_REASON(SeedStore);BIRTH_REASON(SeedEpochAfterStore);
        BIRTH_REASON(SeedCoverageAfterStore);BIRTH_REASON(DeferredBirthObserved);BIRTH_REASON(DeferredBirthUnproved);
#undef BIRTH_REASON
        default:return "Unknown";
        }
    }
};
static_assert(static_cast<unsigned>(BirthAttempt::Reason::Count)<=64);
class FactoryBirthIssuer; // R533: executes the authenticated factory itself.
class ContextState final:public IUnknown {
    std::atomic<ULONG> refs_{1};
    ContextState(uintptr_t owner,Origin origin,uint64_t epoch)noexcept:contextIdentity(owner),certificateEpoch(epoch),ranges(origin){}
    friend bool SeedCreatedContext(CreationWitness&,ID3D11Device*,ID3D11DeviceContext*)noexcept;
public:
    const uint64_t identity=NewIdentity();
    const uint64_t firstObservedTick=GetTickCount64();
    const uintptr_t contextIdentity;
    const uint64_t certificateEpoch; // Zero for UnknownPast; immutable for this COM lifetime.
    Ranges ranges; // Immutable origin; existing UnknownPast cannot be promoted.
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    std::atomic<bool> contention{false};
    explicit ContextState(uintptr_t owner)noexcept:ContextState(owner,Origin::UnknownPast,0){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!IsEqualIID(iid,IID_IUnknown) && !IsEqualIID(iid,StateKey))return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override {return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override {const auto refs=--refs_;if(!refs)delete this;return refs;}
};
// Minted once per authenticated native CreateDevice invocation, before
// handing its outputs to any game/initializer. Strong references last only
// through that callback; no global registry or replayable boolean capability.
class CreationWitness final {
    friend class FactoryBirthIssuer;
    const GenshinNativeQuery::NativeCall* source_=nullptr; // Stack lifetime encloses callback.
    BirthAttempt* observation_=nullptr; // Diagnostics only; never consulted by an admission gate.
    ComPtr<IUnknown> device_,context_;
    DWORD thread_=GetCurrentThreadId();
    bool consumed_=false;
    CreationWitness(ID3D11Device* device,ID3D11DeviceContext* context,const GenshinNativeQuery::NativeCall& source,BirthAttempt* observation=nullptr)noexcept:source_(&source),observation_(observation) {
        if(device)device->QueryInterface(IID_PPV_ARGS(&device_));
        if(context)context->QueryInterface(IID_PPV_ARGS(&context_));
    }
    bool Take(ID3D11Device* device,ID3D11DeviceContext* context)noexcept {
        const bool valid=Matches(device,context);consumed_=true;return valid;
    }
    friend bool SeedCreatedContext(CreationWitness&,ID3D11Device*,ID3D11DeviceContext*)noexcept;
public:
    CreationWitness(const CreationWitness&)=delete;CreationWitness& operator=(const CreationWitness&)=delete;
    bool Matches(ID3D11Device* device,ID3D11DeviceContext* context)const noexcept {
        using Reason=BirthAttempt::Reason;
        const auto fail=[&](Reason reason){if(observation_)observation_->Reject(reason);return false;};
        if(consumed_)return fail(Reason::WitnessConsumed);
        if(!source_)return fail(Reason::WitnessSource);
        if(!source_->Authenticated()){
            if(observation_)observation_->nativePredicate=source_->FailureObservation();
            return fail(Reason::WitnessAuthentication);
        }
        if(thread_!=GetCurrentThreadId())return fail(Reason::WitnessThread);
        if(!device_ || !context_ || !device || !context)return fail(Reason::WitnessObjects);
        if(!GenshinNativeQuery::NativeObject(device,43))return fail(Reason::WitnessNativeDevice);
        if(!GenshinNativeQuery::NativeContext(context,115))return fail(Reason::WitnessNativeContext);
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail(Reason::WitnessImmediateType);
        ComPtr<IUnknown> d,c,ownerId,immediateId;ComPtr<ID3D11Device> owner;ComPtr<ID3D11DeviceContext> immediate;
        device->QueryInterface(IID_PPV_ARGS(&d));context->QueryInterface(IID_PPV_ARGS(&c));
        context->GetDevice(&owner);device->GetImmediateContext(&immediate);
        if(owner)owner->QueryInterface(IID_PPV_ARGS(&ownerId));
        if(immediate)immediate->QueryInterface(IID_PPV_ARGS(&immediateId));
        if(d.Get()!=device_.Get())return fail(Reason::WitnessDeviceIdentity);
        if(c.Get()!=context_.Get())return fail(Reason::WitnessContextIdentity);
        if(ownerId.Get()!=device_.Get())return fail(Reason::WitnessOwnerIdentity);
        if(immediateId.Get()!=context_.Get())return fail(Reason::WitnessImmediateIdentity);
        if(observation_)observation_->witnessMatched=true;
        return true;
    }
    void Cancel()noexcept {consumed_=true;}
};
// Shared product issuer, also used without substitution by the controlled
// non-game harness. Invoke is the only mint path and directly calls the exact
// attested native trampoline once. A caller-selected adapter and optional
// nested native swap factory require their own exact receipts. Failed or
// overlay creations do not consume a global claim. HRESULT/identity/absence alone never mint.
class FactoryBirthIssuer final {
public:
    template<class Factory,class Capture> static HRESULT Invoke(Factory executed,
        IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,
        UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* feature,ID3D11DeviceContext** returnedContext,
        Capture&& capture,BirthAttempt* observation=nullptr) {
        using Reason=BirthAttempt::Reason;
        const auto refuse=[&](Reason reason){if(observation)observation->Reject(reason);};
        GenshinNativeQuery::NativeCall call(executed);
        const auto result=call.Run(adapter,type,software,flags,levels,count,sdk,device,feature,returnedContext);
        if(observation){observation->returnTick=GetTickCount64();observation->factoryResult=result;}
        if(observation)observation->nativePredicate=call.FailureObservation();
        if(result!=S_OK){refuse(Reason::FactoryResult);return result;}
        if(!device || !*device){refuse(Reason::FactoryDeviceOutput);return result;}
        if(observation){observation->device=*device;observation->returned=returnedContext?*returnedContext:nullptr;}
        if(!call.Authenticated()){
            if(observation)observation->nativePredicate=call.FailureObservation();
            refuse(Reason::CallUnauthenticated);
        }
        else if(!GenshinNativeQuery::NativeObject(*device,43))refuse(Reason::NativeDevice);
        else {
            ComPtr<ID3D11DeviceContext> immediate;(*device)->GetImmediateContext(&immediate);
            auto* returned=returnedContext?*returnedContext:immediate.Get();
            if(observation){observation->context=immediate.Get();observation->returned=returned;}
            if(!GenshinNativeQuery::NativeContext(immediate.Get(),115))refuse(Reason::NativeImmediate);
            else if(!GenshinNativeQuery::NativeContext(returned,115))refuse(Reason::NativeReturned);
            else {
                CreationWitness birth(*device,immediate.Get(),call,observation);
                if(birth.Matches(*device,returned)){
                    if(call.Authenticated()){capture(*device,&birth);birth.Cancel();return result;}
                    if(observation)observation->nativePredicate=call.FailureObservation();
                    refuse(Reason::CallUnauthenticated);
                }
            }
        }
        capture(*device,nullptr);return result;
    }
};
inline ComPtr<ContextState> State(ID3D11DeviceContext* context)noexcept {
    ComPtr<ContextState> state;if(!context)return state;
    ComPtr<IUnknown> identity;if(FAILED(context->QueryInterface(IID_PPV_ARGS(&identity))))return state;
    ObjectAccess object(identity.Get());if(!object)return state;
    IUnknown* stored=nullptr;UINT size=sizeof(stored);
    const auto found=context->GetPrivateData(StateKey,&size,&stored);
    if(SUCCEEDED(found)){
        ComPtr<IUnknown> held;held.Attach(stored);
        if(size!=sizeof(stored) || !stored || FAILED(stored->QueryInterface(StateKey,reinterpret_cast<void**>(state.GetAddressOf()))))return {};
        if(state->contextIdentity!=reinterpret_cast<uintptr_t>(identity.Get()))return {};
        return state;
    }
    if(found!=DXGI_ERROR_NOT_FOUND)return state;
    state.Attach(new(std::nothrow) ContextState(reinterpret_cast<uintptr_t>(identity.Get())));
    if(!state || !state->identity || FAILED(context->SetPrivateDataInterface(StateKey,state.Get())))return {};
    return state;
}
// A try-only per-context lock: no global UI lock or CPU wait. D3D11 requires
// serial use of each immediate context; overlapping observation is sticky
// unknown rather than an assumption that either update won the race.
class Access final {
    ComPtr<ContextState> state_;
    bool locked_=false;
public:
    explicit Access(ID3D11DeviceContext* context)noexcept:state_(State(context)) {
        if(state_){locked_=!state_->busy.test_and_set();if(!locked_)state_->contention=true;}
    }
    ~Access(){if(locked_)state_->busy.clear();}
    Access(const Access&)=delete;Access& operator=(const Access&)=delete;
    ContextState* Get()const noexcept {return locked_?state_.Get():nullptr;}
};
// State objects retain their own device, so this non-owning canonical identity
// cannot outlive/rebind its owner. No global state-object registry is needed.
inline void NoteD3D11State(ID3DDeviceContextState* state)noexcept {
    if(!state)return;
    ComPtr<ID3D11Device> device;ComPtr<IUnknown> identity;state->GetDevice(&device);
    if(!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&identity)))){LoseCoverage("state-owner-identity",state);return;}
    const auto owner=reinterpret_cast<uintptr_t>(identity.Get());
    if(FAILED(state->SetPrivateData(StateModeKey,sizeof(owner),&owner)))LoseCoverage("state-owner-private-data",state);
}
inline bool D3D11State(ID3D11DeviceContext* context,ID3DDeviceContextState* state)noexcept {
    if(!context || !state)return false;
    uintptr_t owner=0;UINT size=sizeof(owner);ComPtr<ID3D11Device> device;ComPtr<IUnknown> identity;
    context->GetDevice(&device);
    return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&identity))) &&
        SUCCEEDED(state->GetPrivateData(StateModeKey,&size,&owner)) && size==sizeof(owner) &&
        owner==reinterpret_cast<uintptr_t>(identity.Get());
}
class StateSwap final {
    Access access_;
    bool witnessed_=false;
public:
    StateSwap(ID3D11DeviceContext* context,ID3DDeviceContextState* state)noexcept:access_(context) {
        if(!state)return; // Documented no-op, never a quiet/reset witness.
        auto* current=access_.Get();if(!current){LoseCoverage("state-swap-no-access",context);return;}
        if(!D3D11State(context,state)){current->ranges.Invalidate("state-swap-unproved-mode");return;}
        // Asynchronous objects remain active across this API. Preserve the
        // entire range table. Unknown history is NEVER promoted by a swap.
        witnessed_=CurrentHookEpoch(current->certificateEpoch) && current->ranges.KnownHistory() &&
            !current->contention.load() && !coverageLost.load();
    }
    void Returned(ID3DDeviceContextState* previous)const noexcept {
        // A fresh D3D11 context's first previous state is its D3D11 default.
        // Later previous states are certified only along this unbroken chain.
        if(witnessed_ && previous && CurrentHookEpoch(access_.Get()->certificateEpoch) &&
            !coverageLost.load())NoteD3D11State(previous);
    }
};
enum class Kind { Unknown, Range, EndOnly };
inline Kind QueryKind(ID3D11Asynchronous* async,unsigned* descriptor=nullptr)noexcept {
    if(descriptor)*descriptor=UINT32_MAX;
    if(!async)return Kind::Unknown;
    ComPtr<ID3D11Query> query;
    if(SUCCEEDED(async->QueryInterface(IID_PPV_ARGS(&query)))){
        D3D11_QUERY_DESC desc{};query->GetDesc(&desc);if(descriptor)*descriptor=static_cast<unsigned>(desc.Query);
        switch(desc.Query){
        case D3D11_QUERY_EVENT:case D3D11_QUERY_TIMESTAMP:return Kind::EndOnly;
        case D3D11_QUERY_OCCLUSION:case D3D11_QUERY_TIMESTAMP_DISJOINT:case D3D11_QUERY_PIPELINE_STATISTICS:
        case D3D11_QUERY_OCCLUSION_PREDICATE:case D3D11_QUERY_SO_STATISTICS:case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
        case D3D11_QUERY_SO_STATISTICS_STREAM0:case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
        case D3D11_QUERY_SO_STATISTICS_STREAM1:case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
        case D3D11_QUERY_SO_STATISTICS_STREAM2:case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
        case D3D11_QUERY_SO_STATISTICS_STREAM3:case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:return Kind::Range;
        default:return Kind::Unknown;}
    }
    ComPtr<ID3D11Counter> counter;
    return SUCCEEDED(async->QueryInterface(IID_PPV_ARGS(&counter)))?Kind::Range:Kind::Unknown;
}
struct AsyncStamp {uint64_t identity=0,owner=0;bool open=false,invalid=false;};
// The stamp lives on the actual asynchronous object (QI aliases share it).
// It contains no COM retain. A destroyed-and-reallocated object gets a new
// cookie. Cross-context overlap poisons the object; the original owner still
// has an active entry and its later End cannot erroneously certify quiet.
inline void Observe(ID3D11DeviceContext* context,ID3D11Asynchronous* async,bool begin,uintptr_t caller=0)noexcept {
    unsigned descriptor=UINT32_MAX;const auto kind=QueryKind(async,&descriptor);
    if(kind==Kind::EndOnly && !begin)return; // Includes private mapping EVENTs; never fake a range End.
    Access access(context);auto* state=access.Get();if(!state){LoseCoverage("query-observe-no-access",context);return;}
    if(kind!=Kind::Range){state->ranges.Invalidate("query-kind-not-range");return;}
    ComPtr<IUnknown> identity;
    if(FAILED(async->QueryInterface(IID_PPV_ARGS(&identity)))){state->ranges.Invalidate("query-private-data-or-identity");LoseCoverage("query-private-data-or-identity",context);return;}
    ObjectAccess object(identity.Get());if(!object){state->ranges.Invalidate();return;}
    AsyncStamp stamp{};UINT size=sizeof(stamp);
    const auto found=async->GetPrivateData(AsyncKey,&size,&stamp);
    if(found==DXGI_ERROR_NOT_FOUND){stamp.identity=NewIdentity();}
    else if(FAILED(found) || size!=sizeof(stamp)){state->ranges.Invalidate("query-private-data-or-identity");LoseCoverage("query-private-data-or-identity",context);return;}
    if(!stamp.identity || stamp.invalid){state->ranges.Invalidate("query-cookie-invalid");return;}
    if(begin){
        if(stamp.open){stamp.invalid=true;state->ranges.Invalidate("query-already-open");}
        else{stamp.open=true;stamp.owner=state->identity;state->ranges.Begin(stamp.identity,descriptor,caller);}
    }else{
        if(!stamp.open || stamp.owner!=state->identity){stamp.invalid=true;state->ranges.Invalidate("query-end-owner-or-open");}
        else{state->ranges.End(stamp.identity);stamp.open=false;}
    }
    if(FAILED(async->SetPrivateData(AsyncKey,sizeof(stamp),&stamp))){state->ranges.Invalidate("query-stamp-write");LoseCoverage("query-stamp-write",context);}
}
// Alternate native wrappers can forward to the already hooked direct entry.
// Consume that one exact transfer once; all mismatches permanently lose coverage.
class RangeForward;
inline thread_local std::array<RangeForward*,2> rangeForwards{};
class RangeForward final {
    ID3D11DeviceContext* context_;ID3D11Asynchronous* async_;uintptr_t caller_;
    unsigned index_;RangeForward* previous_;bool consumed_=false;
public:
    RangeForward(ID3D11DeviceContext* context,ID3D11Asynchronous* async,bool begin,uintptr_t caller)noexcept:
        context_(context),async_(async),caller_(caller),index_(begin?0:1),previous_(rangeForwards[index_]){
        if(previous_ || rangeForwards[1-index_])LoseCoverage("native-range-forward-nested",context);
        rangeForwards[index_]=this;
    }
    ~RangeForward(){rangeForwards[index_]=previous_;if(!consumed_)LoseCoverage("native-range-forward-not-observed",context_);}
    bool Claim(ID3D11DeviceContext* context,ID3D11Asynchronous* async,uintptr_t caller)noexcept {
        if(consumed_ || !caller_ || caller!=caller_ || context!=context_ || async!=async_){
            LoseCoverage("native-range-forward-mismatch",context);return false;}
        consumed_=true;return true;
    }
    RangeForward(const RangeForward&)=delete;RangeForward& operator=(const RangeForward&)=delete;
};
inline bool ClaimRangeForward(ID3D11DeviceContext* context,ID3D11Asynchronous* async,bool begin,uintptr_t caller)noexcept {
    const unsigned index=begin?0:1;auto* forward=rangeForwards[index];
    if(!forward && rangeForwards[1-index])LoseCoverage("native-range-forward-operation",context);
    return forward && forward->Claim(context,async,caller);
}
inline std::atomic<void*> hookedBegin{nullptr},hookedEnd{nullptr},hookedExecute{nullptr},hookedFinish{nullptr},hookedSwap{nullptr};
inline std::atomic<void*> hookedCreateState{nullptr};
inline std::atomic<void*> hookedClearState{nullptr};
inline constexpr std::array<unsigned,5> stateSwapSlots{27,28,58,110,114};
inline std::array<std::atomic<void*>,5> hookedStateSwap{};
inline bool ContextEntryCovered(void* entry,unsigned slot,void* direct)noexcept {
    if(entry && entry==direct)return true;
    for(unsigned i=0;i<stateSwapSlots.size();++i)
        if(stateSwapSlots[i]==slot && entry && entry==hookedStateSwap[i].load())return true;
    return false;
}
template<class Interface> inline bool ContextAliasCovered(ID3D11DeviceContext* context)noexcept {
    ComPtr<Interface> alias;const auto result=context->QueryInterface(IID_PPV_ARGS(&alias));
    if(result==E_NOINTERFACE)return true;
    if(FAILED(result) || !alias || !GenshinNativeQuery::NativeContext(alias.Get(),132))return false;
    auto** table=*reinterpret_cast<void***>(alias.Get());
    return ContextEntryCovered(table[27],27,hookedBegin.load()) && ContextEntryCovered(table[28],28,hookedEnd.load()) &&
        ContextEntryCovered(table[58],58,hookedExecute.load()) && ContextEntryCovered(table[114],114,hookedFinish.load()) &&
        ContextEntryCovered(table[110],110,hookedClearState.load()) && table[131]==hookedSwap.load();
}
template<class Interface> inline bool DeviceAliasCovered(ID3D11Device* device)noexcept {
    ComPtr<Interface> alias;const auto result=device->QueryInterface(IID_PPV_ARGS(&alias));
    if(result==E_NOINTERFACE)return true;
    return SUCCEEDED(result) && alias && GenshinNativeQuery::NativeObject(alias.Get(),48) &&
        (*reinterpret_cast<void***>(alias.Get()))[47]==hookedCreateState.load();
}
inline bool HookTargetsMatch(ID3D11DeviceContext* context)noexcept {
    if(!context || !hooksComplete.load() || !GenshinNativeQuery::NativeContext(context,115) ||
       context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    auto** table=*reinterpret_cast<void***>(context);
    if(!ContextEntryCovered(table[27],27,hookedBegin.load()) || !ContextEntryCovered(table[28],28,hookedEnd.load()) ||
       !ContextEntryCovered(table[58],58,hookedExecute.load()) || !ContextEntryCovered(table[114],114,hookedFinish.load()) ||
       !ContextEntryCovered(table[110],110,hookedClearState.load()))return false;
    ComPtr<ID3D11DeviceContext1> context1;
    if(FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))) || !GenshinNativeQuery::NativeContext(context1.Get(),132))return false;
    auto** table1=*reinterpret_cast<void***>(context1.Get());
    ComPtr<ID3D11Device> device;ComPtr<ID3D11Device1> device1;context->GetDevice(&device);
    if(!device || !GenshinNativeQuery::NativeObject(device.Get(),43) ||
       FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || !GenshinNativeQuery::NativeObject(device1.Get(),48) ||
       (*reinterpret_cast<void***>(device1.Get()))[47]!=hookedCreateState.load())return false;
    // Every required executed original is a validated, simple Detours path;
    // matching interface pointers alone cannot authenticate inline transfers.
    for(auto slot:{27u,28u,58u,110u,114u})if(!GenshinNativeQuery::Required(table[slot]))return false;
    if(!GenshinNativeQuery::Required(table1[131]) ||
       !GenshinNativeQuery::Required((*reinterpret_cast<void***>(device1.Get()))[47]))return false;
    return table1[27]==table[27] && table1[28]==table[28] && table1[58]==table[58] &&
        table1[114]==table[114] && table1[131]==hookedSwap.load() &&
        ContextAliasCovered<ID3D11DeviceContext2>(context) && ContextAliasCovered<ID3D11DeviceContext3>(context) &&
        ContextAliasCovered<ID3D11DeviceContext4>(context) && DeviceAliasCovered<ID3D11Device2>(device.Get()) &&
        DeviceAliasCovered<ID3D11Device3>(device.Get()) && DeviceAliasCovered<ID3D11Device4>(device.Get()) &&
        DeviceAliasCovered<ID3D11Device5>(device.Get());
}
// Observation of a coverage mismatch loses optional mapping coverage for the
// process lifetime. Restored pointers cannot erase potentially unseen queries
// or state transitions. This also covers callers other than PrivateWork.
struct CoverageTiming {uint64_t nativeTicks=0,targetTicks=0;};
template<class Check> inline bool TimedCoverage(Check&& check,uint64_t* ticks)noexcept {
    if(!ticks)return check();
    LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
    const bool result=check();QueryPerformanceCounter(&end);
    if(begin.QuadPart && end.QuadPart>=begin.QuadPart)*ticks=static_cast<uint64_t>(end.QuadPart-begin.QuadPart);
    return result;
}
inline bool QueryHooksCover(ID3D11DeviceContext* context,CoverageTiming* timing=nullptr,BirthAttempt* observation=nullptr)noexcept {
    using Reason=BirthAttempt::Reason;
    const auto fail=[&](Reason reason){if(observation)observation->Reject(reason);LoseCoverage("query-hooks-or-native-epoch",context);return false;};
    const auto epoch=hooksEpoch.load();
    if(!CurrentHookEpoch(epoch))return fail(Reason::CoverageEpochBefore);
    if(!TimedCoverage([](){return GenshinNativeQuery::Intact();},timing?&timing->nativeTicks:nullptr))return fail(Reason::CoverageNative);
    if(!TimedCoverage([&](){return HookTargetsMatch(context);},timing?&timing->targetTicks:nullptr))return fail(Reason::CoverageTargets);
    if(!CurrentHookEpoch(epoch))return fail(Reason::CoverageEpochAfter);
    return true;
}
class HookPublication final {
    uint64_t prior_=0;
public:
    HookPublication()noexcept {
        hooksComplete=false;
        auto epoch=hooksEpoch.load();
        // One bounded CAS, no waiting; exhaustion/contention is sticky loss.
        if(!epoch || (epoch&1) || epoch>UINT64_MAX-2 ||
           !hooksEpoch.compare_exchange_strong(epoch,epoch+1)){LoseCoverage("hook-publication-contention");return;}
        prior_=epoch;
    }
    ~HookPublication(){if(prior_)hooksEpoch=prior_+2;}
    HookPublication(const HookPublication&)=delete;
    HookPublication& operator=(const HookPublication&)=delete;
    explicit operator bool()const noexcept {return prior_!=0;}
};
inline void MappingHooksIncomplete()noexcept {HookPublication publication;}
inline void HooksCommitted(void* begin,void* end,void* execute,void* finish,void* swap,void* createState,
    const std::array<void*,5>* stateSwap=nullptr,void* clearState=nullptr)noexcept {
    HookPublication publication;if(!publication)return;
    hookedBegin=begin;hookedEnd=end;hookedExecute=execute;hookedFinish=finish;hookedSwap=swap;
    hookedCreateState=createState;
    hookedClearState=clearState;
    for(unsigned i=0;i<hookedStateSwap.size();++i)hookedStateSwap[i]=stateSwap?(*stateSwap)[i]:nullptr;
    hooksComplete=begin && end && execute && finish && swap && createState;
    if(hooksComplete.load())contextHooksCommitTick.store(GetTickCount64());
}
inline bool SeedCreatedContext(CreationWitness& birth,ID3D11Device* device,ID3D11DeviceContext* context)noexcept {
    using Reason=BirthAttempt::Reason;auto* observation=birth.observation_;
    const auto fail=[&](Reason reason){if(observation)observation->Reject(reason);return false;};
    if(observation){observation->seedEntered=true;observation->device=device;observation->context=context;}
    const auto epoch=hooksEpoch.load();
    if(!birth.Take(device,context))return false;
    if(!CurrentHookEpoch(epoch))return fail(Reason::SeedEpochBefore);
    if(!QueryHooksCover(context,nullptr,observation))return false;
    if(!CurrentHookEpoch(epoch))return fail(Reason::SeedEpochAfterCoverage);
    if(coverageLost.load())return fail(Reason::SeedCoverageLost);
    ComPtr<IUnknown> identity;if(FAILED(context->QueryInterface(IID_PPV_ARGS(&identity))))return fail(Reason::SeedIdentity);
    ObjectAccess object(identity.Get());if(!object)return fail(Reason::SeedObjectAccess);
    IUnknown* existing=nullptr;UINT size=sizeof(existing);
    const auto found=context->GetPrivateData(StateKey,&size,&existing);
    if(observation){observation->privateDataResult=found;observation->privateDataSize=size;}
    if(SUCCEEDED(found)){if(size==sizeof(existing) && existing)existing->Release();return fail(Reason::SeedExistingState);}
    if(found!=DXGI_ERROR_NOT_FOUND)return fail(Reason::SeedPrivateDataResult); // Never replace UnknownPast.
    ComPtr<ContextState> fresh;fresh.Attach(new(std::nothrow) ContextState(reinterpret_cast<uintptr_t>(identity.Get()),Origin::FreshBeforeExposure,epoch));
    if(!fresh)return fail(Reason::SeedAllocation);
    if(!fresh->identity)return fail(Reason::SeedIdentityExhausted);
    if(!CurrentHookEpoch(epoch))return fail(Reason::SeedEpochBeforeStore);
    if(coverageLost.load())return fail(Reason::SeedCoverageBeforeStore);
    const auto stored=context->SetPrivateDataInterface(StateKey,fresh.Get());
    if(observation)observation->storeResult=stored;
    if(FAILED(stored))return fail(Reason::SeedStore);
    if(!CurrentHookEpoch(epoch)){fresh->ranges.Invalidate();return fail(Reason::SeedEpochAfterStore);}
    if(coverageLost.load()){fresh->ranges.Invalidate();return fail(Reason::SeedCoverageAfterStore);}
    if(observation && !observation->refused)observation->reason=Reason::Prepared;
    return true;
}
enum class RefusalKind:unsigned { NoState, HooksOrEpoch, Coverage, Contention, History, Active, Count };
struct WorkRefusalReceipt {
    std::atomic<unsigned> ready{0};uintptr_t context=0,activeFirstCaller=0;DWORD thread=0;
    uint64_t certificate=0,epoch=0,activeTypes=0;unsigned active=0;
    uint64_t refusalTick=0,firstObservedTick=0,attachTick=0,hookCommitTick=0;
    ContextBirthObservation birth{};bool birthObserved=false;
    bool complete=false,history=false,coverage=false,hooks=false;
    const char* rangeReason="none";const char* nativeReason="none";
};
inline std::array<WorkRefusalReceipt,static_cast<unsigned>(RefusalKind::Count)> workRefusals{};
inline void ObserveRefusal(RefusalKind kind,ID3D11DeviceContext* context,const ContextState* state)noexcept {
    auto& receipt=workRefusals[static_cast<unsigned>(kind)];unsigned expected=0;
    if(!receipt.ready.compare_exchange_strong(expected,1))return;
    receipt.context=reinterpret_cast<uintptr_t>(context);receipt.thread=GetCurrentThreadId();
    receipt.refusalTick=GetTickCount64();receipt.attachTick=factoryAttachTick.load();
    receipt.hookCommitTick=contextHooksCommitTick.load();
    if(context){UINT size=sizeof(receipt.birth);
        receipt.birthObserved=SUCCEEDED(context->GetPrivateData(ContextBirthKey,&size,&receipt.birth)) && size==sizeof(receipt.birth);}
    if(state)receipt.firstObservedTick=state->firstObservedTick;
    receipt.epoch=hooksEpoch.load();receipt.coverage=coverageLost.load();receipt.hooks=hooksComplete.load();
    receipt.nativeReason=GenshinNativeQuery::Failure();
    if(state){receipt.certificate=state->certificateEpoch;receipt.active=state->ranges.Active();receipt.activeTypes=state->ranges.ActiveTypes();receipt.activeFirstCaller=state->ranges.FirstCaller();
        receipt.complete=state->ranges.Complete();receipt.history=state->ranges.KnownHistory();
        receipt.rangeReason=state->ranges.InvalidReason();}
    receipt.ready.store(2);
}
// R800: observations of synchronous validation duration, never GPU timing or admission.
struct WorkCost {
    std::atomic<uint64_t> probes{0},earlyRefused{0},validationCalls{0},validationTicks{0},nativeTicks{0},targetTicks{0};
    // Fixed 1 us bins plus overflow; no allocation, formatting or file I/O in admission.
    std::array<std::atomic<uint32_t>,2049> validationLatencyUs{},nativeLatencyUs{};
};
inline WorkCost workCost;
inline uint32_t LatencyBin(uint64_t ticks)noexcept {
    static const uint64_t frequency=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return f.QuadPart>0?uint64_t(f.QuadPart):1ull;}();
    const auto us=ticks>UINT64_MAX/1000000?UINT64_MAX:ticks*1000000/frequency;
    return static_cast<uint32_t>(us>2048?2048:us);
}
struct LatencySummary {uint64_t samples=0,overflow=0;double p50Us=0,p95Us=0,p99Us=0;};
inline LatencySummary ReadLatency(const std::array<std::atomic<uint32_t>,2049>& bins)noexcept {
    LatencySummary s;for(const auto& bin:bins)s.samples+=bin.load(std::memory_order_relaxed);
    if(!s.samples)return s;const uint64_t targets[]{(s.samples+1)/2,(s.samples*95+99)/100,(s.samples*99+99)/100};
    uint64_t seen=0;unsigned found=0;double* percentiles[]{&s.p50Us,&s.p95Us,&s.p99Us};
    for(unsigned i=0;i<bins.size();++i){seen+=bins[i].load(std::memory_order_relaxed);if(i==2048)s.overflow=bins[i].load(std::memory_order_relaxed);
        while(found<3&&seen>=targets[found]){*percentiles[found]=i==2048?2048.0:i+.5;++found;}}
    return s;
}
class PrivateWork final {
    Access access_;
    bool allowed_=false;
public:
    explicit PrivateWork(ID3D11DeviceContext* context)noexcept:access_(context) {
        workCost.probes.fetch_add(1,std::memory_order_relaxed);
        auto* state=access_.Get();
        const auto refuseEarly=[&](RefusalKind kind){workCost.earlyRefused.fetch_add(1,std::memory_order_relaxed);ObserveRefusal(kind,context,state);};
        if(!state){LoseCoverage("private-work-no-access",context);refuseEarly(RefusalKind::NoState);return;}
        if(!CurrentHookEpoch(state->certificateEpoch)){
            state->ranges.Invalidate("private-work-hooks-or-epoch");refuseEarly(RefusalKind::HooksOrEpoch);return;
        }
        // A known refusal submits no private work and grants no certificate.
        // Avoid expensive native code/import/interface checks for that rejected
        // request. Every potentially permitted request still takes the complete
        // original QueryHooksCover + epoch + coverage/history/range checks below.
        if(coverageLost.load() || state->contention.load() || !state->ranges.KnownQuiescent()){
            refuseEarly(coverageLost.load()?RefusalKind::Coverage:state->contention.load()?RefusalKind::Contention:
                !state->ranges.KnownHistory()?RefusalKind::History:RefusalKind::Active);return;
        }
        LARGE_INTEGER started{},finished{};QueryPerformanceCounter(&started);
        workCost.validationCalls.fetch_add(1,std::memory_order_relaxed);
        CoverageTiming timing{};
        const bool covered=QueryHooksCover(context,&timing) && CurrentHookEpoch(state->certificateEpoch);
        QueryPerformanceCounter(&finished);
        workCost.nativeTicks.fetch_add(timing.nativeTicks,std::memory_order_relaxed);
        workCost.targetTicks.fetch_add(timing.targetTicks,std::memory_order_relaxed);
        workCost.nativeLatencyUs[LatencyBin(timing.nativeTicks)].fetch_add(1,std::memory_order_relaxed);
        if(started.QuadPart && finished.QuadPart>=started.QuadPart){
            const auto elapsed=static_cast<uint64_t>(finished.QuadPart-started.QuadPart);
            workCost.validationTicks.fetch_add(elapsed,std::memory_order_relaxed);
            workCost.validationLatencyUs[LatencyBin(elapsed)].fetch_add(1,std::memory_order_relaxed);
        }
        if(!covered){
            state->ranges.Invalidate("private-work-hooks-or-epoch");ObserveRefusal(RefusalKind::HooksOrEpoch,context,state);return;
        }
        allowed_=!coverageLost.load() && !state->contention.load() && state->ranges.KnownQuiescent();
        if(!allowed_)ObserveRefusal(coverageLost.load()?RefusalKind::Coverage:
            state->contention.load()?RefusalKind::Contention:
            !state->ranges.KnownHistory()?RefusalKind::History:RefusalKind::Active,context,state);
    }
    explicit operator bool()const noexcept {return allowed_;}
};
inline bool KnownQuiescent(ID3D11DeviceContext* context)noexcept {
    return static_cast<bool>(PrivateWork(context));
}
inline void OpaqueBoundary(ID3D11DeviceContext* context)noexcept {
    Access access(context);if(auto* state=access.Get())state->ranges.Invalidate();else LoseCoverage("opaque-no-access",context);
}
}
