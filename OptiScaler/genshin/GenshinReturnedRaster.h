// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinNativeMrtContract.h"
#include "GenshinTypedGuides.h"
#include "GenshinDepthDraw.h"
#include "GenshinQueryScope.h"
#include "GenshinDepthSemantic.h"
#include "GenshinDepthReplayState.h"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <cstring>
#include <cmath>

// CPU-side returned API provenance. No GPU work, inferred Z convention,
// external matrix setter, native issuer, or semantic-admission override.
namespace GenshinReturnedRaster {
using Microsoft::WRL::ComPtr;
enum class PixelShaderPresence { Null, PresentUnqualified };
enum class Missing { FullShaderProfileAndSameCallConstants, DepthConvention, JitterPhase, MainCamera };
enum class Boundary { Copy, Update, Map, Unmap, Resolve, GenerateMips, Clear, Compute, State, Query, Present, Owner, OmittedDraw };
// Observation labels only. Never authorize writer coverage or skip invalidation.
enum class UntrackedSite : unsigned { Unknown, InternalBranch, BridgeHandled, UnsupportedHook };
enum class UntrackedRole : unsigned { Rtv=1, OmUav=2, WritableDsv=3 };
struct UntrackedCall {
    unsigned method=0,internalMask=0;
    UntrackedSite site=UntrackedSite::Unknown;
    unsigned action=0; // raw NativeDrawAction only at BridgeHandled
    uintptr_t caller=0; // original hook-entry return address, scalar observation only
};
inline const char* UntrackedMethodName(unsigned method)noexcept {
    switch(method){case 1:return "Draw";case 2:return "DrawIndexed";
    case 3:return "DrawInstanced";case 4:return "DrawIndexedInstanced";
    case 5:return "DrawAuto";case 6:return "DrawIndexedInstancedIndirect";
    case 7:return "DrawInstancedIndirect";default:return "Unknown";}
}
struct Epochs {uint64_t device=0,config=0;};
inline bool operator==(const Epochs& a,const Epochs& b)noexcept {return a.device==b.device && a.config==b.config;}
enum class Refusal:unsigned {None,DrawAuthority,DsvLayout,DepthWrite,Predication,GeometryStage,
    UnpairedTessellation,StageLinkage,MissingVertex,Topology,Viewport,Retention,RasterDepthBias,
    SemanticCapture,ReturnedState,MissingReturn,SemanticReturn,CoverageBudget,CoverageOverflow,
    WriteBoundary,OwnerLost,TrackEvicted,ClaimUnavailable,CopySourceMissing,CopyLayout,CopyPredication,CopyChainLimit,CoverageAllocation};
inline const char* RefusalText(unsigned value)noexcept {
    constexpr const char* names[]{"NONE","DRAW_AUTHORITY","DSV_LAYOUT","DEPTH_WRITE_STATE","PREDICATION",
        "GEOMETRY_STAGE_UNSUPPORTED","UNPAIRED_HS_DS","CLASS_LINKAGE_UNSUPPORTED","MISSING_VERTEX",
        "PATCH_TOPOLOGY","VIEWPORT_LAYOUT","BUFFER_RETENTION","RASTER_DEPTH_BIAS_OR_CLIP",
        "TERMINAL_SEMANTIC_CAPTURE","RETURNED_STATE_MISMATCH","MISSING_ORIGINAL_RETURN",
        "SAME_CALL_SEMANTIC_RETURN","COVERAGE_METADATA_BUDGET","COVERAGE_COUNTER_OVERFLOW",
        "CONTENT_WRITE_BOUNDARY","OWNER_LOST","TRACK_EVICTED","CLAIM_UNAVAILABLE",
        "COPY_SOURCE_CONTENT_UNAVAILABLE","COPY_RESOURCE_LAYOUT","COPY_PREDICATED","COPY_ANCESTRY_LIMIT",
        "COVERAGE_METADATA_ALLOCATION"};
    return value<std::size(names)?names[value]:"UNKNOWN_REFUSAL";
}
// Association failure is observational and independent of the sticky first
// content refusal. A failed claim never grants resource or GPU authority.
enum class ClaimFailure:unsigned {None,SourceUntracked,PendingBracket,ClearMissing,ReturnMissing,
    AlreadyClaimed,ReturnOrder,SourceLayout,ReadersBusy,Allocation};
struct Binding {
    ComPtr<ID3D11Buffer> buffer;UINT first=0,count=4096;
    bool Same(const Binding& b)const noexcept {return buffer.Get()==b.buffer.Get() && first==b.first && count==b.count;}
};
struct Raster {
    // Tokens precede owners; replacement assignment preserves release order.
    GenshinDepthRetention::Buffers<90> retention;
    Raster()=default;Raster(const Raster&)=default;Raster(Raster&&)noexcept=default;
    Raster& operator=(Raster other)noexcept {this->~Raster();new(this) Raster(std::move(other));return *this;}
    ComPtr<ID3D11DepthStencilView> view;
    ComPtr<ID3D11Resource> source;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11DepthStencilState> depth;ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11InputLayout> layout;
    std::array<ComPtr<ID3D11Buffer>,32> vertices{};
    std::array<UINT,32> strides{},offsets{};
    ComPtr<ID3D11Buffer> indices;DXGI_FORMAT indexFormat=DXGI_FORMAT_UNKNOWN;UINT indexOffset=0;
    PixelShaderPresence PixelShader()const noexcept {return ps?PixelShaderPresence::PresentUnqualified:PixelShaderPresence::Null;}
    std::array<Binding,56> constants{}; // VS, PS, HS, DS (14 each)
    GenshinDepthMapping::DrawArguments draw{};
    Epochs epochs{};
    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};D3D11_DEPTH_STENCIL_DESC depthDesc{};
    std::array<D3D11_VIEWPORT,16> viewports{};UINT viewportCount=16;
    std::array<D3D11_RECT,16> scissors{};UINT scissorCount=16,reference=0;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    bool queryQuiescent=false;Refusal refusal=Refusal::None;unsigned stageMask=0;
    std::shared_ptr<const GenshinDepthSemantic::Draw> semantic;
    GenshinDepthSemantic::CaptureFailureObservation captureFailure{};
    // These are actual CB bindings/ranges, NOT immutable numerical CB values.
    // No clip/projection ABI is qualified by object pointers or a FNV hash.
    bool Same(const Raster& b)const noexcept {
        if(view.Get()!=b.view.Get() || source.Get()!=b.source.Get() || vs.Get()!=b.vs.Get() || ps.Get()!=b.ps.Get() ||
           hs.Get()!=b.hs.Get() || ds.Get()!=b.ds.Get() ||
           depth.Get()!=b.depth.Get() || raster.Get()!=b.raster.Get() || layout.Get()!=b.layout.Get() ||
           indices.Get()!=b.indices.Get() || indexFormat!=b.indexFormat || indexOffset!=b.indexOffset ||
           strides!=b.strides || offsets!=b.offsets ||
           reference!=b.reference || topology!=b.topology || viewportCount!=b.viewportCount || scissorCount!=b.scissorCount ||
           std::memcmp(&viewDesc,&b.viewDesc,sizeof(viewDesc)) || std::memcmp(&depthDesc,&b.depthDesc,sizeof(depthDesc)) ||
           std::memcmp(viewports.data(),b.viewports.data(),viewportCount*sizeof(D3D11_VIEWPORT)) ||
           std::memcmp(scissors.data(),b.scissors.data(),scissorCount*sizeof(D3D11_RECT)))return false;
        const auto& a=draw;const auto& d=b.draw;
        if(a.context!=d.context || a.thread!=d.thread || a.kind!=d.kind || a.count!=d.count || a.start!=d.start ||
           a.base!=d.base || a.instances!=d.instances || a.instanceStart!=d.instanceStart ||
           a.indirectBuffer.Get()!=d.indirectBuffer.Get() || a.indirectIdentity.Get()!=d.indirectIdentity.Get() ||
           a.indirectOffset!=d.indirectOffset || a.indirectArgumentsUnknown!=d.indirectArgumentsUnknown ||
           a.observationCall!=d.observationCall || !(epochs==b.epochs))return false;
        for(unsigned i=0;i<constants.size();++i)if(!constants[i].Same(b.constants[i]))return false;
        for(unsigned i=0;i<vertices.size();++i)if(vertices[i].Get()!=b.vertices[i].Get())return false;
        return (!semantic && !b.semantic) || GenshinDepthSemantic::Same(semantic,b.semantic);
    }
};
struct Interval {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11DepthStencilView> clearView;
    D3D11_TEXTURE2D_DESC texture{};D3D11_DEPTH_STENCIL_VIEW_DESC clearDesc{};
    Epochs epochs{};DWORD thread=0;float clearValue=0;UINT clearFlags=0;UINT8 stencil=0;
    uint64_t clearSerial=0,returnedSerial=0,returnedDraws=0,version=0,loss=0;
    Raster last{};bool clearReturned=false,semanticComplete=true;
    ComPtr<ID3D11Resource> copiedFrom;uint64_t copySerial=0;unsigned copyHops=0;
    ComPtr<ID3D11VertexShader> firstUnprovedVs;ComPtr<ID3D11PixelShader> firstUnprovedPs;
    unsigned firstUnprovedReason=0;bool firstUnprovedQueryQuiescent=false;
    GenshinDepthSemantic::CaptureFailureObservation firstUnprovedCapture{};
    GenshinDepthSemantic::WriterGroups semanticWriters;
    uint64_t coveredWriters=0,unresolvedWriters=0,firstUnprovedCall=0;unsigned firstUnprovedStages=0;
    ComPtr<ID3D11Resource> origin;
    uint64_t resourceLifetime=0,originLifetime=0;
    unsigned firstBoundary=UINT_MAX;ClaimFailure claimFailure=ClaimFailure::None;
};
inline GenshinGuides::DepthSource Describe(const Interval& i)noexcept {
    GenshinGuides::DepthSource out;out.selected=reinterpret_cast<uintptr_t>(i.resource.Get());
    out.original=reinterpret_cast<uintptr_t>(i.origin?i.origin.Get():i.resource.Get());
    out.lifetime=i.resourceLifetime;out.originalLifetime=i.originLifetime;out.loss=i.loss;
    out.copiedFrom=reinterpret_cast<uintptr_t>(i.copiedFrom.Get());out.version=i.version;out.clear=i.clearSerial;
    out.returned=i.returnedSerial;out.total=i.returnedDraws;out.covered=i.coveredWriters;out.unresolved=i.unresolvedWriters;
    out.firstCall=i.firstUnprovedCall;out.refusal=i.firstUnprovedReason;out.stageMask=i.firstUnprovedStages;
    out.boundary=i.firstBoundary;out.claim=unsigned(i.claimFailure);
    if(i.firstUnprovedReason)out.operation="RasterWriter";
    out.mip=i.clearDesc.Texture2D.MipSlice;out.copyHops=i.copyHops;out.clearReturned=i.clearReturned;
    out.coverageComplete=i.semanticComplete && i.coveredWriters==i.returnedDraws && !i.unresolvedWriters;
    const auto& c=i.firstUnprovedCapture;out.captureReason=unsigned(c.reason);out.captureStage=c.stage;
    out.shader=c.shader;out.programFailure=c.programFailure;out.resourceSlot=c.resourceSlot;out.resourceType=c.resourceType;
    out.terminalInstruction=c.terminalInstruction;out.terminalOpcode=c.terminalOpcode;out.dependency=c.dependency;out.cbSlot=c.slot;out.cbRevision=c.revision;out.cbLoss=c.loss;out.cbState=c.state;
    return out;
}
inline void Unproved(Interval& i,Refusal reason,const Raster* raster=nullptr)noexcept {
    i.semanticComplete=false;
    // A missing writer permanently invalidates this clear interval. Partial
    // numerical proofs cannot be consumed again; release only this snapshot's
    // roots instead of pinning its payloads until another clear. Claimed/copy
    // snapshots and submitted Flight owners remain independently retained.
    i.semanticWriters={};
    if(i.firstUnprovedReason)return;
    i.firstUnprovedReason=unsigned(reason);
    if(raster){i.firstUnprovedVs=raster->vs;i.firstUnprovedPs=raster->ps;
        i.firstUnprovedCall=raster->draw.observationCall;i.firstUnprovedStages=raster->stageMask;
        i.firstUnprovedQueryQuiescent=raster->queryQuiescent;i.firstUnprovedCapture=raster->captureFailure;}
}
struct Token {
    uint64_t deviceEpoch=0,ownerEpoch=0,configEpoch=0,instance=0,generation=0,drawSerial=0,renderContext=0,tokenTick=0;
    uint32_t frame=0;UINT mip=0,srvFormat=0;
};
class Evidence;
namespace detail {
// NativeFrame stores the selected input as an ID3D11Texture2D interface
// (NativeFrame/ReadBindings), not an ID3D11Resource interface address. Resolve
// the original bound SRV and verify the same COM object before joining it to
// returned DSV content. No resource selection, writer or semantic fact is made.
inline ComPtr<ID3D11Resource> NativeDepthSource(const GenshinLegacy::NativeMrt::Frame& frame)noexcept {
    const auto& input=frame.inputs[1];
    if(!input.resource || !input.view)return {};
    auto* texture=reinterpret_cast<ID3D11Texture2D*>(input.resource);
    auto* view=reinterpret_cast<ID3D11ShaderResourceView*>(input.view);
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};view->GetDesc(&desc);
    if(desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || desc.Format!=input.viewFormat ||
       desc.Texture2D.MostDetailedMip!=input.firstMip || desc.Texture2D.MipLevels!=input.mipCount)return {};
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<IUnknown> expected,actual;
    if(!resource || FAILED(texture->QueryInterface(IID_PPV_ARGS(&expected))) ||
       FAILED(resource->QueryInterface(IID_PPV_ARGS(&actual))) || !expected ||
       expected.Get()!=actual.Get())return {};
    return resource;
}
struct Track {Interval interval{};uint64_t claimedClear=0;};
struct ClearPending {ComPtr<ID3D11DepthStencilView> view;ComPtr<ID3D11Resource> resource;float value=0;UINT flags=0;UINT8 stencil=0;uint64_t serial=0;};
// Fixed scalar history, including first observed retirement before a source is
// selected. Raw addresses are never dereferenced or treated as COM lifetime IDs.
struct UntrackedRecord {
    UntrackedCall call{};UntrackedRole role=UntrackedRole::Rtv;
    uintptr_t context=0,device=0;DWORD ownerThread=0,actualThread=0;
    Epochs epochs{};uint64_t serial=0,ownerLoss=0,actualLoss=0;
    uint64_t clear=0,returned=0,writers=0,version=0,intervalLoss=0;
    unsigned mip=0;bool clearReturned=false;
};
struct UntrackedSource {
    uintptr_t resource=0;UntrackedRecord first{};
    std::array<uint64_t,64> emitted{};std::array<uintptr_t,64> emittedCallers{};unsigned reports=0;
    bool watched=false,saturated=false;GenshinGuides::DepthSource content{};
};
struct State {
    std::mutex mutex;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Epochs epochs{};DWORD thread=0;uint64_t serial=0,loss=0;
    ComPtr<ID3D11Resource> observedCopySource; // bounded diagnostic location, never authority
    std::array<Track,4> tracks{};ClearPending clear{};Raster pending{};int pendingSlot=-1;
    bool indirectReturned=false;
    // Scalar diagnostics only: no COM retention, dereference or authority.
    uintptr_t traceResource=0;unsigned traceOwners=0,traceReports=0;
    const char* traceLast=nullptr;uint64_t traceDetail=UINT64_MAX;
    // Independent of the phase36 64-event budget. Repeated login signatures
    // cannot consume a newly watched world's source budget. Existing State lock.
    std::array<UntrackedSource,32> untrackedSources{};
    unsigned untrackedCursor=0;uint64_t untrackedEvictions=0,untrackedDropped=0;
    std::array<std::weak_ptr<const Evidence>,8> leases{};
};
inline State& Store(){static State state;return state;}
inline std::atomic<uint64_t> loss{1};
inline void Lose()noexcept {
    auto n=loss.load();while(n!=UINT64_MAX && !loss.compare_exchange_weak(n,n+1)){}
}
struct Access {
    State& s=Store();std::unique_lock<std::mutex> lock{s.mutex,std::defer_lock};
    Access()noexcept {try{lock.try_lock();}catch(...){}if(!lock.owns_lock())Lose();}
    explicit operator bool()const noexcept {return lock.owns_lock();}
};
// Called only with the existing State lock held. No Access/Owner/Lose,
// new lock, COM call, GPU work or lifetime extension is permitted here.
inline void NoteLifecycle(State& s,ID3D11Resource* resource,const char* event,
                          uint64_t detail=UINT64_MAX,const Interval* interval=nullptr)noexcept {
    if(!s.traceResource || s.traceReports>=64 ||
       (resource && reinterpret_cast<uintptr_t>(resource)!=s.traceResource))return;
    if(s.traceLast==event && s.traceDetail==detail)return;
    s.traceLast=event;s.traceDetail=detail;++s.traceReports;
    try{LOG_INFO("R827 phase36 source lifecycle: pid={} watch={} report={} source={} event={} detail={} serial={} owner_loss={} actual_loss={} context={} owner_thread={} actual_thread={} interval={} clear_returned={} clear={} returned={} writers={} hops={} observation_only=true",
        GetCurrentProcessId(),s.traceOwners,s.traceReports,s.traceResource,event,detail,s.serial,s.loss,loss.load(),
        reinterpret_cast<uintptr_t>(s.context.Get()),s.thread,GetCurrentThreadId(),interval!=nullptr,
        interval?interval->clearReturned:false,interval?interval->clearSerial:0,interval?interval->returnedSerial:0,
        interval?interval->returnedDraws:0,interval?interval->copyHops:0);}catch(...){}
}
inline uint64_t UntrackedSignature(const UntrackedCall& call,UntrackedRole role)noexcept {
    const auto method=call.method<=7?call.method:0;
    const auto site=static_cast<unsigned>(call.site)<=3?static_cast<unsigned>(call.site):0;
    return method | (uint64_t(role)<<3) | (uint64_t(call.internalMask&7)<<5) |
        (uint64_t(site)<<8) | (uint64_t(call.action)<<10);
}
inline bool SameUntrackedOwner(const UntrackedSource& source,const State& s)noexcept {
    return source.first.context==reinterpret_cast<uintptr_t>(s.context.Get()) &&
        source.first.device==reinterpret_cast<uintptr_t>(s.device.Get()) &&
        source.first.epochs==s.epochs && source.first.ownerThread==s.thread && source.first.ownerLoss==s.loss;
}
// A tombstone must match the same live resource allocation, not a later object
// reusing its address. Assigned at real clear/copy boundaries from the monotonic
// content serial; stored by D3D on that object and destroyed with that object.
// No per-draw getter/setter, new collector or strong texture lifetime root.
inline constexpr GUID ResourceLifetimeKey={0x59cf7841,0xa6bf,0x4adb,{0xb7,0x06,0x2a,0x71,0xa0,0xd8,0x14,0x36}};
inline uint64_t ResourceLifetime(ID3D11Resource* resource,uint64_t assign=0)noexcept {
    if(!resource)return 0;uint64_t value=0;UINT size=sizeof(value);
    if(SUCCEEDED(resource->GetPrivateData(ResourceLifetimeKey,&size,&value)) && size==sizeof(value) && value)return value;
    if(!assign || FAILED(resource->SetPrivateData(ResourceLifetimeKey,sizeof(assign),&assign)))return 0;
    return assign;
}
inline UntrackedSource* FindUntrackedSource(State& s,uintptr_t resource)noexcept {
    for(auto& source:s.untrackedSources)if(source.resource==resource && SameUntrackedOwner(source,s))return &source;
    return nullptr;
}
inline void EmitUntracked(State& s,UntrackedSource& source,const UntrackedRecord& r,bool firstCached)noexcept {
    const auto key=UntrackedSignature(r.call,r.role);
    for(unsigned i=0;i<source.reports;++i)if(source.emitted[i]==key && source.emittedCallers[i]==r.call.caller)return;
    if(source.reports>=source.emitted.size()){
        if(!source.saturated){source.saturated=true;try{LOG_INFO("R827 phase41 writer diagnostic saturated: source={} capacity={} observation_only=true",
            source.resource,source.emitted.size());}catch(...){}}return;
    }
    source.emitted[source.reports]=key;source.emittedCallers[source.reports++]=r.call.caller;
    try{LOG_INFO("R827 phase41 untracked writer: pid={} source={} report={} method={} method_name={} site={} internal_mask={} action={} role={} serial={} first_observed_serial={} cached_first={} context={} device={} owner_thread={} actual_thread={} device_epoch={} config_epoch={} owner_loss={} actual_loss={} clear={} returned={} writers={} version={} interval_loss={} mip={} clear_returned={} cache_evictions={} cache_dropped={} caller={} caller_known={} history_complete=false boundary=BEFORE_POSSIBLE_WRITE observation_only=true",
        GetCurrentProcessId(),source.resource,source.reports,r.call.method,UntrackedMethodName(r.call.method),
        static_cast<unsigned>(r.call.site),r.call.internalMask,r.call.action,static_cast<unsigned>(r.role),r.serial,source.first.serial,firstCached,
        r.context,r.device,r.ownerThread,r.actualThread,r.epochs.device,r.epochs.config,r.ownerLoss,r.actualLoss,
        r.clear,r.returned,r.writers,r.version,r.intervalLoss,r.mip,r.clearReturned,s.untrackedEvictions,s.untrackedDropped,r.call.caller,r.call.caller!=0);}catch(...){}
}
inline void NoteUntrackedRetirement(State& s,const Interval& interval,UntrackedCall call,UntrackedRole role)noexcept {
    const auto resource=reinterpret_cast<uintptr_t>(interval.resource.Get());if(!resource)return;
    UntrackedRecord r{};r.call=call;r.role=role;r.context=reinterpret_cast<uintptr_t>(s.context.Get());
    r.device=reinterpret_cast<uintptr_t>(s.device.Get());r.ownerThread=s.thread;r.actualThread=GetCurrentThreadId();
    r.epochs=s.epochs;r.serial=s.serial;r.ownerLoss=s.loss;r.actualLoss=loss.load();
    r.clear=interval.clearSerial;r.returned=interval.returnedSerial;r.writers=interval.returnedDraws;
    r.version=interval.version;r.intervalLoss=interval.loss;r.mip=interval.clearDesc.Texture2D.MipSlice;r.clearReturned=interval.clearReturned;
    auto* source=FindUntrackedSource(s,resource);
    if(!source){
        for(unsigned n=0;n<s.untrackedSources.size();++n){
            const auto index=(s.untrackedCursor+n)%s.untrackedSources.size();auto& candidate=s.untrackedSources[index];
            if(candidate.watched)continue;
            if(candidate.resource && s.untrackedEvictions!=UINT64_MAX)++s.untrackedEvictions;
            candidate={};candidate.resource=resource;candidate.first=r;source=&candidate;
            s.untrackedCursor=static_cast<unsigned>((index+1)%s.untrackedSources.size());break;
        }
    }
    if(!source){if(s.untrackedDropped!=UINT64_MAX)++s.untrackedDropped;return;}
    if(resource==s.traceResource)source->watched=true;
    if(source->watched)EmitUntracked(s,*source,r,false);
}
inline void WatchUntrackedSource(State& s,uintptr_t resource)noexcept {
    // A later selected-copy may identify a source whose first actual retirement
    // preceded the watch. Publish cached scalars only; never restore its track.
    if(auto* source=FindUntrackedSource(s,resource)){source->watched=true;EmitUntracked(s,*source,source->first,true);}
}
inline void WatchCopySource(State& s,ID3D11Resource* source)noexcept {
    const auto key=reinterpret_cast<uintptr_t>(source);
    if(!key || key==s.traceResource || s.traceOwners>=32)return;
    s.traceResource=key;++s.traceOwners;s.traceReports=0;s.traceLast=nullptr;s.traceDetail=UINT64_MAX;
    NoteLifecycle(s,source,"selected-copy-watch");
    WatchUntrackedSource(s,key);
}
// Reuse the existing bounded tombstone slots. Values have the real owner and
// content version; no pointer is dereferenced and no record can issue a token.
inline UntrackedSource* RememberContent(State& s,const GenshinGuides::DepthSource& content)noexcept {
    if(!content.selected)return nullptr;
    auto* slot=FindUntrackedSource(s,content.selected);
    if(!slot)for(unsigned n=0;n<s.untrackedSources.size();++n){
        auto& candidate=s.untrackedSources[(s.untrackedCursor+n)%s.untrackedSources.size()];
        if(candidate.watched)continue;candidate={};candidate.resource=content.selected;
        candidate.first.context=reinterpret_cast<uintptr_t>(s.context.Get());candidate.first.device=reinterpret_cast<uintptr_t>(s.device.Get());
        candidate.first.epochs=s.epochs;candidate.first.ownerThread=s.thread;candidate.first.ownerLoss=s.loss;slot=&candidate;
        s.untrackedCursor=unsigned((s.untrackedCursor+n+1)%s.untrackedSources.size());break;
    }
    if(slot)slot->content=content;
    return slot;
}
inline void NoteRetirement(State& s,const Interval& interval,const char* reason,
    Refusal cause=Refusal::WriteBoundary,Boundary operation=Boundary::State,const char* exactOperation=nullptr)noexcept {
    if(interval.resource){
        auto content=Describe(interval);content.coverageComplete=false;
        if(!content.refusal){content.refusal=unsigned(cause);content.boundary=unsigned(operation);content.operation=exactOperation?exactOperation:reason;}
        RememberContent(s,content);
    }
    if(interval.resource)NoteLifecycle(s,interval.resource.Get(),reason,UINT64_MAX,&interval);
    static unsigned count=0;if(!s.observedCopySource || interval.resource.Get()!=s.observedCopySource.Get() || count>=20)return;
    ++count;try{LOG_INFO("R811 source retirement: pid={} reason={} source={} serial={} clear={} returned={} writers={} semantic={} no_authority=true",
        GetCurrentProcessId(),reason,reinterpret_cast<uintptr_t>(interval.resource.Get()),s.serial,interval.clearSerial,interval.returnedSerial,interval.returnedDraws,interval.semanticComplete);}catch(...){}
}
// An interrupted indirect bracket must not preserve an earlier writer.
inline void DiscardPending(State& s)noexcept {
    if(s.pendingSlot>=0){auto& i=s.tracks[s.pendingSlot].interval;
        Unproved(i,Refusal::MissingReturn,&s.pending);i.returnedSerial=0;}
    s.pending={};s.pendingSlot=-1;s.indirectReturned=false;
}
inline void Reset(State& s)noexcept {NoteLifecycle(s,nullptr,"reset");DiscardPending(s);
    for(const auto& t:s.tracks)NoteRetirement(s,t.interval,"reset",Refusal::OwnerLost,Boundary::Owner);
    s.tracks={};s.clear={};}
