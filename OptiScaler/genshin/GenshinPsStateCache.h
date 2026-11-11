// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFamilySkip.h"
#include "GenshinNativeQueryProvenance.h"
#include "GenshinPublicPsBinding.h"
#include <wrl/client.h>
#include <memory>
#include <atomic>
#include <array>
#include <cstring>
#include <limits>

#include <mutex>

namespace GenshinLegacy { void OnPresent(ID3D11Device*) noexcept; }

// R864: CPU-only, getter-backed shadow of the actual PS binding. Public COM
// method identities define mutation coverage; private GPU recipes grant nothing
// here. Family metadata and GPU/query/resource admission remain independent.
namespace GenshinLegacy::PsState {
using Microsoft::WRL::ComPtr;
using Endpoint=PublicPsBinding::Endpoint;
namespace Metrics=Family::ShaderCacheMetrics;
using PublicationResult=Metrics::Publication;
inline bool Ready(PublicationResult r) noexcept {return r==PublicationResult::ReusedCoverage || r==PublicationResult::ReplacedCoverage;}
inline constexpr auto Slots=PublicPsBinding::Slots;
struct Coverage {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11Device> device;
    PublicPsBinding::Dispatch dispatch{};
    std::array<Endpoint,PublicPsBinding::MethodCount> endpoints{}; // prepared current vector
    std::array<std::array<Endpoint,PublicPsBinding::Capacity>,PublicPsBinding::MethodCount> variants{};
    std::array<unsigned,PublicPsBinding::MethodCount> variantCounts{};
    bool publicPsMutationCovered=false; // all five public mutation roles committed
    // One optional-cache lifetime per process. All replacement publications share
    // terminal refusal: a late foreign mutation/old receipt can never be laundered
    // through a fresh Coverage. No reset, no extra draw load or GPU authority.
    inline static std::atomic<bool> refused{false};
};
inline std::atomic<std::shared_ptr<const Coverage>> coverage;
inline std::atomic<uint64_t> publicationSerial{0}; // diagnostic only, actual exchanges
inline void NotePublicationExchange() noexcept {
    auto n=publicationSerial.load(std::memory_order_relaxed);
    while(n!=UINT64_MAX && !publicationSerial.compare_exchange_weak(n,n+1,std::memory_order_relaxed)){}
}
inline thread_local const char* publicationFailure="none";

// R206 installation control only. Never read by Mutation/TryCurrent/ProofCurrent.
// A request keeps this exact COM lifetime; no worker, timer or draw poll owns it.
enum class OptionalInstallPhase : unsigned {Idle,Attempting,PendingPublication,DeferredAttempt,Ready,Terminal,AwaitingDeviceHooks};
struct OptionalInstallRequest final {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device> device;
    std::atomic<bool> cancelled{false};
    const void* owner=nullptr;
    uint64_t ownerEpoch=0,deviceEpoch=0,controlsEpoch=0;
    DWORD threadId=0;
    HANDLE thread=nullptr;
    ~OptionalInstallRequest(){if(thread)CloseHandle(thread);}
};
// Only the real OnPresent owner, while retaining nativeMutex, can mint this
// synchronous, noncopyable capability. A Status snapshot cannot construct it.
class InstallOwnerScope final {
    friend void GenshinLegacy::OnPresent(ID3D11Device*) noexcept;
    inline static thread_local const InstallOwnerScope* active_=nullptr;
    const InstallOwnerScope* previous_=nullptr;
    const void* owner_=nullptr;
    const void* bindingOwner_=nullptr; // shaderCreationOwner, NOT nativePipeline
    ID3D11DeviceContext* context_=nullptr;
    ID3D11Device* device_=nullptr;
    uint64_t ownerEpoch_=0,deviceEpoch_=0,controlsEpoch_=0;
    DWORD thread_=0;
    bool (*controlsCurrent_)(uint64_t) noexcept=nullptr;
    InstallOwnerScope(const void* owner,const void* bindingOwner,ID3D11DeviceContext* context,ID3D11Device* device,
        uint64_t ownerEpoch,uint64_t deviceEpoch,uint64_t controlsEpoch,bool (*controlsCurrent)(uint64_t) noexcept) noexcept
        :previous_(active_),owner_(owner),bindingOwner_(bindingOwner),context_(context),device_(device),ownerEpoch_(ownerEpoch),deviceEpoch_(deviceEpoch),
         controlsEpoch_(controlsEpoch),thread_(GetCurrentThreadId()),controlsCurrent_(controlsCurrent){active_=this;}
public:
    ~InstallOwnerScope(){active_=previous_;}
    InstallOwnerScope(const InstallOwnerScope&)=delete;
    InstallOwnerScope& operator=(const InstallOwnerScope&)=delete;
    bool Current()const noexcept {
        return active_==this && !previous_ && owner_ && bindingOwner_ && ownerEpoch_ && context_ && device_ && deviceEpoch_ && controlsEpoch_ &&
            thread_==GetCurrentThreadId() && controlsCurrent_ && controlsCurrent_(controlsEpoch_);
    }
    const void* Owner()const noexcept{return owner_;}
    const void* BindingOwner()const noexcept{return bindingOwner_;}
    uint64_t OwnerEpoch()const noexcept{return ownerEpoch_;}
    ID3D11DeviceContext* Context()const noexcept{return context_;}
    ID3D11Device* Device()const noexcept{return device_;}
    uint64_t DeviceEpoch()const noexcept{return deviceEpoch_;}
    uint64_t ControlsEpoch()const noexcept{return controlsEpoch_;}
};
using OptionalOwnerConsumer=void(*)(const InstallOwnerScope&) noexcept;
inline std::atomic<OptionalOwnerConsumer> optionalOwnerConsumer{nullptr};
class OptionalInstallControl final {
    // Short metadata/publication sections only: never hold over COM, native
    // validation, the installer transaction, logging, or an external callback.
    std::mutex mutex_;
    std::atomic<OptionalInstallPhase> phase_{OptionalInstallPhase::Idle};
    std::shared_ptr<OptionalInstallRequest> active_;
public:
    OptionalInstallPhase Phase()const noexcept{return phase_.load(std::memory_order_acquire);}
    std::shared_ptr<OptionalInstallRequest> Begin(ID3D11DeviceContext* context,ID3D11Device* device) noexcept {
        if(Phase()!=OptionalInstallPhase::Idle)return {};
        try {
            auto request=std::make_shared<OptionalInstallRequest>();
            request->context=context;request->device=device;
            std::lock_guard lock(mutex_);
            if(Phase()!=OptionalInstallPhase::Idle)return {};
            active_=request;phase_.store(OptionalInstallPhase::AwaitingDeviceHooks,std::memory_order_release);return request;
        }catch(...){Cancel();return {};}
    }
    // Exactly one same-device tail schedules the first real Present owner.
    // No CPU binding proof is published during device-hook installation.
    std::shared_ptr<OptionalInstallRequest> ClaimInitial(ID3D11Device* device) noexcept {
        std::lock_guard lock(mutex_);
        if(Phase()!=OptionalInstallPhase::AwaitingDeviceHooks || !active_ ||
           active_->device.Get()!=device || active_->cancelled.load())return {};
        phase_.store(OptionalInstallPhase::Attempting,std::memory_order_release);
        return active_;
    }
    std::shared_ptr<OptionalInstallRequest> Pending() noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return {};
        if(Phase()==OptionalInstallPhase::Ready && PublicPsBinding::RefreshRequested())
            phase_.store(OptionalInstallPhase::PendingPublication,std::memory_order_release);
        if(Phase()!=OptionalInstallPhase::PendingPublication)return {};
        return active_;
    }
    bool ClaimDeferred(const std::shared_ptr<OptionalInstallRequest>& request) noexcept {
        std::lock_guard lock(mutex_);
        if(!request || active_!=request || request->cancelled.load() || Phase()!=OptionalInstallPhase::PendingPublication)return false;
        phase_.store(OptionalInstallPhase::DeferredAttempt,std::memory_order_release);return true;
    }
    void Finish(const std::shared_ptr<OptionalInstallRequest>& request,bool mayDefer) noexcept {
        std::shared_ptr<OptionalInstallRequest> release;
        {std::lock_guard lock(mutex_);
            if(!request || active_!=request || Phase()==OptionalInstallPhase::Ready)return; // Stop/Publish already won.
            if(mayDefer && !request->cancelled.load() &&
               (Phase()==OptionalInstallPhase::Attempting || Phase()==OptionalInstallPhase::DeferredAttempt || Phase()==OptionalInstallPhase::PendingPublication)){
                phase_.store(OptionalInstallPhase::PendingPublication,std::memory_order_release);return;
            }
            request->cancelled.store(true,std::memory_order_release);release=std::move(active_);
            phase_.store(OptionalInstallPhase::Terminal,std::memory_order_release);
        } // Owned COM references are released outside the metadata lock.
    }
    PublicationResult Publish(const std::shared_ptr<OptionalInstallRequest>& request,
        const std::shared_ptr<const Coverage>& candidate,const std::shared_ptr<const Coverage>& expected,uint64_t token,bool reuse) noexcept {
        std::shared_ptr<const Coverage> previous;
        {std::lock_guard lock(mutex_);
            const auto phase=Phase();
            if(!request || active_!=request || request->cancelled.load(std::memory_order_acquire) ||
               (phase!=OptionalInstallPhase::Attempting && phase!=OptionalInstallPhase::DeferredAttempt) ||
               PublicPsBinding::stopped.load(std::memory_order_acquire) || !candidate || candidate->refused.load() ||
               coverage.load(std::memory_order_acquire)!=expected)return PublicationResult::Refused;
            if(PublicPsBinding::RefreshToken()!=token){phase_.store(OptionalInstallPhase::PendingPublication,std::memory_order_release);return PublicationResult::PendingRefresh;}
            if(!reuse){previous=coverage.exchange(candidate,std::memory_order_acq_rel);NotePublicationExchange();}
            if(!PublicPsBinding::AcknowledgeRefresh(token)){phase_.store(OptionalInstallPhase::PendingPublication,std::memory_order_release);return PublicationResult::PendingRefresh;}
            phase_.store(OptionalInstallPhase::Ready,std::memory_order_release);
        } // Old COM/endpoint leases release outside the metadata mutex.
        return reuse?PublicationResult::ReusedCoverage:PublicationResult::ReplacedCoverage;
    }
    bool Cancel() noexcept {
        std::shared_ptr<OptionalInstallRequest> release;
        {std::lock_guard lock(mutex_);
            const auto phase=Phase();if(phase==OptionalInstallPhase::Ready || phase==OptionalInstallPhase::Terminal)return false;
            if(active_)active_->cancelled.store(true,std::memory_order_release);
            release=std::move(active_);phase_.store(OptionalInstallPhase::Terminal,std::memory_order_release);
        }
        return true;
    }
    void Stop() noexcept {
        std::shared_ptr<OptionalInstallRequest> release;
        std::shared_ptr<const Coverage> previous;
        {std::lock_guard lock(mutex_);
            PublicPsBinding::stopped.store(true,std::memory_order_release);
            if(active_)active_->cancelled.store(true,std::memory_order_release);
            release=std::move(active_);previous=coverage.exchange({},std::memory_order_acq_rel);
            if(previous)previous->refused.store(true,std::memory_order_release);
            phase_.store(OptionalInstallPhase::Terminal,std::memory_order_release);
        }
    }
};
inline OptionalInstallControl optionalInstall;

