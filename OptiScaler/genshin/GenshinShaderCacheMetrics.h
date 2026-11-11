// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
struct ID3D11Device;
struct ID3D11DeviceContext;
namespace GenshinLegacy::Family::ShaderCacheMetrics {
enum class Consumer : unsigned {Current,Late,Count};
enum class Api : unsigned {ContextDevice,ShaderDevice,CurrentPs,LatePs,SetterPs,Count};
enum class Setter : unsigned {Entered,Skipped,GetterMismatch,AccessBusy,ProofChanged,Accepted,Count};
enum class Refresh : unsigned {UnknownRoleAlias,PublishDispatchChanged,BeforeAttach,Count};
enum class Publication : unsigned {ReusedCoverage,ReplacedCoverage,PendingRefresh,Refused,Count};
inline const char* PublicationName(Publication p) noexcept {
    switch(p){case Publication::ReusedCoverage:return "ReusedCoverage";case Publication::ReplacedCoverage:return "ReplacedCoverage";
    case Publication::PendingRefresh:return "PendingRefresh";case Publication::Refused:return "Refused";default:return "UNKNOWN";}
}
inline const char* RefreshName(Refresh r) noexcept {
    switch(r){case Refresh::UnknownRoleAlias:return "unknown-role-alias";case Refresh::PublishDispatchChanged:return "publish-dispatch-changed";
    case Refresh::BeforeAttach:return "before-attach";default:return "UNKNOWN";}
}
inline const char* RoleName(unsigned r) noexcept {
    switch(r){case 0:return "PSSetShader";case 1:return "ClearState";case 2:return "ExecuteCommandList";
    case 3:return "FinishCommandList";case 4:return "SwapDeviceContextState";default:return "all-or-UNKNOWN";}
}
enum class Root : unsigned {Current,Boundary,Setter};
enum class Cost : unsigned {CurrentHit,CurrentMiss,Boundary,Setter,Count};
enum class Part : unsigned {TryBinding,ContextDevice,PsGetter,FallbackRead,RememberReturned,Count};
template<class E> inline constexpr unsigned Size=static_cast<unsigned>(E::Count);
struct Reading {
    uint64_t calls=0,samples=0,ticks=0,max=0,clockFailed=0;
    void Add(const Reading& r) noexcept {calls+=r.calls;samples+=r.samples;ticks+=r.ticks;
        if(r.max>max)max=r.max;clockFailed+=r.clockFailed;}
};
struct Details {
    std::array<uint64_t,Size<Consumer>> hits{},misses{};
    std::array<uint64_t,Size<Api>> api{};
    std::array<uint64_t,Size<Setter>> setter{};
    std::array<uint64_t,Size<Refresh>> refresh{};
    std::array<uint64_t,5> refreshRoles{};
    std::array<uint64_t,Size<Publication>> publication{};
    std::array<Reading,Size<Cost>> costs{};
    std::array<Reading,Size<Part>> parts{};
    Reading currentSelf{};
    uint64_t lateObservedReuse=0,nestedSuppressed=0,excluded=0;
    uint64_t refreshToken=0,publicationId=0; // last observed state, never summed
    template<class F> void Fields(F&& f) noexcept {
        for(auto& x:hits)f(x);for(auto& x:misses)f(x);for(auto& x:api)f(x);for(auto& x:setter)f(x);
        for(auto& x:refresh)f(x);for(auto& x:refreshRoles)f(x);for(auto& x:publication)f(x);
        const auto reading=[&](Reading& r){f(r.calls);f(r.samples);f(r.ticks);f(r.max);f(r.clockFailed);};
        for(auto& x:costs)reading(x);for(auto& x:parts)reading(x);reading(currentSelf);
        f(lateObservedReuse);f(nestedSuppressed);f(excluded);f(refreshToken);f(publicationId);
    }
    void ClearTimings() noexcept {
        const auto clear=[](Reading& r){r.samples=0;r.ticks=0;r.max=0;};
        for(auto& x:costs)clear(x);for(auto& x:parts)clear(x);clear(currentSelf);
    }
    void Add(const Details& r) noexcept {
        for(unsigned i=0;i<hits.size();++i){hits[i]+=r.hits[i];misses[i]+=r.misses[i];}
        for(unsigned i=0;i<api.size();++i)api[i]+=r.api[i];
        for(unsigned i=0;i<setter.size();++i)setter[i]+=r.setter[i];
        for(unsigned i=0;i<refresh.size();++i)refresh[i]+=r.refresh[i];
        for(unsigned i=0;i<refreshRoles.size();++i)refreshRoles[i]+=r.refreshRoles[i];
        for(unsigned i=0;i<publication.size();++i)publication[i]+=r.publication[i];
        for(unsigned i=0;i<costs.size();++i)costs[i].Add(r.costs[i]);
        for(unsigned i=0;i<parts.size();++i)parts[i].Add(r.parts[i]);
        currentSelf.Add(r.currentSelf);lateObservedReuse+=r.lateObservedReuse;
        nestedSuppressed+=r.nestedSuppressed;excluded+=r.excluded;
        if(r.refreshToken)refreshToken=r.refreshToken;if(r.publicationId)publicationId=r.publicationId;
    }
};
inline constexpr unsigned DetailFields=2*Size<Consumer>+Size<Api>+Size<Setter>+Size<Refresh>+5+Size<Publication>+(Size<Cost>+Size<Part>+1)*5+5;
struct CostTicket {uint64_t frame=0,serial=0;int64_t begin=0;Root root=Root::Current;bool sampled=false,hit=false;};
struct PartTicket {uint64_t frame=0,serial=0;int64_t begin=0;Part part=Part::TryBinding;bool direct=false;};
CostTicket BeginCost(Root root,ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept;
void EndCost(const CostTicket& ticket) noexcept;
PartTicket BeginPart(Part part) noexcept;
void EndPart(const PartTicket& ticket) noexcept;
class CostScope final {
    CostTicket ticket_;
public:
    CostScope(Root root,ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept
        :ticket_(BeginCost(root,context,device,epoch)){}
    ~CostScope(){EndCost(ticket_);}
    void Hit() noexcept {ticket_.hit=true;}
    CostScope(const CostScope&)=delete;CostScope& operator=(const CostScope&)=delete;
};
class PartScope final {
    PartTicket ticket_;
public:
    explicit PartScope(Part part) noexcept:ticket_(BeginPart(part)){}
    ~PartScope(){EndPart(ticket_);}
    PartScope(const PartScope&)=delete;PartScope& operator=(const PartScope&)=delete;
};
void ApiCall(Api api,ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept;
void SetterResult(Setter result,ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept;
void RefreshEvent(Refresh reason,unsigned roleMask,uint64_t token,ID3D11DeviceContext* context,uint64_t epoch) noexcept;
void PublicationEvent(Publication result,uint64_t token,uint64_t publicationId,ID3D11DeviceContext* context,uint64_t epoch) noexcept;
void LateObservedReuse(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept;
}