inline uint64_t Next(State& s)noexcept {if(s.serial==UINT64_MAX){Reset(s);return 0;}return ++s.serial;}
inline bool Owner(State& s,ID3D11DeviceContext* c,Epochs e)noexcept {
    if(!c || !e.device || !e.config || loss.load()==UINT64_MAX){Reset(s);return false;}
    // A retained immediate context cannot change device/type. The actual owner
    // lock still protects epoch/thread/loss changes and all resource state.
    ComPtr<ID3D11Device> queried;auto* d=s.device.Get();
    if(s.context.Get()!=c || !d){
        if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){Reset(s);return false;}
        c->GetDevice(&queried);d=queried.Get();
    }
    if(!d || FAILED(d->GetDeviceRemovedReason())){Reset(s);return false;}
    const auto currentLoss=loss.load();
    if(s.context.Get()!=c || s.device.Get()!=d || !(s.epochs==e) || s.thread!=GetCurrentThreadId() || s.loss!=currentLoss){
        Reset(s);s.context=c;s.device=d;s.epochs=e;s.thread=GetCurrentThreadId();s.loss=currentLoss;
    }
    GenshinDepthSemantic::ObserveOwner(c,e.device,e.config);
    return true;
}
inline bool ClearView(ID3D11DepthStencilView* v,ComPtr<ID3D11Resource>& resource,
                      D3D11_DEPTH_STENCIL_VIEW_DESC& view,D3D11_TEXTURE2D_DESC& texture)noexcept {
    if(!v)return false;v->GetDesc(&view);v->GetResource(&resource);ComPtr<ID3D11Texture2D> t;
    if(!resource || view.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D ||
       FAILED(resource.As(&t)))return false;
    t->GetDesc(&texture);return texture.ArraySize==1 && texture.SampleDesc.Count==1 && !texture.SampleDesc.Quality &&
        view.Texture2D.MipSlice<texture.MipLevels && texture.Width && texture.Height;
}
inline int Find(State& s,ID3D11Resource* r,UINT mip)noexcept {
    for(unsigned i=0;i<s.tracks.size();++i)if(s.tracks[i].interval.resource.Get()==r && s.tracks[i].interval.clearDesc.Texture2D.MipSlice==mip)return static_cast<int>(i);
    return -1;
}
// Under the existing collector owner lock, limit only this draw's output
// binding queries. These are immutable descriptors of the retained resource,
// including the actual destination descriptor after a returned full copy.
// Typeless view aliases still require the same resource's RTV/UAV bind flags;
// no format-based or shader/count-based writer inference is made here.
inline UINT TrackedRasterOutputBindings(const State& s)noexcept {
    constexpr UINT all=D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
    UINT bindings=0;
    for(const auto& track:s.tracks)if(track.interval.resource){
        const auto& texture=track.interval.texture;
        // Unknown/special allocations retain the original identity scans. This
        // scope is never used for Copy/Update/Map/Resolve or deferred writes.
        if(!texture.Width || !texture.Height || !texture.MipLevels || !texture.ArraySize ||
           texture.Format==DXGI_FORMAT_UNKNOWN || !texture.SampleDesc.Count || texture.MiscFlags)return all;
        bindings|=texture.BindFlags;
    }
    return bindings&all;
}
// A bound DSV can be read without changing its depth plane. Preserve the last
// completed writer in that case; stencil operations do not write depth either.
inline bool DepthWritable(ID3D11DeviceContext* c,const D3D11_DEPTH_STENCIL_VIEW_DESC& view)noexcept {
    if(view.Flags&D3D11_DSV_READ_ONLY_DEPTH)return false;
    ComPtr<ID3D11DepthStencilState> state;UINT reference=0;c->OMGetDepthStencilState(&state,&reference);
    if(!state)return true; // D3D11 default: depth enabled, write all.
    D3D11_DEPTH_STENCIL_DESC desc{};state->GetDesc(&desc);
    return desc.DepthEnable && desc.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;
}
inline bool SnapshotCandidate(ID3D11DeviceContext* c,Raster& r,Epochs epochs,bool captureSemantic)noexcept {
    r={};r.draw=GenshinDepthMapping::CurrentDraw();r.epochs=epochs;
    const auto refuse=[&](Refusal reason){r.refusal=reason;return false;};
    const bool direct=r.draw.Matches(c,r.draw.kind,r.draw.count);
    // A direct instanced draw shares these exact bound shaders/CBs and raster
    // state for every instance. Retain its full original arguments without
    // relaxing the separate single-instance replay/mapping contract.
    const bool instanced=r.draw.context==c && r.draw.thread==GetCurrentThreadId() &&
        r.draw.count && r.draw.instances && (r.draw.kind==3 || r.draw.kind==4);
    bool indirect=false;
    const auto& draw=r.draw;
    if(draw.kind==6 && draw.indirectArgumentsUnknown && draw.context==c && draw.thread==GetCurrentThreadId() &&
       draw.observationCall && draw.indirectBuffer && draw.indirectIdentity && epochs.device && epochs.config &&
       !draw.count && !draw.start && !draw.base && !draw.instances && !draw.instanceStart && !(draw.indirectOffset&3u)){
        ComPtr<ID3D11Device> bufferDevice,contextDevice;
        draw.indirectBuffer->GetDevice(&bufferDevice);c->GetDevice(&contextDevice);
        D3D11_BUFFER_DESC desc{};draw.indirectBuffer->GetDesc(&desc);
        indirect=bufferDevice && bufferDevice.Get()==contextDevice.Get() &&
            (desc.MiscFlags&D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) && draw.indirectOffset<=desc.ByteWidth &&
            sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)<=desc.ByteWidth-draw.indirectOffset;
    }
    if(!direct && !instanced && !indirect)return refuse(Refusal::DrawAuthority);
    c->OMGetRenderTargets(0,nullptr,&r.view);
    D3D11_TEXTURE2D_DESC texture{};if(!ClearView(r.view.Get(),r.source,r.viewDesc,texture))return refuse(Refusal::DsvLayout);
    c->OMGetDepthStencilState(&r.depth,&r.reference);
    r.depthDesc.DepthEnable=TRUE;r.depthDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;r.depthDesc.DepthFunc=D3D11_COMPARISON_LESS;
    r.depthDesc.StencilReadMask=D3D11_DEFAULT_STENCIL_READ_MASK;r.depthDesc.StencilWriteMask=D3D11_DEFAULT_STENCIL_WRITE_MASK;
    r.depthDesc.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    r.depthDesc.BackFace=r.depthDesc.FrontFace;
    if(r.depth)r.depth->GetDesc(&r.depthDesc);
    if(!r.depthDesc.DepthEnable || r.depthDesc.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL ||
       (r.viewDesc.Flags&D3D11_DSV_READ_ONLY_DEPTH))return refuse(Refusal::DepthWrite);
    ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;c->GetPredication(&predicate,&value);if(predicate)return refuse(Refusal::Predication);
    ComPtr<ID3D11GeometryShader> gs;UINT hsClasses=0,dsClasses=0;
    c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&r.hs,nullptr,&hsClasses);c->DSGetShader(&r.ds,nullptr,&dsClasses);
    r.stageMask=(gs?4u:0u)|(r.hs?8u:0u)|(r.ds?16u:0u);
    if(gs)return refuse(Refusal::GeometryStage);
    if(bool(r.hs)!=bool(r.ds))return refuse(Refusal::UnpairedTessellation);
    if(hsClasses || dsClasses)return refuse(Refusal::StageLinkage);
    UINT classes=0;c->VSGetShader(&r.vs,nullptr,&classes);if(!r.vs)return refuse(Refusal::MissingVertex);
    if(classes)return refuse(Refusal::StageLinkage);r.stageMask|=1u;
    classes=0;c->PSGetShader(&r.ps,nullptr,&classes);if(classes)return refuse(Refusal::StageLinkage);if(r.ps)r.stageMask|=2u;
    c->IAGetInputLayout(&r.layout);c->IAGetPrimitiveTopology(&r.topology);c->RSGetState(&r.raster);
    const bool patch=r.topology>=D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && r.topology<=D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
    if(bool(r.ds)!=patch)return refuse(Refusal::Topology);
    std::array<ID3D11Buffer*,32> buffers{};
    c->IAGetVertexBuffers(0,32,buffers.data(),r.strides.data(),r.offsets.data());
    for(unsigned i=0;i<buffers.size();++i)r.vertices[i].Attach(buffers[i]);
    c->IAGetIndexBuffer(&r.indices,&r.indexFormat,&r.indexOffset);
    c->RSGetViewports(&r.viewportCount,r.viewports.data());c->RSGetScissorRects(&r.scissorCount,r.scissors.data());
    if(r.viewportCount>16 || r.scissorCount>16)return refuse(Refusal::Viewport);
    ComPtr<ID3D11DeviceContext1> c1;c->QueryInterface(IID_PPV_ARGS(&c1));
    // Batch getters, including tessellation only on this Genshin writer path.
    // VS-only retains the original two calls; other games never enter here.
    for(UINT stage=0;stage<(r.ds?4u:2u);++stage){
        std::array<ID3D11Buffer*,14> bound{};std::array<UINT,14> first{},count{};count.fill(4096);
        if(c1){switch(stage){
            case 0:c1->VSGetConstantBuffers1(0,14,bound.data(),first.data(),count.data());break;
            case 1:c1->PSGetConstantBuffers1(0,14,bound.data(),first.data(),count.data());break;
            case 2:c1->HSGetConstantBuffers1(0,14,bound.data(),first.data(),count.data());break;
            case 3:c1->DSGetConstantBuffers1(0,14,bound.data(),first.data(),count.data());break;
        }}else switch(stage){
            case 0:c->VSGetConstantBuffers(0,14,bound.data());break;case 1:c->PSGetConstantBuffers(0,14,bound.data());break;
            case 2:c->HSGetConstantBuffers(0,14,bound.data());break;case 3:c->DSGetConstantBuffers(0,14,bound.data());break;
        }
        for(UINT i=0;i<14;++i){auto& b=r.constants[stage*14+i];b.buffer.Attach(bound[i]);b.first=first[i];b.count=count[i];}
    }
    if(!r.retention.Add(r.draw.indirectBuffer.Get()) || !r.retention.Add(r.indices.Get()))return refuse(Refusal::Retention);
    for(const auto& b:r.vertices)if(!r.retention.Add(b.Get()))return refuse(Refusal::Retention);
    for(const auto& b:r.constants)if(!r.retention.Add(b.buffer.Get()))return refuse(Refusal::Retention);
    r.queryQuiescent=GenshinQueryScope::KnownQuiescent(c);
    D3D11_RASTERIZER_DESC rd{};rd.DepthClipEnable=TRUE;
    if(r.raster)r.raster->GetDesc(&rd);
    // One unknown writer already made this clear interval fail closed. Until
    // the next real clear, parsing further shaders cannot restore semantics;
    // retain their real raster state for mapping/lifetime without redoing it.
    if(captureSemantic && epochs.device && epochs.config && r.viewportCount==1 && rd.DepthBias==0 &&
       rd.DepthBiasClamp==0 && rd.SlopeScaledDepthBias==0 && rd.DepthClipEnable){
        std::array<GenshinDepthSemantic::Binding,42> bindings{};
        for(UINT stage=0;stage<(r.ds?3u:1u);++stage)for(UINT i=0;i<14;++i){
            const auto& b=r.constants[(stage==0?0:stage==1?28:42)+i];bindings[stage*14+i]={b.buffer.Get(),b.first,b.count};}
        r.semantic=GenshinDepthSemantic::Capture(c,r.vs.Get(),r.hs.Get(),r.ds.Get(),r.ps.Get(),r.topology,bindings,r.viewports[0],
            r.depthDesc.DepthFunc,epochs.device,epochs.config,&r.captureFailure);
        if(!r.semantic)r.refusal=Refusal::SemanticCapture;
    }else if(captureSemantic)r.refusal=Refusal::RasterDepthBias;
    return true;
}
inline bool Snapshot(ID3D11DeviceContext* c,Raster& r,Epochs epochs={},bool captureSemantic=true)noexcept {
    r={};Raster candidate;
    // Getter references exist only in this temporary during admission. Failure
    // releases every partial owner/charge, leaving no persistent candidate.
    if(!SnapshotCandidate(c,candidate,epochs,captureSemantic)){
        r.refusal=candidate.refusal;r.stageMask=candidate.stageMask;r.epochs=epochs;
        r.captureFailure=candidate.captureFailure;
        if(r.retention.Add(candidate.draw.indirectBuffer.Get()))r.draw=candidate.draw;
        else {
            // Keep the scalar first-call association even when no budget can
            // retain its argument buffer. Such an indirect call stays unproved.
            r.draw.context=candidate.draw.context;r.draw.thread=candidate.draw.thread;
            r.draw.kind=candidate.draw.kind;r.draw.observationCall=candidate.draw.observationCall;
        }
        return false;
    }
    r=std::move(candidate);return true;
}
}
// Immutable historical facts plus a current-content check. API return is not
// GPU completion, pixel coverage, depth-test success or a main-camera proof.
class Evidence final {
    Interval interval_;Token token_;
    Evidence(Interval i,Token t):interval_(std::move(i)),token_(t){}
public:
    static std::shared_ptr<const Evidence> Associate(ID3D11DeviceContext*,const GenshinLegacy::NativeMrt::Transaction&)noexcept;
    const Interval& Get()const noexcept {return interval_;}
    const Token& InputToken()const noexcept {return token_;}
    Missing FirstMissingSemantic()const noexcept {return Missing::FullShaderProfileAndSameCallConstants;}
    bool Current()const noexcept;
    bool Matches(const GenshinLegacy::NativeMrt::Frame& f)const noexcept {
        const auto& t=token_;const auto& i=interval_;const auto& d=f.inputs[1];
        if(f.thread!=i.thread || GetCurrentThreadId()!=i.thread)return false;
        const auto resource=detail::NativeDepthSource(f);if(!resource)return false;
        return f.context==reinterpret_cast<uintptr_t>(i.context.Get()) && f.device==reinterpret_cast<uintptr_t>(i.device.Get()) &&
            f.thread==i.thread && GetCurrentThreadId()==i.thread && f.deviceEpoch==t.deviceEpoch &&
            f.ownerEpoch==t.ownerEpoch && f.configEpoch==t.configEpoch && f.instance==t.instance &&
            f.generation==t.generation && f.drawSerial==t.drawSerial && f.renderContext==t.renderContext &&
            f.tokenTick==t.tokenTick && f.frame==t.frame && resource.Get()==i.resource.Get() &&
            d.firstMip==t.mip && d.viewFormat==t.srvFormat &&
            GenshinLegacy::NativeMrt::Fits(d,f.renderWidth,f.renderHeight);
    }
};
// All callbacks originate after real Genshin current-controls/device/thread
// entry checks. Tests call these same primitives on real D3D objects; they
// cannot issue a native token or populate a semantic Facts field.
inline void Invalidate(ID3D11DeviceContext* c,ID3D11Resource* written,Boundary boundary,const char* diagnosticOperation=nullptr)noexcept {
    if(GenshinDepthSemantic::Internal())return;
    GenshinDepthMapping::Deferred::Written(written,boundary==Boundary::Unmap);
    // The Unmap hook brackets numerical invalidation/capture itself.
    if(boundary!=Boundary::Unmap)GenshinDepthSemantic::Invalidate(c,written);
    detail::Access a;if(!a)return;auto& s=a.s;
    // Writes/deferred boundaries on another context can alias a retained source.
    // Revocation never adopts the caller as the producer owner.
    (void)c;
    detail::NoteLifecycle(s,written,diagnosticOperation?diagnosticOperation:"write-boundary",static_cast<unsigned>(boundary));
    s.clear={};detail::DiscardPending(s);
    for(auto& t:s.tracks)if(!written || t.interval.resource.Get()==written){
        detail::NoteRetirement(s,t.interval,"invalidate",Refusal::WriteBoundary,boundary,diagnosticOperation);t={};}
    static unsigned reports=0;if(s.observedCopySource && reports<16 && (!written || written==s.observedCopySource.Get())){++reports;
        LOG_INFO("R811 write boundary: pid={} boundary={} written={} thread={} no_authority=true",GetCurrentProcessId(),static_cast<unsigned>(boundary),reinterpret_cast<uintptr_t>(written),GetCurrentThreadId());}
    detail::Next(s);
    if(!c && !written){s.context.Reset();s.device.Reset();s.thread=0;s.epochs={};}
}
// A complete game CopyResource transports existing content, not a new clear,
// writer, camera, numerical interpretation or GPU completion. Capture only an
// existing same-owner returned interval after destination revocation; publish
// after the original API returns and only if that source/version is unchanged.
// Partial/resolve/deferred/predicated/shared/unknown copies retain revocation.
class CopySubmission final {
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Resource> destination_,source_;
    std::optional<Interval> content_;
    D3D11_TEXTURE2D_DESC destinationDesc_{};
    uint64_t serial_=0,loss_=0;Epochs epochs_{};
    std::optional<GenshinGuides::DepthSource> refusedCopy_;
public:
    CopySubmission()=default;
    CopySubmission(const CopySubmission&)=delete;
    CopySubmission& operator=(const CopySubmission&)=delete;
    CopySubmission(CopySubmission&&)=default;
    static CopySubmission Begin(ID3D11DeviceContext* c,ID3D11Resource* d,ID3D11Resource* r)noexcept {
        CopySubmission out;
        if(GenshinDepthSemantic::Internal() || !c || !d || !r)return out;
        detail::Access a;if(!a)return out;auto& s=a.s;
        if(s.context.Get()!=c || s.thread!=GetCurrentThreadId() || s.loss!=detail::loss.load() ||
           s.pendingSlot>=0 || s.clear.serial)return out;
        ComPtr<ID3D11Resource> dst,src;
        if(FAILED(d->QueryInterface(IID_PPV_ARGS(&dst))) || FAILED(r->QueryInterface(IID_PPV_ARGS(&src))) ||
           !dst || !src || dst.Get()==src.Get())return out;
        // Record only an observed source track/tombstone or the actual
        // selected destination. This is copy ancestry, never copied authority.
        GenshinGuides::DepthSource observed{};
        for(const auto& track:s.tracks)if(track.interval.resource.Get()==src.Get()){observed=Describe(track.interval);break;}
        if(!observed.selected)if(const auto* old=detail::FindUntrackedSource(s,reinterpret_cast<uintptr_t>(src.Get())))
            if(old->content.lifetime && old->content.lifetime==detail::ResourceLifetime(src.Get()))observed=old->content;
        const auto& expected=GenshinDepthEvidence::Expected();
        if(observed.selected || (expected.context.Get()==c && expected.resource.Get()==dst.Get())){
            if(!observed.selected){observed.selected=reinterpret_cast<uintptr_t>(src.Get());observed.original=observed.selected;
                observed.loss=s.loss;observed.lifetime=detail::ResourceLifetime(src.Get());observed.originalLifetime=observed.lifetime;}
            out.refusedCopy_=observed;
            out.context_=c;out.destination_=dst;out.source_=src;out.serial_=s.serial;out.loss_=s.loss;out.epochs_=s.epochs;
        }
        const auto refuse=[&](Refusal why){
            if(out.refusedCopy_ && !out.refusedCopy_->refusal){out.refusedCopy_->refusal=unsigned(why);out.refusedCopy_->boundary=unsigned(Boundary::Copy);out.refusedCopy_->operation="CopyResource";}
        };
        ComPtr<ID3D11Texture2D> dt,st;if(FAILED(dst.As(&dt)) || FAILED(src.As(&st))){refuse(Refusal::CopyLayout);return out;}
        D3D11_TEXTURE2D_DESC dd{},sd{};dt->GetDesc(&dd);st->GetDesc(&sd);
        if(dd.Width!=sd.Width || dd.Height!=sd.Height || dd.Format!=sd.Format || dd.MipLevels!=sd.MipLevels ||
           dd.ArraySize!=1 || sd.ArraySize!=1 || dd.SampleDesc.Count!=1 || sd.SampleDesc.Count!=1 ||
           dd.SampleDesc.Quality || sd.SampleDesc.Quality || dd.Usage!=D3D11_USAGE_DEFAULT || sd.Usage!=D3D11_USAGE_DEFAULT ||
           dd.CPUAccessFlags || sd.CPUAccessFlags || dd.MiscFlags || sd.MiscFlags){refuse(Refusal::CopyLayout);return out;}
        ComPtr<ID3D11Device> device,sourceDevice;dst->GetDevice(&device);src->GetDevice(&sourceDevice);
        if(device.Get()!=s.device.Get() || sourceDevice.Get()!=s.device.Get() || FAILED(device->GetDeviceRemovedReason())){refuse(Refusal::OwnerLost);return out;}
        ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;c->GetPredication(&predicate,&value);if(predicate){refuse(Refusal::CopyPredication);return out;}
        const auto& selected=GenshinDepthEvidence::Expected();
        if(selected.context.Get()==c && selected.resource.Get()==dst.Get())detail::WatchCopySource(s,src.Get());
        // ArraySize=1: each tracked mip identifies exactly one copied plane.
        for(const auto& track:s.tracks){const auto& i=track.interval;
            if(i.resource.Get()!=src.Get() || !i.clearReturned || !i.returnedSerial || !i.returnedDraws ||
               i.loss!=s.loss || i.copyHops>=8)continue;
            try{out.content_=i;}catch(...){return CopySubmission{};}
            s.observedCopySource=src;out.context_=c;out.destination_=dst;out.source_=src;out.destinationDesc_=dd;out.serial_=s.serial;break;
        }
        if(!out.content_){refuse(observed.copyHops>=8?Refusal::CopyChainLimit:Refusal::CopySourceMissing);
            const auto& expected=GenshinDepthEvidence::Expected();static unsigned missing=0;
            if(missing<12 && expected.context.Get()==c && expected.resource.Get()==dst.Get()){
                ++missing;unsigned occupied=0;for(const auto& t:s.tracks)occupied+=bool(t.interval.resource);
                LOG_INFO("R810 selected source absent: pid={} source={} destination={} tracked={} serial={} owner_loss={} actual_loss={} no_authority=true",
                    GetCurrentProcessId(),reinterpret_cast<uintptr_t>(src.Get()),reinterpret_cast<uintptr_t>(dst.Get()),occupied,s.serial,s.loss,detail::loss.load());
                for(unsigned slot=0;slot<s.tracks.size();++slot){const auto& i=s.tracks[slot].interval;
                    try{LOG_INFO("R827 phase36 refused source slot: pid={} source={} slot={} resource={} source_match={} clear_returned={} returned={} writers={} interval_loss={} owner_loss={} hops={} observation_only=true",
                        GetCurrentProcessId(),reinterpret_cast<uintptr_t>(src.Get()),slot,reinterpret_cast<uintptr_t>(i.resource.Get()),
                        i.resource.Get()==src.Get(),i.clearReturned,i.returnedSerial,i.returnedDraws,i.loss,s.loss,i.copyHops);}catch(...){}
                }
                detail::NoteLifecycle(s,src.Get(),"copy-source-refused");
            }
        }
        return out;
    }
    void Returned()noexcept {
        if(!content_){
            if(!refusedCopy_)return;
            detail::Access a;if(!a)return;auto& s=a.s;
            if(s.context.Get()!=context_.Get() || s.thread!=GetCurrentThreadId() || s.serial!=serial_ ||
               s.loss!=loss_ || s.loss!=detail::loss.load() || !(s.epochs==epochs_))return;
            auto observed=*refusedCopy_;refusedCopy_.reset();
            observed.copiedFrom=reinterpret_cast<uintptr_t>(source_.Get());observed.selected=reinterpret_cast<uintptr_t>(destination_.Get());
            observed.lifetime=detail::ResourceLifetime(destination_.Get(),s.serial);
            observed.version=s.serial;observed.coverageComplete=false;observed.claim=unsigned(ClaimFailure::SourceUntracked);
            if(observed.copyHops!=UINT_MAX)++observed.copyHops;
            detail::RememberContent(s,observed);return;
        }
        auto saved=std::move(content_);content_.reset();
        detail::Access a;if(!a)return;auto& s=a.s;const auto& before=*saved;
        if(s.context.Get()!=context_.Get() || s.thread!=GetCurrentThreadId() || s.serial!=serial_ ||
           s.loss!=detail::loss.load() || s.loss!=before.loss || !(s.epochs==before.epochs))return;
        const int sourceSlot=detail::Find(s,source_.Get(),before.clearDesc.Texture2D.MipSlice);
        if(sourceSlot<0)return;const auto& current=s.tracks[sourceSlot].interval;
        if(current.version!=before.version || current.returnedSerial!=before.returnedSerial ||
           current.clearSerial!=before.clearSerial || FAILED(s.device->GetDeviceRemovedReason()))return;
        int slot=detail::Find(s,destination_.Get(),before.clearDesc.Texture2D.MipSlice);
        if(slot<0)for(unsigned n=0;n<s.tracks.size();++n)if(!s.tracks[n].interval.resource){slot=static_cast<int>(n);break;}
        if(slot<0){
            for(unsigned n=0;n<s.tracks.size();++n)if(static_cast<int>(n)!=sourceSlot &&
                (slot<0 || s.tracks[n].interval.version<s.tracks[slot].interval.version))slot=static_cast<int>(n);
        }
        if(slot<0 || slot==sourceSlot)return;
        const auto version=detail::Next(s);if(!version)return;
        const auto sourceVersion=before.version,writerCount=before.returnedDraws;
        const auto semanticComplete=before.semanticComplete;const auto hops=before.copyHops+1;
        // Preserve the actual source clear and every actual writer unchanged.
        // Content identity/version refers to the copied destination plane.
        saved->resource=destination_;saved->texture=destinationDesc_;saved->version=version;
        saved->resourceLifetime=detail::ResourceLifetime(destination_.Get(),version);
        saved->copiedFrom=source_;saved->copySerial=version;++saved->copyHops;
        detail::NoteRetirement(s,s.tracks[slot].interval,"copy-cache-replace",Refusal::TrackEvicted,Boundary::Copy);
        s.tracks[slot]={};s.tracks[slot].interval=std::move(*saved);
        static unsigned reports=0;if(reports<8){++reports;
            LOG_INFO("R806 returned full depth copy: pid={} source={} destination={} source_version={} content_version={} writers={} semantic_complete={} copy_hops={} gpu_completion=UNKNOWN no_semantics_inferred=true",
                GetCurrentProcessId(),reinterpret_cast<uintptr_t>(source_.Get()),reinterpret_cast<uintptr_t>(destination_.Get()),
                sourceVersion,version,writerCount,semanticComplete,hops);
            const auto& published=s.tracks[slot].interval;
            const auto& failure=published.firstUnprovedCapture;
            LOG_INFO("R809 first missing writer: pid={} destination={} reason={} query_quiescent={} first_vs={} first_ps={} semantic_complete={} capture_reason={} capture_tick={} capture_call={} capture_thread={} draw_kind={} draw_count={} draw_instances={} draw_instance_start={} draw_indirect={} cb_slot={} cb_buffer={} cb_revision={} cb_first={} cb_constants={} cb_state={} no_authority=true",
                GetCurrentProcessId(),reinterpret_cast<uintptr_t>(destination_.Get()),published.firstUnprovedReason,published.firstUnprovedQueryQuiescent,
                reinterpret_cast<uintptr_t>(published.firstUnprovedVs.Get()),reinterpret_cast<uintptr_t>(published.firstUnprovedPs.Get()),published.semanticComplete,
                GenshinDepthSemantic::CaptureFailureText(failure.reason),failure.tick,failure.call,failure.thread,
                failure.kind,failure.count,failure.instances,failure.instanceStart,failure.indirect,
                failure.slot,failure.buffer,failure.revision,failure.first,failure.constants,failure.state);
            // Shader bytecode is retained for real semantic/replay consumers.
            // Automatic missing-writer diagnostics must not disassemble new
            // world shaders on the draw/copy thread. Explicit RuntimeCapture
            // owns shader export and disassembly on its existing worker.
        }
    }
};