// Committed public hooks authorize CPU tickets, independently of the private
// GPU StableLocked/WholeLocked/BindingTableLocked contracts.
inline bool PublicCoverageIntact(const Coverage& c) noexcept {
    if(c.refused.load(std::memory_order_acquire) || !c.publicPsMutationCovered ||
       PublicPsBinding::stopped.load(std::memory_order_acquire))return false;
    for(size_t m=0;m<c.variants.size();++m){
        if(!c.variantCounts[m] || c.variantCounts[m]>c.variants[m].size())return false;
        for(unsigned n=0;n<c.variantCounts[m];++n)if(!c.variants[m][n].Current()){
            c.refused.store(true,std::memory_order_release);return false;
        }
    }
    return true;
}
template<class Interface> inline bool SameInterface(ID3D11DeviceContext* context) noexcept {
    ComPtr<Interface> alias;
    const auto hr=context->QueryInterface(IID_PPV_ARGS(&alias));
    return hr==E_NOINTERFACE || (SUCCEEDED(hr) && alias.Get() &&
        static_cast<IUnknown*>(alias.Get())==static_cast<IUnknown*>(context));
}
// IUnknown has canonical COM identity, not the context interface's address.
// Defined with the observed overload below so both install paths share it.
template<> inline bool SameInterface<IUnknown>(ID3D11DeviceContext* context) noexcept;
// Prepare only the documented immediate-context/device/interface identities.
// Unknown proxies, aliases and deferred contexts cannot use this CPU cache.
inline std::shared_ptr<Coverage> PrepareCoverage(ID3D11DeviceContext* context,ID3D11Device* device) noexcept;
// R193 startup-only receipt. It is passed explicitly only through observed
// overloads; no TLS/global observer, logging or cache admission reads this data.
struct PrepareObservation final {
    const char* scope="none";
    const char* predicate="not-started";
    const char* statusKind="none";
    const char* historicalNativeReason="not-sampled";
    LONG status=0;
    int slot=-1,entry=-1,mask=-1,mode=-1,image=-1,patch=-1,section=-1;
    int modeMismatch[2]{-1,-1};
    uint32_t rva=0;
    size_t offset=0,spanOffset=0,requestedBytes=0,readBytes=0;
    uint64_t apiValue=0;
    DWORD win32=0;
    bool apiValid=false,win32Valid=false,exception=false;
    unsigned nativeInitialized=0;
    bool nativeInitializedValid=false,initializationPending=false;
    void Initialized(unsigned value) noexcept {nativeInitialized=value;nativeInitializedValid=true;}
    void BeforeLeaf(const char* next) noexcept {
        predicate=next;statusKind="none";status=0;
        apiValid=false;apiValue=0;win32Valid=false;win32=0;
        requestedBytes=0;readBytes=0;
    }
    void At(const char* nextScope,uint32_t nextRva=0) noexcept {
        scope=nextScope;rva=nextRva;slot=-1;entry=-1;mask=-1;mode=-1;image=-1;patch=-1;section=-1;
        modeMismatch[0]=-1;modeMismatch[1]=-1;offset=0;spanOffset=0;
        BeforeLeaf("not-evaluated");
    }
    void Before(const char* next) noexcept {At("PrepareCoverage");BeforeLeaf(next);}
    bool Leaf(const char* next,bool value) noexcept {
        predicate=next;statusKind="bool";status=value?TRUE:FALSE;return value;
    }
    bool Boolean(const char* next,bool value,int nextSlot=-1,int nextEntry=-1,
                 int nextMask=-1,uint32_t nextRva=0) noexcept {
        At("PrepareCoverage",nextRva);slot=nextSlot;entry=nextEntry;mask=nextMask;
        return Leaf(next,value);
    }
    HRESULT HResult(const char* next,HRESULT value) noexcept {
        At("PrepareCoverage");predicate=next;statusKind="hresult";status=value;return value;
    }
    void Win32(const char* next,BOOL value,DWORD error) noexcept {
        predicate=next;statusKind="win32_bool";status=value;
        win32=error;win32Valid=true;
    }
};
template<class Interface> inline bool SameInterface(ID3D11DeviceContext* context,
    PrepareObservation& observation,const char* query,const char* nonnull,const char* identity) noexcept {
    ComPtr<Interface> alias;
    const auto hr=observation.HResult(query,context->QueryInterface(IID_PPV_ARGS(&alias)));
    if(hr==E_NOINTERFACE)return true; // unsupported optional interface is unchanged
    if(FAILED(hr))return false;
    if(!observation.Boolean(nonnull,alias.Get()!=nullptr))return false;
    return observation.Boolean(identity,
        static_cast<IUnknown*>(alias.Get())==static_cast<IUnknown*>(context));
}
// R630: normalize BOTH interface identities through real QI(IID_IUnknown).
// A canonical IUnknown may have a different address from the native context.
// It grants no native-call authority: require an exact context round trip and
// leave all versioned-interface, device, table and hook proofs below intact.
// Prepare-only, with bounded COM ownership; never called from a draw/cache hit.
template<> inline bool SameInterface<IUnknown>(ID3D11DeviceContext* context,
    PrepareObservation& observation,const char* query,const char* nonnull,const char* identity) noexcept {
    ComPtr<IUnknown> alias;
    if(FAILED(observation.HResult(query,context->QueryInterface(IID_PPV_ARGS(&alias)))))return false;
    // IUnknown is mandatory; E_NOINTERFACE cannot authorize this cache.
    if(!observation.Boolean(nonnull,alias.Get()!=nullptr))return false;
    ComPtr<IUnknown> contextIdentity,aliasIdentity;
    if(FAILED(observation.HResult("IUnknown-context-canonical-QueryInterface",
        context->QueryInterface(IID_PPV_ARGS(&contextIdentity)))))return false;
    if(!observation.Boolean("IUnknown-context-canonical-nonnull",contextIdentity.Get()!=nullptr))return false;
    if(FAILED(observation.HResult("IUnknown-self-QueryInterface",
        alias->QueryInterface(IID_PPV_ARGS(&aliasIdentity)))))return false;
    if(!observation.Boolean("IUnknown-self-identity",aliasIdentity.Get()==alias.Get()))return false;
    if(!observation.Boolean(identity,contextIdentity.Get()==aliasIdentity.Get()))return false;
    ComPtr<ID3D11DeviceContext> roundTrip;
    if(FAILED(observation.HResult("IUnknown-context-QueryInterface",
        aliasIdentity->QueryInterface(IID_PPV_ARGS(&roundTrip)))))return false;
    return observation.Boolean("IUnknown-context-identity",roundTrip.Get()==context);
}
template<> inline bool SameInterface<IUnknown>(ID3D11DeviceContext* context) noexcept {
    PrepareObservation observation;
    return SameInterface<IUnknown>(context,observation,"IUnknown-QueryInterface","IUnknown-nonnull","IUnknown-identity");
}
// Prepare only the documented immediate-context/device/interface identities.
// Unknown proxies, aliases and deferred contexts cannot use this CPU cache.
inline std::shared_ptr<Coverage> PrepareCoverage(ID3D11DeviceContext* context,ID3D11Device* device,
    PrepareObservation& observation) noexcept {
    observation.historicalNativeReason=GenshinNativeQuery::detail::PublishedFailure(); // diagnostic only
    try {
        if(!observation.Boolean("context-nonnull",context!=nullptr) ||
           !observation.Boolean("device-nonnull",device!=nullptr))return {};
        ComPtr<ID3D11Device> actual;context->GetDevice(&actual);
        if(!observation.Boolean("context-device-identity",actual.Get()==device) ||
           !observation.Boolean("context-type-immediate",context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE))return {};
        ComPtr<ID3D11DeviceContext> immediate;device->GetImmediateContext(&immediate);
        if(!observation.Boolean("public-immediate-context-identity",immediate.Get()==context))return {};
        auto c=std::make_shared<Coverage>();c->context=context;c->device=device;
        if(FAILED(observation.HResult("context1-QueryInterface",context->QueryInterface(IID_PPV_ARGS(&c->context1)))))return {};
        if(!observation.Boolean("context1-identity",static_cast<ID3D11DeviceContext*>(c->context1.Get())==context))return {};
        if(!SameInterface<IUnknown>(context,observation,"IUnknown-QueryInterface","IUnknown-nonnull","IUnknown-identity") ||
           !SameInterface<ID3D11DeviceContext2>(context,observation,"context2-QueryInterface","context2-nonnull","context2-identity") ||
           !SameInterface<ID3D11DeviceContext3>(context,observation,"context3-QueryInterface","context3-nonnull","context3-identity") ||
           !SameInterface<ID3D11DeviceContext4>(context,observation,"context4-QueryInterface","context4-nonnull","context4-identity"))return {};
        c->dispatch=PublicPsBinding::ReadDispatch(context);
        for(size_t i=0;i<c->dispatch.size();++i)
            if(!observation.Boolean("public-mutation-method-nonnull",c->dispatch[i]!=nullptr,static_cast<int>(Slots[i])))return {};
        observation.Boolean("public-context-prepared",true);return c;
    }catch(...){observation.exception=true;return {};}
}
inline std::shared_ptr<Coverage> PrepareCoverage(ID3D11DeviceContext* context,ID3D11Device* device) noexcept {
    PrepareObservation observation;return PrepareCoverage(context,device,observation);
}
inline unsigned ChangedRoles(const PublicPsBinding::Dispatch& a,const PublicPsBinding::Dispatch& b) noexcept {
    unsigned mask=0;for(unsigned m=0;m<a.size();++m)if(a[m]!=b[m])mask|=1u<<m;return mask;
}
inline unsigned FirstRole(unsigned mask) noexcept {
    for(unsigned m=0;m<PublicPsBinding::MethodCount;++m)if(mask&(1u<<m))return m;return PublicPsBinding::MethodCount;
}
// Exact actually observed public aliases; fixed owner-thread storage only.
struct RefreshWitness final {
    ID3D11DeviceContext* context=nullptr;uint64_t deviceEpoch=0,token=0;
    std::array<std::array<void*,PublicPsBinding::Capacity>,PublicPsBinding::MethodCount> aliases{};
    std::array<unsigned,PublicPsBinding::MethodCount> counts{};
    bool gap=false,refused=false;
    Metrics::Refresh lastReason=Metrics::Refresh::Count;
    unsigned lastRole=PublicPsBinding::MethodCount,lastRoleMask=0;
    const char* refusal="none";
};
inline thread_local RefreshWitness refreshWitness;
inline thread_local RefreshWitness publicationWitness; // fixed last checked receipt, not authority
inline uint64_t RequestPublicRefresh(ID3D11DeviceContext* context,uint64_t epoch,Metrics::Refresh reason,
    unsigned roleMask,const PublicPsBinding::Dispatch& actual) noexcept {
    auto& w=refreshWitness;w.gap=true;w.lastReason=reason;w.lastRole=FirstRole(roleMask);w.lastRoleMask=roleMask;
    if(w.context && (w.context!=context || w.deviceEpoch!=epoch)){w.refused=true;w.refusal="observed-alias-identity";}
    else {w.context=context;w.deviceEpoch=epoch;
        for(unsigned m=0;m<actual.size();++m){
            if(w.counts[m]>w.aliases[m].size()){w.refused=true;w.refusal="observed-alias-shape";continue;}
            if(!actual[m]){w.refused=true;w.refusal="observed-null-alias";continue;}
            bool found=false;for(unsigned n=0;n<w.counts[m];++n)found|=w.aliases[m][n]==actual[m];
            if(!found){if(w.counts[m]>=w.aliases[m].size()){w.refused=true;w.refusal="observed-alias-capacity";}else w.aliases[m][w.counts[m]++]=actual[m];}
        }
    }
    w.token=PublicPsBinding::RequestRefresh();
    Metrics::RefreshEvent(reason,roleMask,w.token,context,epoch);return w.token;
}
// Installation/publication only; do NOT add these bounded shape walks to draw proofs.
inline bool CoverageShapeIntact(const Coverage& c) noexcept {
    if(!c.context || !c.context1 || !c.device || !c.publicPsMutationCovered ||
       static_cast<ID3D11DeviceContext*>(c.context1.Get())!=c.context.Get())return false;
    for(unsigned m=0;m<c.endpoints.size();++m){
        const auto count=c.variantCounts[m];
        if(!c.dispatch[m] || c.endpoints[m].requested!=c.dispatch[m] || !count || count>c.variants[m].size())return false;
        bool selected=false;
        for(unsigned n=0;n<count;++n){const auto& a=c.variants[m][n];
            if(!a.requested)return false;
            for(unsigned k=0;k<n;++k)if(a.requested==c.variants[m][k].requested)return false;
            if(a.requested==c.dispatch[m]){
                if(!a.SameReceipt(c.endpoints[m]))return false;selected=true;
            }
        }
        if(!selected)return false;
    }
    return true;
}
inline bool SameCoverage(const Coverage& a,const Coverage& b) noexcept {
    if(!CoverageShapeIntact(a) || !CoverageShapeIntact(b) ||
       a.context.Get()!=b.context.Get() || a.context1.Get()!=b.context1.Get() || a.device.Get()!=b.device.Get() ||
       a.dispatch!=b.dispatch || a.publicPsMutationCovered!=b.publicPsMutationCovered)return false;
    for(unsigned m=0;m<a.endpoints.size();++m){
        if(!a.endpoints[m].SameReceipt(b.endpoints[m]) || a.variantCounts[m]!=b.variantCounts[m])return false;
        for(unsigned n=0;n<a.variantCounts[m];++n){bool found=false;
            for(unsigned k=0;k<b.variantCounts[m];++k)if(a.variants[m][n].requested==b.variants[m][k].requested){
                if(!a.variants[m][n].SameReceipt(b.variants[m][k]))return false;found=true;break;}
            if(!found)return false;
        }
    }
    return true;
}
inline bool SelectKnownCoverageDispatch(Coverage& c,const PublicPsBinding::Dispatch& actual) noexcept {
    auto selected=c.endpoints;
    for(unsigned m=0;m<actual.size();++m){bool found=false;
        if(!c.variantCounts[m] || c.variantCounts[m]>c.variants[m].size())return false;
        for(unsigned n=0;n<c.variantCounts[m];++n)if(c.variants[m][n].requested==actual[m]){
            if(found || !c.variants[m][n].Current())return false;selected[m]=c.variants[m][n];found=true;}
        if(!found)return false;
    }
    c.dispatch=actual;c.endpoints=selected;return true; // candidate only, never a shader ticket
}
inline PublicationResult PublishCoverage(std::shared_ptr<Coverage> c,
    const std::shared_ptr<OptionalInstallRequest>& request,const InstallOwnerScope& owner,
    const RefreshWitness& installed,uint64_t token) noexcept;
