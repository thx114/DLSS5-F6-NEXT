// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinFamilySkip.h"
#include "GenshinPsStateCache.h"
#include "GenshinHookCpuTiming.h"
#include <atomic>
#include <mutex>
#include <limits>
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")
#include <wrl/client.h>

namespace GenshinLegacy::Family
{
namespace {
using CacheView=GenshinHookCpuTiming::ShaderCacheSnapshot;
// Observation identity only: these pointers are never dereferenced or retained.
// The capture owner supplies its existing locked Present proof, not the UI.
struct CacheMetricsLocal {
    const void* owner=nullptr;
    ID3D11DeviceContext* context=nullptr;
    ID3D11Device* device=nullptr;
    uint64_t deviceEpoch=0,configEpoch=0,ownerEpoch=0,bindingToken=0,revision=0,timingEpoch=0;
    uint64_t frameId=0,readSequence=0,beginMs=0,lastPublishMs=0;
    bool armed=false,invalid=false,timing=false,clockFailed=false,windowClockFailed=false;
    unsigned currentDepth=0,getterDepth=0,readDepth=0,excludedDepth=0;
    uint64_t costSerial=0,costChildren=0;
    std::array<uint64_t,3> costSequence{};
    unsigned partDepth=0;
    bool costActive=false,costSampled=false,costFailed=false;
    ShaderCacheMetrics::Root costRoot=ShaderCacheMetrics::Root::Current;
    std::array<ShaderCacheMetrics::Reading,ShaderCacheMetrics::Size<ShaderCacheMetrics::Part>> costParts{};
    CacheView frame{},window{};
};
thread_local CacheMetricsLocal cacheMetrics; // trivial TLS: no destructor/heap registration
std::atomic<uint64_t> cacheMetricsRevision{1};
bool MetricsFrameCurrent()noexcept {
    auto& m=cacheMetrics;
    if(!m.armed || m.invalid)return false;
    if(m.revision!=cacheMetricsRevision.load(std::memory_order_acquire) ||
       m.timingEpoch!=GenshinHookCpuTiming::monitorEpoch.load(std::memory_order_acquire) ||
       m.timing!=GenshinHookCpuTiming::Enabled()){
        m.invalid=true;return false;
    }
    return true;
}
// Every selected Read uses the same start point before the metadata lookup.
// Successful hits and complete getter fallbacks have separate populations.
// Nested Read/Current/getter callbacks never create a second overlapping sample.
class CacheReadMetric final {
    uint64_t frame_=0;
    int64_t begin_=0;
    bool sampled_=false,hit_=false,fallback_=false;
public:
    CacheReadMetric(ID3D11Device* device,uint64_t epoch,bool nested)noexcept {
        auto& m=cacheMetrics;
        if(nested || m.currentDepth>1 || m.getterDepth || m.excludedDepth || PsState::DrawScope::ReentrantForMetrics() ||
           !MetricsFrameCurrent())return;
        if(device!=m.device || epoch!=m.deviceEpoch)return;
        frame_=m.frameId;
        // A Read nested under Current shares that root's decision, not an independent sample.
        sampled_=m.costActive && m.costRoot==ShaderCacheMetrics::Root::Current?m.costSampled:
            (m.timing && (++m.readSequence&(GenshinHookCpuTiming::Stride-1))==1);
        if(sampled_)begin_=GenshinHookCpuTiming::Tick();
    }
    void Metadata(bool hit)noexcept {
        if(!frame_)return;
        auto& m=cacheMetrics;
        ++m.frame.values[hit?CacheView::MetadataHits:CacheView::MetadataMisses];
        hit_=hit;
    }
    void Fallback()noexcept {
        if(frame_){++cacheMetrics.frame.values[CacheView::FallbackReads];fallback_=true;}
    }
    ~CacheReadMetric() {
        // Stop before aggregating observer counters. QPC failure is never a
        // zero-duration sample; poison the window's costs, preserving counts.
        if(!sampled_ || (!hit_ && !fallback_))return;
        const auto end=GenshinHookCpuTiming::Tick();
        if(!frame_ || frame_!=cacheMetrics.frameId || !MetricsFrameCurrent())return;
        auto& m=cacheMetrics;
        if(begin_<=0 || end<begin_){m.clockFailed=true;return;}
        ++m.frame.values[hit_?CacheView::HitSamples:CacheView::MissSamples];
        m.frame.values[hit_?CacheView::HitTicks:CacheView::MissTicks]+=uint64_t(end-begin_);
    }
};
struct CacheCurrentMetric {
    ShaderCacheMetrics::CostScope cost;
    bool foreign=false,excluded=false;
    CacheCurrentMetric(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch)noexcept
        :cost(ShaderCacheMetrics::Root::Current,context,device,epoch) {
        auto& m=cacheMetrics;
        foreign=context!=m.context || device!=m.device || epoch!=m.deviceEpoch;
        excluded=foreign || m.currentDepth || m.getterDepth || m.readDepth || PsState::DrawScope::ReentrantForMetrics();
        if(excluded)++m.excludedDepth;
        ++m.currentDepth;
        if(foreign)++m.getterDepth; // exclude this context's nested Read too
    }
    ~CacheCurrentMetric(){auto& m=cacheMetrics;if(foreign)--m.getterDepth;if(excluded)--m.excludedDepth;--m.currentDepth;}
};
struct CacheGetterMetric {
    CacheGetterMetric()noexcept {++cacheMetrics.getterDepth;}
    ~CacheGetterMetric(){--cacheMetrics.getterDepth;}
};
// First successful CPU classification stays resident until an observed mark,
// owner revocation or bounded-slot eviction; there is no frame/time expiry.
// 2048 direct-mapped slots give the observed 585 marked shaders more headroom,
// but collisions/larger working sets can still evict and require a cold Read.
// The binding bounds the strong references and their lifetime, not GPU work.
constexpr size_t MetadataCapacity=2048;
struct CachedMetadata {
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    Microsoft::WRL::ComPtr<ID3D11Device> returnedOwner;
    Shader fact;
    ReadObservation observation;
    uint64_t generation=0;
};
struct MetadataSlot {
    CachedMetadata value;
    uint64_t generation=0;
    size_t writers=0;
};
size_t MetadataIndex(ID3D11PixelShader* shader)noexcept {
    auto key=reinterpret_cast<uintptr_t>(shader)>>4;
    key^=key>>9;key^=key>>17;
    return key&(MetadataCapacity-1);
}
}
struct CreationBinding {
    Microsoft::WRL::ComPtr<ID3D11Device> creator,returnedOwner;
    uint64_t epoch=0,token=0;
    // Every access is serialized by bindingMutex, including mark generations.
    uint64_t metadataSequence=0;
    bool metadataDisabled=false;
    std::array<MetadataSlot,MetadataCapacity> metadata;
};
namespace {
// Publication is weak: retiring the capture owner leaves no global device roots.
// There is exactly one capture owner, and no pointer-keyed device equivalence map.
std::mutex bindingMutex;
std::weak_ptr<CreationBinding> activeBinding;
struct MetadataTicket {
    std::shared_ptr<CreationBinding> binding;
    size_t index=0;
    uint64_t generation=0;
    bool publishable=false;
};
uint64_t NextMetadataGeneration(CreationBinding& binding)noexcept {
    if(binding.metadataDisabled || binding.metadataSequence==(std::numeric_limits<uint64_t>::max)()){
        binding.metadataDisabled=true;return 0;
    }
    return ++binding.metadataSequence;
}
// Clear before every mark attempt, even a conflicting/failed re-mark. Keeping
// the slot busy through StoreReturned also prevents reentrant reads from
// republishing a record while its mark is being decided. COM releases are
// outside bindingMutex; no per-draw allocation or unbounded search is needed.
class MetadataMutation final {
    std::shared_ptr<CreationBinding> binding_;
    size_t index_=0;
    bool counted_=false;
public:
    explicit MetadataMutation(ID3D11PixelShader* shader)noexcept {
        if(!shader)return;
        CachedMetadata retired;
        {
            std::lock_guard lock(bindingMutex);
            binding_=activeBinding.lock();if(!binding_)return;
            index_=MetadataIndex(shader);auto& slot=binding_->metadata[index_];
            retired=std::move(slot.value);
            slot.generation=NextMetadataGeneration(*binding_);
            if(slot.writers==(std::numeric_limits<size_t>::max)())
                binding_->metadataDisabled=true;
            else {++slot.writers;counted_=true;}
        }
    }
    ~MetadataMutation() {
        if(binding_ && counted_){std::lock_guard lock(bindingMutex);--binding_->metadata[index_].writers;}
    }
    MetadataMutation(const MetadataMutation&)=delete;
    MetadataMutation& operator=(const MetadataMutation&)=delete;
};
// The real context/PS getter remains the caller's job. A hit only reuses an
// already validated project record for these exact, strongly held endpoints.
// Contention/reentry/unknown owners use the existing complete Read below.
bool ReadMetadata(ID3D11Device* device,ID3D11PixelShader* shader,uint64_t epoch,
                  MetadataTicket& ticket,Shader& fact,ReadObservation& observation)noexcept {
    std::unique_lock lock(bindingMutex,std::try_to_lock);if(!lock.owns_lock())return false;
    ticket.binding=activeBinding.lock();
    if(!ticket.binding || ticket.binding->metadataDisabled ||
       ticket.binding->creator.Get()!=device || ticket.binding->epoch!=epoch)return false;
    ticket.index=MetadataIndex(shader);const auto& slot=ticket.binding->metadata[ticket.index];
    if(slot.writers)return false;
    ticket.generation=slot.generation;ticket.publishable=true;
    const auto& cached=slot.value;
    if(cached.shader.Get()!=shader || !cached.generation || cached.generation!=slot.generation ||
       !cached.fact.Known() || cached.fact.deviceEpoch!=epoch || !cached.returnedOwner ||
       (cached.returnedOwner.Get()!=device && cached.returnedOwner.Get()!=ticket.binding->returnedOwner.Get()))return false;
    fact=cached.fact;observation=cached.observation;
    observation.metadataCached=true;observation.metadataGeneration=cached.generation;
    return true;
}
uint64_t PublishMetadata(const MetadataTicket& ticket,ID3D11Device* device,ID3D11PixelShader* shader,
                         ID3D11Device* actual,const Shader& fact,const ReadObservation& observation)noexcept {
    if(!ticket.publishable || !ticket.binding || !fact.Known() || observation.state!=ReadState::Ready)return 0;
    // Acquire the real shader/device references before taking the publication
    // lock. A collision's previous COM references are released after unlocking.
    CachedMetadata next,retired;
    next.shader=shader;next.returnedOwner=actual;next.fact=fact;next.observation=observation;
    std::shared_ptr<CreationBinding> current;
    std::unique_lock lock(bindingMutex,std::try_to_lock);if(!lock.owns_lock())return 0;
    current=activeBinding.lock();
    if(current!=ticket.binding || current->metadataDisabled ||
       current->creator.Get()!=device || current->epoch!=fact.deviceEpoch ||
       (actual!=device && current->returnedOwner.Get()!=actual))return 0;
    auto& slot=current->metadata[ticket.index];
    if(slot.writers || slot.generation!=ticket.generation)return 0;
    next.generation=NextMetadataGeneration(*current);if(!next.generation)return 0;
    slot.generation=next.generation;
    const auto generation=next.generation;
    retired=std::move(slot.value);slot.value=std::move(next);
    return generation;
}
thread_local bool metadataReadActive=false;
struct MetadataReadScope {
    bool previous=metadataReadActive;
    bool excluded=false;
    MetadataReadScope()noexcept {
        auto& m=cacheMetrics;
        excluded=previous || m.getterDepth || m.currentDepth>1 || PsState::DrawScope::ReentrantForMetrics();
        if(excluded)++m.excludedDepth;
        ++m.readDepth;metadataReadActive=true;
    }
    ~MetadataReadScope(){auto& m=cacheMetrics;--m.readDepth;if(excluded)--m.excludedDepth;metadataReadActive=previous;}
};
struct StoredShader {
    uint64_t hash=0,bytes=0,deviceEpoch=0,identity=0;
    uintptr_t creator=0,returnedOwner=0,shaderObject=0;
    uint64_t bindingToken=0;
    std::array<unsigned char,32> digest{};
    uint64_t seal=0;
};
uint64_t Seal(const StoredShader& record)noexcept {
    return Hash(&record,offsetof(StoredShader,seal));
}
bool Valid(const StoredShader& record,UINT size)noexcept {
    return size==sizeof(record) && record.hash && record.bytes && record.deviceEpoch &&
        record.identity && record.creator && record.returnedOwner && record.shaderObject && record.seal==Seal(record);
}
bool ReturnedBindingCurrent(const StoredShader& record)noexcept {
    std::lock_guard lock(bindingMutex);
    const auto binding=activeBinding.lock();
    return binding && record.bindingToken && binding->token==record.bindingToken &&
        binding->epoch==record.deviceEpoch &&
        reinterpret_cast<uintptr_t>(binding->creator.Get())==record.creator &&
        reinterpret_cast<uintptr_t>(binding->returnedOwner.Get())==record.returnedOwner;
}
bool ByteDigest(const void* bytes,size_t size,std::array<unsigned char,32>& digest)noexcept {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    const auto result=BCryptHash(algorithm,nullptr,0,
        const_cast<PUCHAR>(static_cast<const unsigned char*>(bytes)),static_cast<ULONG>(size),
        digest.data(),static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm,0);
    return result>=0;
}
bool StoreReturned(ID3D11Device* creator,ID3D11Device* returnedOwner,ID3D11PixelShader* shader,
                   const void* bytes,size_t size,uint64_t deviceEpoch,uint64_t bindingToken=0)noexcept
{
    MetadataMutation mutation(shader);
    if(size>(std::numeric_limits<ULONG>::max)())return false;
    static std::atomic<uint64_t> sequence{1};
    StoredShader record{Hash(bytes,size),static_cast<uint64_t>(size),deviceEpoch,sequence.fetch_add(1)};
    record.creator=reinterpret_cast<uintptr_t>(creator);
    record.returnedOwner=reinterpret_cast<uintptr_t>(returnedOwner);
    record.bindingToken=bindingToken;record.shaderObject=reinterpret_cast<uintptr_t>(shader);
    if(!record.hash || !record.identity)return false;
    // Only the exact NR profile consumes the SHA digest. Ordinary bridge
    // shaders retain their actual object/device/epoch/FNV/size record without
    // opening BCrypt and digesting unrelated world shaders on creation.
    if(record.bytes==GenshinNr::ShaderProfile::Bytes && record.hash==GenshinNr::ShaderProfile::Fnv &&
        !ByteDigest(bytes,size,record.digest))return false;
    StoredShader previous;UINT previousSize=sizeof(previous);
    const auto hr=shader->GetPrivateData(ShaderKey,&previousSize,&previous);
    if(hr!=DXGI_ERROR_NOT_FOUND) {
        // D3D11 may return the same cached shader on an identical creation. Keep
        // its identity stable; a different payload/owner/epoch never replaces it.
        return SUCCEEDED(hr) && Valid(previous,previousSize) &&
            previous.creator==record.creator && previous.returnedOwner==record.returnedOwner &&
            previous.shaderObject==record.shaderObject && previous.deviceEpoch==record.deviceEpoch && previous.bindingToken==record.bindingToken &&
            previous.hash==record.hash && previous.bytes==record.bytes && previous.digest==record.digest;
    }
    record.seal=Seal(record);
    return SUCCEEDED(shader->SetPrivateData(ShaderKey,sizeof(record),&record));
}
}
namespace ShaderCacheMetrics {
namespace {
// Getter annotations identify only the existing direct leaf. Callbacks inside
// that getter/Read are excluded without adding a getter, lock or native proof.
bool Owned(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch,
    bool getterLeaf=false,bool readLeaf=false) noexcept {
    auto& m=cacheMetrics;if(!MetricsFrameCurrent())return false;
    if((context && context!=m.context) || (device && device!=m.device) || epoch!=m.deviceEpoch ||
       m.currentDepth>1 || m.excludedDepth ||
       (m.getterDepth && (!getterLeaf || m.getterDepth!=1)) ||
       (m.readDepth && (!readLeaf || m.readDepth!=1)) ||
       PsState::DrawScope::ReentrantForMetrics()) {++m.frame.detail.excluded;return false;}
    return true;
}
void Sample(Reading& out,uint64_t ticks) noexcept {++out.samples;out.ticks+=ticks;if(ticks>out.max)out.max=ticks;}
}
CostTicket BeginCost(Root root,ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept {
    CostTicket t;auto& m=cacheMetrics;if(!context || !device || !Owned(context,device,epoch))return t;
    if(m.costActive){++m.frame.detail.nestedSuppressed;return t;}
    if(++m.costSerial==0){m.invalid=true;return t;}
    t.frame=m.frameId;t.serial=m.costSerial;t.root=root;
    t.sampled=m.timing && (++m.costSequence[static_cast<unsigned>(root)]&(GenshinHookCpuTiming::Stride-1))==1;
    m.costActive=true;m.costSampled=t.sampled;m.costRoot=root;m.costFailed=false;m.costChildren=0;m.partDepth=0;
    if(root==Root::Current && t.sampled)m.costParts={};
    if(t.sampled)t.begin=GenshinHookCpuTiming::Tick();return t;
}
void EndCost(const CostTicket& t) noexcept {
    if(!t.frame)return;auto& m=cacheMetrics;
    const auto end=t.sampled?GenshinHookCpuTiming::Tick():0; // stop before observer aggregation
    if(!m.costActive || t.serial!=m.costSerial)return;
    m.costActive=false;
    if(t.frame!=m.frameId || !MetricsFrameCurrent())return;
    const auto kind=t.root==Root::Current?(t.hit?Cost::CurrentHit:Cost::CurrentMiss):
        (t.root==Root::Boundary?Cost::Boundary:Cost::Setter);
    auto& out=m.frame.detail.costs[static_cast<unsigned>(kind)];++out.calls;
    const bool valid=t.begin>0 && end>=t.begin && !m.costFailed && !m.partDepth &&
        m.costChildren<=uint64_t(end-t.begin);
    if(t.sampled){if(valid)Sample(out,uint64_t(end-t.begin));else {++out.clockFailed;m.clockFailed=true;}}
    if(t.root==Root::Current){
        auto& self=m.frame.detail.currentSelf;++self.calls;
        if(t.sampled)for(unsigned i=0;i<m.costParts.size();++i){auto part=m.costParts[i];
            part.calls=0; // actual part call counts were recorded at the leaf
            if(t.sampled && !valid){part.samples=0;part.ticks=0;part.max=0;}
            m.frame.detail.parts[i].Add(part);}
        if(t.sampled){if(valid)Sample(self,uint64_t(end-t.begin)-m.costChildren);else ++self.clockFailed;}
    }
}
PartTicket BeginPart(Part part) noexcept {
    PartTicket t;auto& m=cacheMetrics;
    if(!m.costActive || m.costRoot!=Root::Current || m.currentDepth!=1 || m.excludedDepth || m.partDepth ||
       PsState::DrawScope::ReentrantForMetrics() || !MetricsFrameCurrent())return t;
    t.frame=m.frameId;t.serial=m.costSerial;t.part=part;t.direct=true;++m.partDepth;
    ++m.frame.detail.parts[static_cast<unsigned>(part)].calls;
    if(m.costSampled)t.begin=GenshinHookCpuTiming::Tick();return t;
}
void EndPart(const PartTicket& t) noexcept {
    if(!t.direct)return;auto& m=cacheMetrics;const auto end=m.costSampled?GenshinHookCpuTiming::Tick():0;
    if(!m.costActive || t.serial!=m.costSerial)return;
    if(m.partDepth)--m.partDepth;
    if(!m.costSampled)return;
    if(t.begin<=0 || end<t.begin){m.costFailed=true;return;}
    const auto ticks=uint64_t(end-t.begin);m.costChildren+=ticks;
    Sample(m.costParts[static_cast<unsigned>(t.part)],ticks);
}
void ApiCall(Api api,ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept {
    const bool getterLeaf=api==Api::ContextDevice || api==Api::CurrentPs;
    if(Owned(context,device,epoch,getterLeaf,api==Api::ShaderDevice))++cacheMetrics.frame.detail.api[static_cast<unsigned>(api)];
}
void SetterResult(Setter result,ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept {
    if(Owned(context,device,epoch))++cacheMetrics.frame.detail.setter[static_cast<unsigned>(result)];
}
void RefreshEvent(Refresh reason,unsigned roleMask,uint64_t token,ID3D11DeviceContext* context,uint64_t epoch) noexcept {
    if(!Owned(context,nullptr,epoch))return;auto& d=cacheMetrics.frame.detail;
    ++d.refresh[static_cast<unsigned>(reason)];
    for(unsigned m=0;m<d.refreshRoles.size();++m)if(roleMask&(1u<<m))++d.refreshRoles[m];d.refreshToken=token;
}
void PublicationEvent(Publication result,uint64_t token,uint64_t id,ID3D11DeviceContext* context,uint64_t epoch) noexcept {
    if(!Owned(context,nullptr,epoch))return;auto& d=cacheMetrics.frame.detail;
    ++d.publication[static_cast<unsigned>(result)];d.refreshToken=token;d.publicationId=id;
}
void LateObservedReuse(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t epoch) noexcept {
    if(Owned(context,device,epoch))++cacheMetrics.frame.detail.lateObservedReuse;
}
uint64_t Draw(const void* owner,ID3D11DeviceContext* context,ID3D11Device* device,
    uint64_t deviceEpoch,uint64_t configEpoch)noexcept {
    auto& m=cacheMetrics;
    if(!MetricsFrameCurrent())return 0;
    if(owner!=m.owner || context!=m.context || device!=m.device ||
       deviceEpoch!=m.deviceEpoch || configEpoch!=m.configEpoch){m.invalid=true;return 0;}
    return m.frameId;
}
void BindingResult(uint64_t frame,Consumer consumer,bool hit,unsigned availability)noexcept {
    auto& m=cacheMetrics;
    if(!frame || frame!=m.frameId || metadataReadActive || m.currentDepth>1 || m.getterDepth || m.excludedDepth ||
       !MetricsFrameCurrent())return;
    const auto i=static_cast<unsigned>(consumer);if(i>=Size<Consumer>)return;
    ++(hit?m.frame.detail.hits:m.frame.detail.misses)[i];
    m.frame.values[CacheView::BindingState]=availability;
}
void Revoke()noexcept {
    cacheMetricsRevision.fetch_add(1,std::memory_order_acq_rel);
    cacheMetrics.armed=false;cacheMetrics.invalid=true;
    // Exceptional lifecycle publication only; an ordinary draw never publishes.
    GenshinHookCpuTiming::PublishShaderCache({});
}
void Controls(uint64_t configEpoch,bool enabled)noexcept {
    // ObserveConfig can run on a draw. Only invalidate TLS here; the existing
    // Present publishes the change (or the old snapshot ages out after 2s).
    if(cacheMetrics.armed && (!enabled || configEpoch!=cacheMetrics.configEpoch))cacheMetrics.invalid=true;
}
}
void CreationOwner::PresentShaderCacheMetrics(ID3D11DeviceContext* context,ID3D11Device* device,
    uint64_t deviceEpoch,uint64_t configEpoch,uint64_t ownerEpoch,bool current)const noexcept
{
    auto& m=cacheMetrics;
    const auto now=GetTickCount64();
    const auto epoch=GenshinHookCpuTiming::monitorEpoch.load(std::memory_order_acquire);
    const bool timing=GenshinHookCpuTiming::Enabled();
    const auto revision=cacheMetricsRevision.load(std::memory_order_acquire);
    const auto frequency=GenshinHookCpuTiming::frequency.load(std::memory_order_relaxed);
    // One existing-lock try at Present, never an extra draw lock. Do not call
    // PsState::ProofCurrent from this boundary or the UI: it mutates authority.
    bool inspected=false;unsigned metadataState=0;uint64_t token=0;
    std::shared_ptr<CreationBinding> active;
    if(current && context && device && deviceEpoch && configEpoch && ownerEpoch){
        std::unique_lock lock(bindingMutex,std::try_to_lock);
        if(lock.owns_lock()){
            inspected=true;active=activeBinding.lock();
            token=binding_?binding_->token:0;
            metadataState=active && active==binding_ && !active->metadataDisabled &&
                active->creator.Get()==device && active->epoch==deviceEpoch?2u:1u;
        }
    }
    const bool boundary=current && inspected && !metadataReadActive && !m.currentDepth && !m.getterDepth &&
        !PsState::DrawScope::ActiveForMetrics() && !m.costActive && !m.partDepth;
    const bool complete=boundary && MetricsFrameCurrent() && m.owner==this && m.context==context &&
        m.device==device && m.deviceEpoch==deviceEpoch && m.configEpoch==configEpoch &&
        m.ownerEpoch==ownerEpoch && m.bindingToken==token && m.revision==revision && m.timingEpoch==epoch && m.timing==timing;
    if(!boundary){
        const bool changed=m.armed;m.armed=false;m.invalid=false;m.frame={};m.window={};
        if(changed || now-m.lastPublishMs>=1000){
            CacheView view{};view.values[CacheView::EndMs]=now;
            view.values[CacheView::TimingEpoch]=epoch;
            GenshinHookCpuTiming::PublishShaderCache(view);m.lastPublishMs=now;
        }
        return;
    }
    if(!complete){
        // First/partial/changed owner frame cannot enter a per-frame denominator.
        m.owner=this;m.context=context;m.device=device;m.deviceEpoch=deviceEpoch;m.configEpoch=configEpoch;
        m.ownerEpoch=ownerEpoch;m.bindingToken=token;m.revision=revision;m.timingEpoch=epoch;m.timing=timing;
        m.armed=true;m.invalid=false;m.readSequence=0;m.beginMs=now;m.window={};m.windowClockFailed=false;
        CacheView view{};view.values[CacheView::EndMs]=now;view.values[CacheView::ThreadId]=GetCurrentThreadId();
        view.values[CacheView::TimingEpoch]=epoch;view.values[CacheView::MetadataState]=metadataState;
        GenshinHookCpuTiming::PublishShaderCache(view);m.lastPublishMs=now;
    }else {
        ++m.window.values[CacheView::Frames];
        // Explicit accumulation: states/epochs are never accidentally summed.
        for(auto i:{CacheView::MetadataHits,CacheView::MetadataMisses,CacheView::FallbackReads,
                    CacheView::HitSamples,CacheView::HitTicks,CacheView::MissSamples,CacheView::MissTicks})
            m.window.values[i]+=m.frame.values[i];
        m.window.detail.Add(m.frame.detail);
        m.windowClockFailed=m.windowClockFailed || m.clockFailed;
        if(now-m.beginMs>=1000){
            auto view=m.window;
            for(unsigned i=0;i<ShaderCacheMetrics::Size<ShaderCacheMetrics::Consumer>;++i){
                view.values[CacheView::BindingHits]+=view.detail.hits[i];view.values[CacheView::BindingMisses]+=view.detail.misses[i];}
            view.values[CacheView::BeginMs]=m.beginMs;view.values[CacheView::EndMs]=now;
            view.values[CacheView::ThreadId]=GetCurrentThreadId();view.values[CacheView::TimingEpoch]=epoch;
            view.values[CacheView::MetadataState]=metadataState;
            // Only this frame's actual TryCurrent result can report binding
            // eligibility. A resident Domain/known Shader alone is not evidence.
            view.values[CacheView::BindingState]=m.frame[CacheView::BindingState];
            if(timing && frequency>0 && !m.windowClockFailed)
                view.values[CacheView::QpcFrequency]=uint64_t(frequency);
            else {for(auto i:{CacheView::HitSamples,CacheView::HitTicks,CacheView::MissSamples,CacheView::MissTicks})view.values[i]=0;
                view.detail.ClearTimings();}
            // Lifecycle/control changes racing the boundary discard the whole
            // publication. No old-frame counts divided by a new-frame total.
            if(revision!=cacheMetricsRevision.load(std::memory_order_acquire) ||
               epoch!=GenshinHookCpuTiming::monitorEpoch.load(std::memory_order_acquire) ||
               timing!=GenshinHookCpuTiming::Enabled()){
                m.armed=false;GenshinHookCpuTiming::PublishShaderCache({});return;
            }
            GenshinHookCpuTiming::PublishShaderCache(view);m.lastPublishMs=now;
            m.window={};m.beginMs=now;m.windowClockFailed=false;
        }
    }
    m.frame={};m.clockFailed=false;
    if(++m.frameId==0){m.armed=false;ShaderCacheMetrics::Revoke();}
}
bool Record(ID3D11Device* device,ID3D11PixelShader* shader,const void* bytes,size_t size,uint64_t deviceEpoch)noexcept
{
    if(!device || !shader || !bytes || !size || !deviceEpoch){MetadataMutation refused(shader);return false;}
    Microsoft::WRL::ComPtr<ID3D11Device> actual;shader->GetDevice(&actual);
    if(actual.Get()!=device){MetadataMutation refused(shader);return false;}
    return StoreReturned(device,actual.Get(),shader,bytes,size,deviceEpoch);
}
bool CreationOwner::Bind(ID3D11Device* creator,uint64_t epoch)noexcept
{
    if(!creator || !epoch)return false;
    if(binding_)return binding_->creator.Get()==creator && binding_->epoch==epoch;
    try {
        auto binding=std::make_shared<CreationBinding>();
        static std::atomic<uint64_t> tokens{1};
        binding->creator=creator;binding->epoch=epoch;binding->token=tokens.fetch_add(1);
        if(!binding->token)return false;
        std::lock_guard lock(bindingMutex);
        if(!activeBinding.expired())return false;
        binding_=std::move(binding);activeBinding=binding_;return true;
    }catch(...){return false;}
}
// Destruction expires weak publication without touching another static object.
CreationOwner::~CreationOwner()=default;
void CreationOwner::Reset()noexcept
{
    ShaderCacheMetrics::Revoke();
    PsState::RetireOwner(this);
    // Revoking publication also makes every metadata ticket/cache unreachable.
    // The bounded shader/device references retire with this binding outside the
    // lock. In-flight CPU reads keep only their local binding lease; this does
    // not retire GPU resources or substitute for their existing owner gates.
    std::shared_ptr<CreationBinding> retired;
    {
        std::lock_guard lock(bindingMutex);
        const auto current=activeBinding.lock();
        if(current && current==binding_)activeBinding.reset();
        retired=std::move(binding_);
    }
}
bool CreationOwner::RecordReturned(ID3D11Device* creator,ID3D11PixelShader* shader,
                                   const void* bytes,size_t size)noexcept
{
    const auto binding=binding_; // caller serializes this owner with Reset/Bind
    if(!binding || !creator || creator!=binding->creator.Get() || !binding->epoch || !shader || !bytes || !size)
        {MetadataMutation refused(shader);return false;}
    Microsoft::WRL::ComPtr<ID3D11Device> actual;shader->GetDevice(&actual);
    if(!actual){MetadataMutation refused(shader);return false;}
    // Direct objects do not consume the separately witnessed returned domain.
    // Their CPU metadata cache still requires this current creator/epoch.
    if(actual.Get()==creator)return StoreReturned(creator,actual.Get(),shader,bytes,size,binding->epoch);
    if(binding->returnedOwner && binding->returnedOwner.Get()!=actual.Get()){MetadataMutation refused(shader);return false;}
    if(!StoreReturned(creator,actual.Get(),shader,bytes,size,binding->epoch,binding->token))return false;
    std::lock_guard lock(bindingMutex);
    const auto current=activeBinding.lock();
    if(current!=binding)return false;
    binding->returnedOwner=actual;return true;
}
Shader Read(ID3D11Device* device,ID3D11PixelShader* shader,uint64_t deviceEpoch)noexcept
{
    return Read(device,shader,deviceEpoch,nullptr);
}
Shader Read(ID3D11Device* device,ID3D11PixelShader* shader,uint64_t deviceEpoch,ReadObservation* observation)noexcept
{
    ReadObservation observed;observed.shader=reinterpret_cast<uintptr_t>(shader);
    const auto finish=[&](ReadState state,Shader result=Shader{}) {
        observed.state=state;if(observation)*observation=observed;return result;
    };
    if(!device || !shader || !deviceEpoch)return finish(ReadState::MissingOwner);
    MetadataReadScope metadataScope;
    CacheReadMetric metric(device,deviceEpoch,metadataScope.previous);
    MetadataTicket ticket;Shader cached;
    if(!metadataScope.previous){
        const bool hit=ReadMetadata(device,shader,deviceEpoch,ticket,cached,observed);
        metric.Metadata(hit);
        if(hit){if(observation)*observation=observed;return cached;}
    }
    metric.Fallback();
    Microsoft::WRL::ComPtr<ID3D11Device> actual;
    ShaderCacheMetrics::ApiCall(ShaderCacheMetrics::Api::ShaderDevice,nullptr,device,deviceEpoch);shader->GetDevice(&actual);
    observed.actualDevice=reinterpret_cast<uintptr_t>(actual.Get());
    if(!actual)return finish(ReadState::WrongShaderDevice);
    StoredShader record;UINT size=sizeof(record);
    observed.privateDataResult=shader->GetPrivateData(ShaderKey,&size,&record);
    if(FAILED(observed.privateDataResult))return finish(actual.Get()==device?ReadState::MissingRecord:ReadState::WrongShaderDevice);
    if(!Valid(record,size))return finish(ReadState::InvalidRecord);
    // The exact object must have been returned to this creator, and still report
    // the recorded owning device. A creator/child relation never aliases another
    // object, resource, context or trust domain. The epoch is checked independently.
    if(record.creator!=reinterpret_cast<uintptr_t>(device) || record.returnedOwner!=observed.actualDevice ||
       record.shaderObject!=reinterpret_cast<uintptr_t>(shader))
        return finish(ReadState::WrongShaderDevice);
    observed.recordedEpoch=record.deviceEpoch;
    if(record.deviceEpoch!=deviceEpoch)return finish(ReadState::WrongEpoch);
    if(actual.Get()!=device && !ReturnedBindingCurrent(record))return finish(ReadState::WrongShaderDevice);
    Shader result{record.hash,record.bytes,record.deviceEpoch,record.identity};
    if(record.bytes==GenshinNr::ShaderProfile::Bytes && record.hash==GenshinNr::ShaderProfile::Fnv &&
       record.digest==GenshinNr::ShaderProfile::Digest) {
        try {result.nrProfile=std::shared_ptr<const GenshinNr::ShaderProfile>(
            new GenshinNr::ShaderProfile(record.deviceEpoch,record.identity));}
        catch(...){return finish(ReadState::ProfileUnavailable);}
    }
    observed.state=ReadState::Ready;
    // Only a fully validated project mark may populate a miss. Generation and
    // current owner are rechecked after the real getter chain; an intervening
    // mark, collision, Reset or owner replacement cannot publish a stale fact.
    if(!metadataScope.previous)
        observed.metadataGeneration=PublishMetadata(ticket,device,shader,actual.Get(),result,observed);
    return finish(ReadState::Ready,std::move(result));
}
Shader Current(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t deviceEpoch,
               ReadObservation* observation)noexcept
{
    CacheCurrentMetric currentMetric(context,device,deviceEpoch);
    const auto refuse=[&](ReadState state,uintptr_t actual=0) {
        if(observation){*observation={};observation->state=state;observation->actualDevice=actual;}
        return Shader{};
    };
    if(!context || !device || !deviceEpoch)return refuse(ReadState::MissingOwner);
    Shader cached;
    {ShaderCacheMetrics::PartScope part(ShaderCacheMetrics::Part::TryBinding);
        if(PsState::DrawScope::TryCurrent(context,device,deviceEpoch,observation,cached)){currentMetric.cost.Hit();return cached;}}
    // A miss is still a real getter read, never counted as a binding hit.
    const auto bindingTicket=PsState::DrawScope::BeginRead(context,device,deviceEpoch);
    Microsoft::WRL::ComPtr<ID3D11Device> actual;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    {CacheGetterMetric getters;
        {ShaderCacheMetrics::PartScope part(ShaderCacheMetrics::Part::ContextDevice);
            ShaderCacheMetrics::ApiCall(ShaderCacheMetrics::Api::ContextDevice,context,device,deviceEpoch);context->GetDevice(&actual);}
        if(actual.Get()!=device)return refuse(ReadState::WrongContextDevice,reinterpret_cast<uintptr_t>(actual.Get()));
        {ShaderCacheMetrics::PartScope part(ShaderCacheMetrics::Part::PsGetter);
            ShaderCacheMetrics::ApiCall(ShaderCacheMetrics::Api::CurrentPs,context,device,deviceEpoch);context->PSGetShader(&shader,nullptr,nullptr);}}
    if(!shader)return refuse(ReadState::NoPixelShader,reinterpret_cast<uintptr_t>(actual.Get()));
    Shader fact;
    {ShaderCacheMetrics::PartScope part(ShaderCacheMetrics::Part::FallbackRead);fact=Read(device,shader.Get(),deviceEpoch,observation);}
    {ShaderCacheMetrics::PartScope part(ShaderCacheMetrics::Part::RememberReturned);
        PsState::DrawScope::RememberReturned(bindingTicket,context,device,deviceEpoch,shader.Get(),fact);}
    return fact;
}
}