// Binding-only API boundaries retire an unfinished bracket but do not write
// resource memory. Deferred work/discard/foreign owner still uses Invalidate.
inline void BindingChanged(ID3D11DeviceContext* c)noexcept {
    if(GenshinDepthSemantic::Internal())return;
    detail::Access a;if(!a)return;auto& s=a.s;if(!s.context)return;
    if(!c || c!=s.context.Get() || s.thread!=GetCurrentThreadId() || s.loss!=detail::loss.load() ||
       c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !s.device || FAILED(s.device->GetDeviceRemovedReason())){
        detail::Reset(s);detail::Next(s);GenshinDepthSemantic::Invalidate(c,nullptr);return;
    }
    s.clear={};detail::DiscardPending(s);detail::Next(s);
}
// Present does not write ordinary offscreen depth or constant buffers. Retire
// pending API brackets, while content remains versioned until an actual write,
// unknown/deferred boundary, owner/config change or device loss revokes it.
inline void Presented(ID3D11Device* device)noexcept {
    // Declared before Access: drain only after raster/semantic locks release,
    // including early returns. This bounded output never grants admission.
    struct Drain {ID3D11Device* device;~Drain(){GenshinDepthEvidence::DrainRefusedShader(device);}} drain{device};
    detail::Access a;if(!a)return;auto& s=a.s;
    if(!s.context)return;
    if(!device || device!=s.device.Get() || s.thread!=GetCurrentThreadId() ||
       s.loss!=detail::loss.load() || FAILED(device->GetDeviceRemovedReason())){
        detail::Reset(s);detail::Next(s);GenshinDepthSemantic::Invalidate(nullptr,nullptr);return;
    }
    s.clear={};detail::DiscardPending(s);detail::Next(s);
    GenshinDepthSemantic::Poll(s.context.Get());
}
// Unknown/omitted/internal raster cannot certify a writer. Its possible writes
// are the actual RTVs, OM UAVs and a writable DSV; unrelated source planes and
// exclusively CONSTANT_BUFFER resources cannot be modified by such a draw.
inline void BeforeUntrackedRaster(ID3D11DeviceContext* c,UntrackedCall diagnostic={})noexcept {
    GenshinDepthMapping::Deferred::Outputs(c,false);
    detail::Access a;if(!a)return;auto& s=a.s;
    s.clear={};detail::DiscardPending(s);
    bool tracked=false;for(const auto& track:s.tracks)tracked=tracked || bool(track.interval.resource);
    if(!tracked)return;
    if(!c || c!=s.context.Get() || s.thread!=GetCurrentThreadId() ||
       s.loss!=detail::loss.load() || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
       !s.device || FAILED(s.device->GetDeviceRemovedReason())){
        detail::Reset(s);detail::Next(s);GenshinDepthSemantic::Invalidate(c,nullptr);return;
    }
    const auto revoke=[&](ID3D11View* view,UntrackedRole role)noexcept {
        if(!view)return;ComPtr<ID3D11Resource> written;view->GetResource(&written);
        for(auto& track:s.tracks)if(track.interval.resource.Get()==written.Get()){detail::NoteUntrackedRetirement(s,track.interval,diagnostic,role);detail::NoteRetirement(s,track.interval,"untracked-raster",Refusal::WriteBoundary,Boundary::OmittedDraw);track={};}
    };
    std::array<ID3D11RenderTargetView*,8> rtvs{};ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(8,rtvs.data(),&depth);
    for(auto* view:rtvs)if(view){revoke(view,UntrackedRole::Rtv);view->Release();}
    std::array<ID3D11UnorderedAccessView*,64> uavs{};
    const UINT slots=s.device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
    c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,uavs.data());
    for(auto* view:uavs)if(view){revoke(view,UntrackedRole::OmUav);view->Release();}
    if(depth){D3D11_DEPTH_STENCIL_VIEW_DESC desc{};depth->GetDesc(&desc);
        if(detail::DepthWritable(c,desc))revoke(depth.Get(),UntrackedRole::WritableDsv);}
    detail::Next(s);
}