inline bool CoverageCurrent(const Coverage& c) noexcept {
    // Scalar publication state only. Endpoint bytes are checked at the owning
    // DrawScope/mutation boundary, never repeatedly around Family::Read.
    return !PublicPsBinding::RefreshRequested() && !c.refused.load(std::memory_order_acquire) &&
        c.publicPsMutationCovered && !PublicPsBinding::stopped.load(std::memory_order_acquire);
}
inline bool PublicRefreshRequested() noexcept {return PublicPsBinding::RefreshRequested();}

struct Domain final {
    const void* owner=nullptr;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device> device;
    uint64_t deviceEpoch=0,configEpoch=0;
    DWORD thread=0;
    std::shared_ptr<const Coverage> hooks;
    PublicPsBinding::Dispatch dispatch{};
    // Public mutation roles, not GPU history or private native execution fields.
    // A shader pointer alone never certifies its CURRENT binding.
    std::atomic<uint64_t> sequence{1};
    std::atomic<unsigned> inFlight{0};
    std::atomic<bool> retired{false},foreignThread{false};
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    ComPtr<ID3D11PixelShader> shader; // accessed only while busy is held
    uint64_t shaderSequence=0;
};
inline std::atomic<std::shared_ptr<Domain>> current;
// Non-owning publication tag, NEVER dereferenced or used as a binding proof.
// Empty draw returns must not enter atomic<shared_ptr>'s internal lock.
inline std::atomic<Domain*> liveDomain{nullptr};
static_assert(std::atomic<Domain*>::is_always_lock_free,
    "draw-return early rejection must remain lock-free");
class Access final {
    Domain* domain_=nullptr;
public:
    explicit Access(Domain& d) noexcept {if(!d.busy.test_and_set(std::memory_order_acquire))domain_=&d;}
    ~Access(){if(domain_)domain_->busy.clear(std::memory_order_release);}
    explicit operator bool()const noexcept{return domain_!=nullptr;}
    Access(const Access&)=delete;Access& operator=(const Access&)=delete;
};
inline void MarkRetired(Domain& d) noexcept {
    d.retired.store(true,std::memory_order_release);
    auto* expected=&d;
    // A delayed old owner's retirement cannot erase a newer publication tag.
    liveDomain.compare_exchange_strong(expected,nullptr,std::memory_order_acq_rel,std::memory_order_acquire);
}
inline uint64_t Advance(Domain& d) noexcept {
    auto v=d.sequence.load(std::memory_order_acquire);
    while(v!=UINT64_MAX){
        if(d.sequence.compare_exchange_weak(v,v+1,std::memory_order_acq_rel))return v+1;
    }
    MarkRetired(d);return 0; // never wrap/ABA
}
inline void Invalidate(Domain& d,bool retire=false) noexcept {
    if(retire)MarkRetired(d);
    Advance(d);
}
inline void RetireOwner(const void* owner=nullptr) noexcept {
    auto d=current.load(std::memory_order_acquire);
    if(!d || (owner && d->owner!=owner))return;
    Invalidate(*d,true);
    auto expected=d;
    current.compare_exchange_strong(expected,{},std::memory_order_acq_rel);
    // Active scopes keep a bounded lease; endpoints release after those return.
}
inline void ForgetCoverage() noexcept {
    optionalInstall.Stop();RetireOwner();
    // Hooks remain transparent pass-throughs. A terminal shutdown cannot
    // retain the old device/context or lend their manifest to a new lifetime.
}
inline PublicationResult PublishCoverage(std::shared_ptr<Coverage> c,
    const std::shared_ptr<OptionalInstallRequest>& request,const InstallOwnerScope& owner,
    const RefreshWitness& installed,uint64_t token) noexcept {
    const auto prior=coverage.load(std::memory_order_acquire);
    publicationFailure="none";publicationWitness=installed;
    const auto refuse=[&](const char* why) noexcept {publicationFailure=why;Coverage::refused.store(true,std::memory_order_release);RetireOwner();return PublicationResult::Refused;};
    const auto pending=[&](const char* why) noexcept {publicationFailure=why;publicationWitness=refreshWitness;RetireOwner();return PublicationResult::PendingRefresh;};
    const auto ownerCurrent=[&]() noexcept {return request && !request->cancelled.load(std::memory_order_acquire) && owner.Current() &&
        request->owner==owner.Owner() && request->ownerEpoch==owner.OwnerEpoch() && request->context.Get()==owner.Context() &&
        request->device.Get()==owner.Device() && request->deviceEpoch==owner.DeviceEpoch() &&
        request->controlsEpoch==owner.ControlsEpoch() && request->threadId==GetCurrentThreadId();};
    if(!c || !ownerCurrent() || c->context.Get()!=owner.Context() || c->device.Get()!=owner.Device())return refuse("public-owner-identity");
    if(!CoverageShapeIntact(*c) || (prior && !CoverageShapeIntact(*prior)))return refuse("public-receipt-shape");
    if((prior && prior->refused.load()) || !PublicCoverageIntact(*c))return refuse("public-receipt-refused");
    // This exact generation and witness were frozen BEFORE batch planning.
    // Never adopt a request which arrived during authentication/commit, even
    // when its current vector happens to have switched back to known aliases.
    if(token==UINT64_MAX || PublicPsBinding::RefreshToken()==UINT64_MAX)return refuse("refresh-generation-exhausted");
    if(installed.refused)return refuse(installed.refusal);
    if(refreshWitness.refused)return refuse(refreshWitness.refusal);
    if(PublicPsBinding::RefreshToken()!=token)return pending("install-generation-changed-pending");
    if(installed.token && installed.token!=token)return refuse("install-witness-generation");
    if(installed.context && (installed.context!=c->context.Get() || installed.deviceEpoch!=request->deviceEpoch))return refuse("install-witness-identity");
    const auto actual=PublicPsBinding::ReadDispatch(c->context.Get());
    if(actual!=c->dispatch){
        RetireOwner(); // known normalization cannot carry an older shader ticket
        if(!SelectKnownCoverageDispatch(*c,actual)){
            RequestPublicRefresh(c->context.Get(),request->deviceEpoch,Metrics::Refresh::PublishDispatchChanged,ChangedRoles(c->dispatch,actual),actual);
            publicationWitness=refreshWitness;if(refreshWitness.refused)return refuse(refreshWitness.refusal);
            publicationFailure="dispatch-uncovered-pending";
            return PublicationResult::PendingRefresh;
        }
    }
    const auto finalDispatch=PublicPsBinding::ReadDispatch(c->context.Get());
    if(finalDispatch!=c->dispatch){
        RetireOwner();RequestPublicRefresh(c->context.Get(),request->deviceEpoch,Metrics::Refresh::PublishDispatchChanged,ChangedRoles(c->dispatch,finalDispatch),finalDispatch);
        publicationWitness=refreshWitness;if(refreshWitness.refused)return refuse(refreshWitness.refusal);
        publicationFailure="dispatch-changed-again-pending";
        return PublicationResult::PendingRefresh;
    }
    auto& w=refreshWitness;
    if(w.refused)return refuse(w.refusal);
    if(PublicPsBinding::RefreshToken()!=token)return pending("install-generation-changed-pending");
    if(w.context && (w.context!=c->context.Get() || w.deviceEpoch!=request->deviceEpoch))return refuse("observed-alias-identity");
    // Disappearing aliases stay mandatory: every frozen witness must have its
    // own authenticated forwarding receipt, including aliases absent at Present.
    for(unsigned m=0;m<installed.aliases.size();++m){
        if(installed.counts[m]>installed.aliases[m].size())return refuse("observed-alias-shape");
        for(unsigned n=0;n<installed.counts[m];++n){bool found=false;
            for(unsigned k=0;k<c->variantCounts[m];++k)found|=c->variants[m][k].requested==installed.aliases[m][n];
            if(!found)return refuse("uncovered-observed-alias");
        }
    }
    // Same-generation TLS metadata cannot silently grow outside that snapshot.
    if(w.context!=installed.context || w.deviceEpoch!=installed.deviceEpoch || w.token!=installed.token ||
       w.counts!=installed.counts || w.aliases!=installed.aliases || w.gap!=installed.gap)return refuse("install-witness-changed");
    if(!ownerCurrent())return refuse("public-owner-changed");
    if(!PublicCoverageIntact(*c))return refuse("public-receipt-damaged");
    if(prior && !PublicCoverageIntact(*prior))return refuse("prior-public-receipt-damaged");
    const bool same=prior && SameCoverage(*prior,*c);
    auto d=current.load(std::memory_order_acquire);
    const bool preserve=same && !w.gap && !(token&1) && d && !d->retired.load() && !d->foreignThread.load() &&
        !d->inFlight.load() && d->owner==owner.BindingOwner() && d->hooks==prior && d->context.Get()==owner.Context() &&
        d->device.Get()==owner.Device() && d->deviceEpoch==owner.DeviceEpoch() && d->configEpoch==owner.ControlsEpoch() &&
        d->thread==GetCurrentThreadId();
    if(!preserve)RetireOwner();
    if(!ownerCurrent())return refuse("public-owner-changed-before-exchange");
    const auto result=optionalInstall.Publish(request,same?prior:std::shared_ptr<const Coverage>(c),prior,token,same);
    if(result==PublicationResult::Refused)return refuse("publication-request-or-lifetime-changed");
    if(result==PublicationResult::PendingRefresh){publicationFailure="refresh-generation-changed";RetireOwner();return result;}
    if(w.token==0 || w.token==token)w={}; // no resurrection of gap-before shader
    return result;
}
inline void RefusePublicCoverage() noexcept {
    Coverage::refused.store(true,std::memory_order_release);RetireOwner();
}
inline void ObserveConfig(uint64_t epoch,bool enabled) noexcept {
    Family::ShaderCacheMetrics::Controls(epoch,enabled);
    auto d=current.load(std::memory_order_acquire);
    if(d && (!enabled || epoch!=d->configEpoch))RetireOwner(d->owner);
}
// Select only endpoints actually observed on this exact public context. A
// per-role combination is legitimate PUBLIC API dispatch, not private GPU table
// normalization authority. The caller invalidates before a vector change.
inline bool SelectPublicDispatch(Domain& d) noexcept {
    const auto actual=PublicPsBinding::ReadDispatch(d.context.Get());
    unsigned unknownRoles=0;
    for(size_t m=0;m<actual.size();++m){
        const auto count=d.hooks->variantCounts[m];
        if(!count || count>d.hooks->variants[m].size())return false;
        bool known=false;
        for(unsigned n=0;n<count;++n)if(d.hooks->variants[m][n].requested==actual[m]){known=true;break;}
        if(!known)unknownRoles|=1u<<m;
    }
    if(unknownRoles){RequestPublicRefresh(d.context.Get(),d.deviceEpoch,Metrics::Refresh::UnknownRoleAlias,unknownRoles,actual);return false;}
    d.dispatch=actual;return true;
}
inline bool BindingDispatchCurrent(const Domain& d) noexcept {
    return PublicPsBinding::ReadDispatch(d.context.Get())==d.dispatch;
}
inline void ObserveReturnedDispatch(Domain& d) noexcept {
    Metrics::CostScope boundary(Metrics::Root::Boundary,d.context.Get(),d.device.Get(),d.deviceEpoch);
    if(d.retired.load(std::memory_order_acquire))return;
    if(d.thread!=GetCurrentThreadId()){
        d.foreignThread.store(true,std::memory_order_release);
        d.hooks->refused.store(true,std::memory_order_release);Invalidate(d,true);return;
    }
    if(!BindingDispatchCurrent(d)){
        Invalidate(d); // including a switch to another already observed vector
        if(!SelectPublicDispatch(d))Invalidate(d,true);
    }
}
inline void ObserveNativeDrawReturn(ID3D11DeviceContext* context) noexcept {
    // Check at RETURN: BeforeDraw may have created a domain after guard entry.
    if(!liveDomain.load(std::memory_order_acquire))return;
    auto d=current.load(std::memory_order_acquire); // the real strong owner lease
    if(d && d->context.Get()==context)ObserveReturnedDispatch(*d);
}
// Placed in ALL seven existing draw wrappers, including early-skip and
// internal paths. Native Draw itself does not change PS state, but its runtime
// normalization must not silently carry a ticket onto an unknown public vector.
class NativeDrawReturnScope final {
    ID3D11DeviceContext* context_;
public:
    explicit NativeDrawReturnScope(ID3D11DeviceContext* context) noexcept:context_(context){}
    ~NativeDrawReturnScope(){if(context_)ObserveNativeDrawReturn(context_);}
    NativeDrawReturnScope(const NativeDrawReturnScope&)=delete;
    NativeDrawReturnScope& operator=(const NativeDrawReturnScope&)=delete;
};
// The public API is the mutation contract: PSSetShader, ClearState,
// ExecuteCommandList, FinishCommandList and SwapDeviceContextState. Native
// Draw is NOT a PS mutation. Unknown dispatch changes never preserve a ticket.
// Exactly one receipt check belongs at an owner entry or mutation return, not
// before AND after every metadata lookup. Retained aliases also need coverage:
// a caller may hold an older public method pointer after the vector changes.
inline bool BindingBoundaryCurrent(Domain& domain) noexcept {
    Metrics::CostScope metric(Metrics::Root::Boundary,domain.context.Get(),domain.device.Get(),domain.deviceEpoch);
    if(domain.retired.load(std::memory_order_acquire) || domain.foreignThread.load(std::memory_order_acquire) ||
       GetCurrentThreadId()!=domain.thread || !CoverageCurrent(*domain.hooks)){
        Invalidate(domain,true);return false;
    }
    if(!BindingDispatchCurrent(domain)){
        Invalidate(domain);
        if(!SelectPublicDispatch(domain)){Invalidate(domain,true);return false;}
    }
    if(!PublicCoverageIntact(*domain.hooks)){Invalidate(domain,true);return false;}
    return true;
}
// Within an owned boundary, keep only the live scalar/dispatch/ordering guards.
// An intervening observed mutation advances sequence before any original call;
// an unexpected public-vector change invalidates before an old shader is read.
inline bool ProofCurrent(Domain& domain) noexcept {
    if(domain.retired.load(std::memory_order_acquire) || domain.foreignThread.load(std::memory_order_acquire) ||
       GetCurrentThreadId()!=domain.thread || !CoverageCurrent(*domain.hooks)){
        Invalidate(domain,true);return false;
    }
    if(!BindingDispatchCurrent(domain)){
        Invalidate(domain);
        if(!SelectPublicDispatch(domain))Invalidate(domain,true);
        return false;
    }
    return true;
}
// nativeMutex serializes the capture owner, but public setters run outside it.
// A cold/late-installed domain is rebuilt from a REAL getter. Completed public
// setters may publish between draws. Every opaque boundary invalidates before
// forwarding and stays in-flight through return; it never infers restoration.
// A normal return to the game or a native Draw is not a documented PS mutation,
// so it does not by itself discard the correctly retained binding.
inline std::shared_ptr<Domain> Bind(const void* owner,ID3D11DeviceContext* context,
    ID3D11Device* device,uint64_t deviceEpoch,uint64_t configEpoch) noexcept {
    if(!owner || !context || !device || !deviceEpoch || !configEpoch)return {};
    auto prior=current.load(std::memory_order_acquire);
    const auto published=coverage.load(std::memory_order_acquire);
    if(prior && !prior->retired.load() && prior->owner==owner && prior->context.Get()==context &&
       prior->device.Get()==device && prior->deviceEpoch==deviceEpoch && prior->configEpoch==configEpoch &&
       prior->thread==GetCurrentThreadId() && prior->hooks==published && !prior->foreignThread.load())return prior;
    // A thread change cannot steal the existing owner's cache.
    if(prior && prior->owner==owner && prior->thread!=GetCurrentThreadId()){
        Invalidate(*prior,true);return {};
    }
    RetireOwner();
    auto c=published;
    if(!c || c->context.Get()!=context || c->device.Get()!=device || !CoverageCurrent(*c))return {};
    try {
        auto result=std::make_shared<Domain>();
        result->owner=owner;result->context=context;result->device=device;
        result->deviceEpoch=deviceEpoch;result->configEpoch=configEpoch;
        result->thread=GetCurrentThreadId();result->hooks=c;
        if(!SelectPublicDispatch(*result))return {};
        // Announce before shared publication; no throwing work between stores.
        // A later retirement cannot be followed by a stale live-tag store.
        liveDomain.store(result.get(),std::memory_order_release);
        current.store(result,std::memory_order_release);return result;
    }catch(...){return {};}
}