// Dispatch can only write bound CS UAVs. Reads from a tracked depth SRV and
// writes to unrelated UAVs must not erase its actual returned writer/clear.
// Deferred/unobservable contexts retain the existing conservative revocation.
inline void BeforeCompute(ID3D11DeviceContext* c)noexcept {
    GenshinDepthMapping::Deferred::Outputs(c,true);
    detail::Access a;if(!a)return;auto& s=a.s;
    s.clear={};detail::DiscardPending(s);
    bool tracked=false;for(const auto& track:s.tracks)tracked=tracked || bool(track.interval.resource);
    if(!tracked)return; // No D3D state reads on inactive/other-game dispatches.
    if(!c || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){detail::Reset(s);detail::Next(s);return;}
    ComPtr<ID3D11Device> device;c->GetDevice(&device);
    if(!device || FAILED(device->GetDeviceRemovedReason())){detail::Reset(s);detail::Next(s);return;}
    std::array<ID3D11UnorderedAccessView*,64> uavs{};
    const UINT count=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
    c->CSGetUnorderedAccessViews(0,count,uavs.data());
    for(auto* view:uavs)if(view){
        ComPtr<ID3D11Resource> written;view->GetResource(&written);
        for(auto& track:s.tracks)if(track.interval.resource.Get()==written.Get()){detail::NoteRetirement(s,track.interval,"compute",Refusal::WriteBoundary,Boundary::Compute);track={};}
        view->Release();
    }
    detail::Next(s);
}
inline void BeforeClear(ID3D11DeviceContext* c,Epochs e,ID3D11DepthStencilView* v,UINT flags,float depth,UINT8 stencil)noexcept {
    if(!(flags&D3D11_CLEAR_DEPTH))return;
    detail::Access a;if(!a)return;auto& s=a.s;if(!detail::Owner(s,c,e))return;
    s.clear={};detail::DiscardPending(s);
    ComPtr<ID3D11Resource> r;D3D11_DEPTH_STENCIL_VIEW_DESC vd{};D3D11_TEXTURE2D_DESC td{};
    if(!detail::ClearView(v,r,vd,td)){detail::Reset(s);return;}
    detail::NoteLifecycle(s,r.Get(),"before-clear",flags);
    for(auto& t:s.tracks)if(t.interval.resource.Get()==r.Get()){detail::NoteRetirement(s,t.interval,"before-clear-replace",Refusal::WriteBoundary,Boundary::Clear);t={};}
    if(!(flags&D3D11_CLEAR_DEPTH) || !std::isfinite(depth) || depth<0 || depth>1 || (vd.Flags&D3D11_DSV_READ_ONLY_DEPTH))return;
    ComPtr<ID3D11Device> d;r->GetDevice(&d);if(d.Get()!=s.device.Get())return;
    const auto serial=detail::Next(s);if(serial)s.clear={v,r,depth,flags,stencil,serial};
}
inline void ClearReturned(ID3D11DeviceContext* c,Epochs e,ID3D11DepthStencilView* v,UINT flags,float depth,UINT8 stencil)noexcept {
    if(!(flags&D3D11_CLEAR_DEPTH))return;
    detail::Access a;if(!a)return;auto& s=a.s;if(!detail::Owner(s,c,e))return;
    auto pending=std::move(s.clear);s.clear={};if(!pending.serial || pending.view.Get()!=v || pending.flags!=flags ||
       std::memcmp(&pending.value,&depth,sizeof(depth)) || pending.stencil!=stencil)return;
    ComPtr<ID3D11Resource> r;D3D11_DEPTH_STENCIL_VIEW_DESC vd{};D3D11_TEXTURE2D_DESC td{};
    if(!detail::ClearView(v,r,vd,td) || r.Get()!=pending.resource.Get())return;
    int slot=-1;for(unsigned i=0;i<s.tracks.size();++i)if(!s.tracks[i].interval.resource){slot=static_cast<int>(i);break;}
    // Bounded cache of CURRENT content, not the first four process-lifetime
    // allocations. Eviction drops authority; immutable evidence is never
    // relabelled and Current() rejects an evicted resource/version.
    if(slot<0){
        slot=0;
        for(unsigned i=1;i<s.tracks.size();++i)
            if(s.tracks[i].interval.version<s.tracks[slot].interval.version)slot=static_cast<int>(i);
        detail::NoteRetirement(s,s.tracks[slot].interval,"clear-cache-eviction",Refusal::TrackEvicted,Boundary::Clear);s.tracks[slot]={};
    }
    auto& out=s.tracks[slot].interval;
    out.device=s.device;out.context=c;out.resource=r;out.origin=r;out.clearView=v;out.texture=td;out.clearDesc=vd;
    out.epochs=e;out.thread=s.thread;out.clearValue=depth;out.clearFlags=flags;out.stencil=stencil;
    const auto version=detail::Next(s);if(!version)return;
    out.clearSerial=pending.serial;out.version=version;out.loss=s.loss;out.clearReturned=true;
    out.resourceLifetime=out.originLifetime=detail::ResourceLifetime(r.Get(),version);
    detail::NoteLifecycle(s,r.Get(),"clear-returned",flags,&out);
}
inline void BeforeRaster(ID3D11DeviceContext* c,Epochs e)noexcept {
    detail::Access a;if(!a)return;auto& s=a.s;if(!detail::Owner(s,c,e))return;
    detail::DiscardPending(s);s.clear={};
    if(std::none_of(s.tracks.begin(),s.tracks.end(),[](const detail::Track& t){return bool(t.interval.resource);}))return;
    // Actual RTV/UAV writes to a tracked plane are not certified DSV writers.
    // Resource binding capability is independent of capture/FG/NR demand.
    // These getters only retire tracks whose resource equals a bound output;
    // unmatched views feed no diagnostic, capture or consumer publication.
    // The DSV writer and its exact Before/After snapshot below are unchanged.
    const UINT outputBindings=detail::TrackedRasterOutputBindings(s);
    if(outputBindings&D3D11_BIND_RENDER_TARGET){
        std::array<ID3D11RenderTargetView*,8> rtvs{};c->OMGetRenderTargets(8,rtvs.data(),nullptr);
        for(auto* v:rtvs)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);for(auto& t:s.tracks)if(t.interval.resource.Get()==r.Get()){detail::NoteRetirement(s,t.interval,"raster-nondepth-output");t={};}v->Release();}
    }
    if(outputBindings&D3D11_BIND_UNORDERED_ACCESS){
        std::array<ID3D11UnorderedAccessView*,64> uavs{};
        const UINT slots=s.device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
        c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,uavs.data());
        for(auto* v:uavs)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);for(auto& t:s.tracks)if(t.interval.resource.Get()==r.Get()){detail::NoteRetirement(s,t.interval,"raster-nondepth-output");t={};}v->Release();}
    }
    ComPtr<ID3D11DepthStencilView> v;c->OMGetRenderTargets(0,nullptr,&v);ComPtr<ID3D11Resource> source;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};D3D11_TEXTURE2D_DESC td{};
    if(!detail::ClearView(v.Get(),source,vd,td)){
        if(v){v->GetResource(&source);detail::NoteLifecycle(s,source.Get(),"raster-dsv-layout-refused");for(auto& t:s.tracks)if(t.interval.resource.Get()==source.Get()){detail::NoteRetirement(s,t.interval,"raster-dsv-layout-refused",Refusal::DsvLayout);t={};}}
        return;
    }
    const int slot=detail::Find(s,source.Get(),vd.Texture2D.MipSlice);if(slot<0){detail::NoteLifecycle(s,source.Get(),"raster-source-untracked",vd.Format);return;}
    if(!detail::DepthWritable(c,vd))return;
    auto& t=s.tracks[slot];const auto version=detail::Next(s);if(!version)return;
    t.interval.version=version;t.interval.returnedSerial=0; // missing return cannot reuse an earlier draw
    const bool captured=detail::Snapshot(c,s.pending,e,t.interval.semanticComplete);
    if(!captured || s.pending.refusal!=Refusal::None)Unproved(t.interval,s.pending.refusal,&s.pending);
    s.pendingSlot=slot;
}
namespace detail {
inline bool SameIndirectCall(const State& s,ID3D11DeviceContext* c,ID3D11Buffer* buffer,UINT offset,uint64_t call)noexcept {
    const auto& draw=s.pending.draw;
    return call && s.pendingSlot>=0 && c && c==s.context.Get() && s.thread==GetCurrentThreadId() &&
        s.loss==loss.load() && s.device && SUCCEEDED(s.device->GetDeviceRemovedReason()) &&
        s.pending.epochs==s.epochs && draw.context==c && draw.thread==GetCurrentThreadId() &&
        draw.kind==6 && draw.indirectArgumentsUnknown && draw.observationCall==call &&
        draw.indirectIdentity && draw.indirectBuffer.Get()==buffer && draw.indirectOffset==offset;
}
}
inline bool PendingIndirect(ID3D11DeviceContext* c,ID3D11Buffer* buffer,UINT offset,uint64_t call)noexcept {
    detail::Access a;return a && detail::SameIndirectCall(a.s,c,buffer,offset,call);
}
// The legacy return path still rechecks current producer epochs and raster state.
inline void IndirectReturned(ID3D11DeviceContext* c,ID3D11Buffer* buffer,UINT offset,uint64_t call)noexcept {
    detail::Access a;if(!a)return;
    if(!detail::SameIndirectCall(a.s,c,buffer,offset,call) || a.s.indirectReturned){
        detail::DiscardPending(a.s);return;
    }
    a.s.indirectReturned=true;
}
inline void FinishIndirectObservation()noexcept {
    detail::Access a;if(a && a.s.pending.draw.indirectArgumentsUnknown)detail::DiscardPending(a.s);
}
inline void RasterReturned(ID3D11DeviceContext* c,Epochs e)noexcept {
    detail::Access a;if(!a)return;auto& s=a.s;if(!detail::Owner(s,c,e))return;
    const int slot=s.pendingSlot;s.pendingSlot=-1;if(slot<0)return;
    auto& out=s.tracks[slot].interval;Raster now{};
    const bool matchingCall=s.pending.draw.observationCall && s.pending.draw.observationCall==GenshinDepthMapping::CurrentDraw().observationCall;
    const bool snapshot=s.pending.refusal==Refusal::None || s.pending.refusal==Refusal::SemanticCapture || s.pending.refusal==Refusal::RasterDepthBias;
    const bool paired=matchingCall && (!s.pending.draw.indirectArgumentsUnknown || s.indirectReturned) && out.clearReturned &&
        (!snapshot || (detail::Snapshot(c,now,e,out.semanticComplete) && s.pending.Same(now)));
    if(!paired)Unproved(out,Refusal::ReturnedState,&s.pending);
    if(out.returnedDraws==UINT64_MAX){Unproved(out,Refusal::CoverageOverflow,&s.pending);out.returnedSerial=0;
        s.pending={};s.indirectReturned=false;return;}
    out.last=std::move(s.pending);s.pending={};s.indirectReturned=false;
    out.last.queryQuiescent=out.last.queryQuiescent && now.queryQuiescent;
    out.returnedSerial=detail::Next(s);if(!out.returnedSerial)return;
    ++out.returnedDraws;
    // Every real returned writer is counted, including the first rejected one
    // and all following writers. A later valid writer cannot revive this clear.
    if(!out.semanticComplete){++out.unresolvedWriters;return;}
    if(!out.last.semantic){Unproved(out,out.last.refusal==Refusal::None?Refusal::SemanticCapture:out.last.refusal,&out.last);++out.unresolvedWriters;return;}
    if(!GenshinDepthSemantic::Returned(c,out.last.semantic)){Unproved(out,Refusal::SemanticReturn,&out.last);++out.unresolvedWriters;return;}
    const auto appended=out.semanticWriters.Append(out.last.semantic);
    if(appended!=GenshinDepthSemantic::CoverageStatus::Stored){
        const auto reason=appended==GenshinDepthSemantic::CoverageStatus::Overflow?Refusal::CoverageOverflow:
            appended==GenshinDepthSemantic::CoverageStatus::Allocation?Refusal::CoverageAllocation:
            appended==GenshinDepthSemantic::CoverageStatus::InvalidProof?Refusal::SemanticReturn:Refusal::CoverageBudget;
        Unproved(out,reason,&out.last);++out.unresolvedWriters;return;
    }
    ++out.coveredWriters;

}
// Read-only inspection copies real recorded resource/clear/return facts. It is
// not token authority. Production TypedGuides only attaches Associate below.
// Diagnostic stamp only. Contention refuses WITHOUT changing collector loss,
// ownership or admission. Never held while acquiring the six-sample mutex.
inline std::array<uint64_t,2> ObservationStamp(ID3D11DeviceContext* c)noexcept {
    try{auto& s=detail::Store();std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock() || s.context.Get()!=c || s.thread!=GetCurrentThreadId() ||
           s.loss!=detail::loss.load() || s.loss==UINT64_MAX)return {};
        return {s.serial,s.loss};
    }catch(...){return {};}
}
inline std::optional<Interval> ObservationLatest(ID3D11DeviceContext* c,ID3D11Resource* r,UINT mip)noexcept {
    try{auto& s=detail::Store();std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock() || s.context.Get()!=c || s.thread!=GetCurrentThreadId() || s.loss!=detail::loss.load())return {};
        const int slot=detail::Find(s,r,mip);
        if(slot<0 || s.pendingSlot==slot || !s.tracks[slot].interval.returnedSerial)return {};
        return s.tracks[slot].interval;
    }catch(...){return {};}
}
inline std::optional<Interval> Latest(ID3D11DeviceContext* c,ID3D11Resource* r,UINT mip)noexcept {
    detail::Access a;if(!a)return {};auto& s=a.s;
    if(s.context.Get()!=c || s.thread!=GetCurrentThreadId() || s.loss!=detail::loss.load())return {};
    const int slot=detail::Find(s,r,mip);if(slot<0 || s.pendingSlot==slot || !s.tracks[slot].interval.returnedSerial)return {};
    return s.tracks[slot].interval;
}
inline bool Evidence::Current()const noexcept {
    detail::Access a;if(!a)return false;auto& s=a.s;
    if(s.context.Get()!=interval_.context.Get() || s.thread!=GetCurrentThreadId() || !(s.epochs==interval_.epochs) || s.loss!=detail::loss.load())return false;
    const int slot=detail::Find(s,interval_.resource.Get(),interval_.clearDesc.Texture2D.MipSlice);
    if(slot<0)return false;const auto& now=s.tracks[slot].interval;
    return now.clearSerial==interval_.clearSerial && now.returnedSerial==interval_.returnedSerial &&
        now.version==interval_.version && now.loss==interval_.loss;
}
inline GenshinGuides::DepthSource DescribeSelected(ID3D11DeviceContext* c,const GenshinLegacy::NativeMrt::Frame& f)noexcept {
    GenshinGuides::DepthSource out;const auto resource=detail::NativeDepthSource(f);if(!resource)return out;
    out.selected=reinterpret_cast<uintptr_t>(resource.Get());out.mip=f.inputs[1].firstMip;
    out.refusal=unsigned(Refusal::ClaimUnavailable);out.claim=unsigned(ClaimFailure::SourceUntracked);
    try{auto& s=detail::Store();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock() || s.context.Get()!=c || s.thread!=f.thread || !(s.epochs==Epochs{f.deviceEpoch,f.configEpoch}))return out;
        const auto slot=detail::Find(s,resource.Get(),out.mip);
        if(slot>=0)return Describe(s.tracks[slot].interval);
        if(const auto* source=detail::FindUntrackedSource(s,out.selected))if(source->content.selected && source->content.lifetime &&
           source->content.lifetime==detail::ResourceLifetime(resource.Get())){auto observed=source->content;
            if(!observed.claim)observed.claim=unsigned(ClaimFailure::SourceUntracked);return observed;}
    }catch(...){}
    return out;
}
inline std::shared_ptr<const Evidence> Evidence::Associate(ID3D11DeviceContext* c,const GenshinLegacy::NativeMrt::Transaction& native)noexcept {
    const auto* f=native.ClaimedFrame();
    if(!f || GenshinLegacy::NativeMrt::Validate(*f)!=GenshinLegacy::NativeMrt::Reason::Ready ||
       f->context!=reinterpret_cast<uintptr_t>(c) || f->thread!=GetCurrentThreadId())return {};
    const auto sourceResource=detail::NativeDepthSource(*f);if(!sourceResource)return {};
    detail::Access a;if(!a)return {};auto& s=a.s;
    if(s.context.Get()!=c || s.device.Get()!=reinterpret_cast<ID3D11Device*>(f->device) || s.thread!=f->thread ||
       !(s.epochs==Epochs{f->deviceEpoch,f->configEpoch}) || s.loss!=detail::loss.load())return {};
    const auto& source=f->inputs[1];const int slot=detail::Find(s,sourceResource.Get(),source.firstMip);
    if(slot<0)return {};auto& track=s.tracks[slot];auto& i=track.interval;
    const auto refuse=[&](ClaimFailure failure)->std::shared_ptr<const Evidence>{i.claimFailure=failure;return {};};
    if(s.pendingSlot>=0 || s.clear.serial)return refuse(ClaimFailure::PendingBracket);
    if(!i.clearReturned)return refuse(ClaimFailure::ClearMissing);
    if(!i.returnedSerial)return refuse(ClaimFailure::ReturnMissing);
    if(i.clearSerial<=track.claimedClear)return refuse(ClaimFailure::AlreadyClaimed);
    if(i.returnedSerial<=i.clearSerial)return refuse(ClaimFailure::ReturnOrder);
    if(!GenshinLegacy::NativeMrt::Fits(source,f->renderWidth,f->renderHeight) ||
       i.texture.Width!=source.desc.Width || i.texture.Height!=source.desc.Height ||
       i.texture.MipLevels!=source.desc.MipLevels || i.texture.Format!=source.desc.Format)return refuse(ClaimFailure::SourceLayout);
    std::weak_ptr<const Evidence>* lease=nullptr;for(auto& entry:s.leases)if(entry.expired()){lease=&entry;break;}
    if(!lease)return refuse(ClaimFailure::ReadersBusy);
    const Token token{f->deviceEpoch,f->ownerEpoch,f->configEpoch,f->instance,f->generation,f->drawSerial,f->renderContext,f->tokenTick,
                      f->frame,source.firstMip,source.viewFormat};
    try{i.claimFailure=ClaimFailure::None;auto result=std::shared_ptr<const Evidence>(new Evidence(i,token));*lease=result;track.claimedClear=i.clearSerial;return result;}
    catch(...){return refuse(ClaimFailure::Allocation);}
}
}