// Every observed PS mutation or opaque callback creates this BEFORE forwarding
// and keeps it through return. Internal calls are deliberately NOT suppressed.
// Restore=true and HRESULT success never synthesize a restored shader binding.
class Mutation final {
    std::shared_ptr<Domain> domain_;
    uint64_t ticket_=0;
public:
    explicit Mutation(ID3D11DeviceContext* context) noexcept {
        auto d=current.load(std::memory_order_acquire);
        if(!d || d->context.Get()!=context)return;
        domain_=std::move(d);
        domain_->inFlight.fetch_add(1,std::memory_order_acq_rel);
        ticket_=Advance(*domain_);
        if(domain_->thread!=GetCurrentThreadId()){
            domain_->foreignThread.store(true,std::memory_order_release);
            domain_->hooks->refused.store(true,std::memory_order_release); // unknown cross-thread ordering: permanent getter fallback
        }
    }
    ~Mutation(){
        if(!domain_)return;
        // Observe state-swap/normalization at its RETURN as well as at draws.
        // An unknown vector stays refused even if a later call switches back.
        ObserveReturnedDispatch(*domain_);
        domain_->inFlight.fetch_sub(1,std::memory_order_acq_rel);
    }
    Mutation(const Mutation&)=delete;Mutation& operator=(const Mutation&)=delete;
    void SetterReturned(ID3D11PixelShader* requested,ID3D11ClassInstance* const* classes,UINT count) noexcept {
        auto& d=domain_;if(!d)return;
        Metrics::CostScope cost(Metrics::Root::Setter,d->context.Get(),d->device.Get(),d->deviceEpoch);
        const auto event=[&](Metrics::Setter result) noexcept {Metrics::SetterResult(result,d->context.Get(),d->device.Get(),d->deviceEpoch);};
        event(Metrics::Setter::Entered);
        // Preserve the original order: class/null/array cases retain fallback.
        // A non-null zero-count class array is still a conservative cache miss.
        // Nested public forwarding must finish before the outer real getter;
        // PSSetShader is void, so arguments alone can never certify the ticket.
        if(!ticket_ || !requested || classes || count || d->inFlight.load()!=1 || !BindingBoundaryCurrent(*d)){
            event(Metrics::Setter::Skipped);return;
        }
        const auto returnedSequence=d->sequence.load(std::memory_order_acquire);
        if(!returnedSequence || returnedSequence==UINT64_MAX){event(Metrics::Setter::ProofChanged);return;}
        ComPtr<ID3D11PixelShader> actual;
        Metrics::ApiCall(Metrics::Api::SetterPs,d->context.Get(),d->device.Get(),d->deviceEpoch);
        d->context->PSGetShader(&actual,nullptr,nullptr);
        if(actual.Get()!=requested){event(Metrics::Setter::GetterMismatch);return;}
        Access access(*d);if(!access){event(Metrics::Setter::AccessBusy);return;}
        if(d->inFlight.load()!=1 || d->sequence.load()!=returnedSequence || !ProofCurrent(*d)){
            event(Metrics::Setter::ProofChanged);return;
        }
        d->shader=std::move(actual);d->shaderSequence=returnedSequence;
        event(Metrics::Setter::Accepted); // only after the real ticket was submitted
    }
};

class DrawScope final {
    inline static thread_local DrawScope* active_=nullptr;
    DrawScope* previous_=nullptr;
    std::shared_ptr<Domain> domain_;
    bool (*controlsCurrent_)(uint64_t)=nullptr;
    uint64_t metricsFrame_=0;
public:
    DrawScope(const void* owner,ID3D11DeviceContext* context,ID3D11Device* device,
        uint64_t deviceEpoch,uint64_t configEpoch,bool allowed,bool (*controlsCurrent)(uint64_t)) noexcept
        :previous_(active_),controlsCurrent_(controlsCurrent) {
        active_=this; // a nested scope shadows/refuses the outer proof
        if(previous_){if(previous_->domain_)Invalidate(*previous_->domain_);return;}
        metricsFrame_=Family::ShaderCacheMetrics::Draw(owner,context,device,deviceEpoch,configEpoch);
        Metrics::CostScope boundary(Metrics::Root::Boundary,context,device,deviceEpoch);
        if(allowed && controlsCurrent_ && controlsCurrent_(configEpoch)){
            domain_=Bind(owner,context,device,deviceEpoch,configEpoch);
            if(domain_ && !BindingBoundaryCurrent(*domain_))domain_.reset();
        }
    }
    ~DrawScope(){
        if(domain_){
            ObserveReturnedDispatch(*domain_);
            // A normal scope exit preserves the getter-backed PS identity.
            // Mutations and unknown returned vectors already invalidate it.
        }
        active_=previous_;
    }
    DrawScope(const DrawScope&)=delete;DrawScope& operator=(const DrawScope&)=delete;
    static bool ActiveForMetrics() noexcept {return active_!=nullptr;}
    static bool ReentrantForMetrics() noexcept {return active_ && active_->previous_;}
    // Begin BEFORE getters. A mutation/reentry between the read and publication
    // cannot turn a prior observation into a current ticket.
    static uint64_t BeginRead(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept {
        auto* scope=active_;if(!scope || scope->previous_ || !scope->domain_)return 0;
        auto& d=*scope->domain_;
        if(d.context.Get()!=context || d.device.Get()!=device || d.deviceEpoch!=epoch ||
           d.inFlight.load(std::memory_order_acquire) || !scope->controlsCurrent_(d.configEpoch) || !ProofCurrent(d))return 0;
        return d.sequence.load(std::memory_order_acquire);
    }
    static void RememberReturned(uint64_t ticket,ID3D11DeviceContext* context,ID3D11Device* device,
        uint64_t epoch,ID3D11PixelShader* shader,const Family::Shader& fact) noexcept {
        auto* scope=active_;
        if(!ticket || ticket==UINT64_MAX || !shader || !fact.Known() || !scope || scope->previous_ || !scope->domain_)return;
        auto& d=*scope->domain_;Access access(d);if(!access)return;
        if(d.context.Get()!=context || d.device.Get()!=device || d.deviceEpoch!=epoch || d.inFlight.load() ||
           d.sequence.load()!=ticket || !scope->controlsCurrent_(d.configEpoch) || !ProofCurrent(d))return;
        d.shader=shader;d.shaderSequence=ticket; // strongly held REAL getter-returned COM object
    }
    // Actual bound-object consumer for the existing late accumulate getter.
    // This returns the REAL strongly held PS object, never just its metadata.
    // The caller keeps its original PSGetShader fallback on every refusal.
    static bool TryShader(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch,
        ComPtr<ID3D11PixelShader>& out) noexcept {
        auto* scope=active_;if(!scope || scope->previous_)return false;
        const auto finish=[&](bool hit,unsigned availability) noexcept {
            Family::ShaderCacheMetrics::BindingResult(scope->metricsFrame_,Metrics::Consumer::Late,hit,availability);return hit;
        };
        if(!scope->domain_)return finish(false,1);
        auto& d=*scope->domain_;Access access(d);if(!access)return finish(false,0);
        const auto ticket=d.sequence.load(std::memory_order_acquire);
        if(!d.shader || d.shaderSequence!=ticket || d.inFlight.load() ||
           d.context.Get()!=context || d.device.Get()!=device || d.deviceEpoch!=epoch ||
           !scope->controlsCurrent_(d.configEpoch) || !ProofCurrent(d))return finish(false,1);
        ComPtr<ID3D11PixelShader> held=d.shader;
        if(d.sequence.load()!=ticket || d.inFlight.load() || !ProofCurrent(d) ||
           !scope->controlsCurrent_(d.configEpoch))return finish(false,1);
        out=std::move(held);return finish(true,2);
    }
    static bool TryCurrent(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch,
                           Family::ReadObservation* observation,Family::Shader& out) noexcept {
        auto* scope=active_;if(!scope || scope->previous_)return false;
        const auto finish=[&](bool hit,unsigned availability)noexcept {
            Family::ShaderCacheMetrics::BindingResult(scope->metricsFrame_,Metrics::Consumer::Current,hit,availability);return hit;
        };
        if(!scope->domain_)return finish(false,1);
        auto& d=*scope->domain_;
        const auto ticket=d.sequence.load(std::memory_order_acquire);
        // Empty/expired tickets leave BEFORE table/epoch/config proof.
        Access access(d);if(!access)return finish(false,0);
        if(!d.shader || d.shaderSequence!=ticket || d.inFlight.load())return finish(false,1);
        if(d.context.Get()!=context || d.device.Get()!=device || d.deviceEpoch!=epoch ||
           !scope->controlsCurrent_(d.configEpoch) || !ProofCurrent(d)){
            Invalidate(d);return finish(false,1);
        }
        Family::ReadObservation read;
        auto value=Family::Read(device,d.shader.Get(),epoch,&read);
        // A real fresh Read retains all creator/returned-owner/private-data/
        // digest/epoch checks. We do not cache a Family result or nrProfile.
        if(!value.Known() || d.sequence.load()!=ticket || d.inFlight.load() || d.retired.load() ||
           d.foreignThread.load() || !ProofCurrent(d) ||
           !scope->controlsCurrent_(d.configEpoch)){Invalidate(d);return finish(false,1);}
        if(observation)*observation=read;out=std::move(value);return finish(true,2);
    }
};
} // namespace GenshinLegacy::PsState
