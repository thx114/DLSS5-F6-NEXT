// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinInputObservation.h"
#include "GenshinDepthRetention.h"
#include "../runtime/CpuSnapshotBytes.h"
#include "../runtime/CpuChangedByteRanges.h"
#include "../runtime/MappedResourceJournal.h"
#include <d3d11_1.h>
#include <algorithm>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <malloc.h>
#include <cstring>
#include <cstdint>
#include <utility>

// CPU-only observations of public resource creation/upload/copy boundaries.
// No added GPU copy/readback, query-history repair, GIMI dependency or game
// pipeline state setter. Closed-device clones cannot feed this source ledger.
namespace GenshinDepthPortable {
using Microsoft::WRL::ComPtr;
inline std::atomic<bool> enabled{false};
// The closed picture lane brackets its SDK factory and child-object cloning.
// Existing hooks forward without adopting those private objects as game input.
inline thread_local unsigned privateCreation=0;
struct PrivateCreation {PrivateCreation(){++privateCreation;}~PrivateCreation(){--privateCreation;}};
inline std::atomic<uint64_t> epoch{1};
// Suppress only the portable notification made by this exact pre-Map
// invalidation. Replay/semantic/query revocation still executes unchanged;
// the public Map return decides whether CPU content was actually writable.
inline thread_local ID3D11Resource* mapInvalidation=nullptr;
struct MapInvalidation {
    ID3D11Resource* previous;
    explicit MapInvalidation(ID3D11Resource* resource):previous(mapInvalidation){mapInvalidation=resource;}
    ~MapInvalidation(){mapInvalidation=previous;}
};
enum class Cause : unsigned {Resident,NoCreationOrUpload,NoInitialBytes,CaptureFailed,Written,
    Evicted,OpaqueLoss,Mapped,Pending,Contended,RevisionExhausted,Disabled};
struct ReadStatus {
    Cause cause=Cause::NoCreationOrUpload;bool attempted=false;
    uint64_t revision=0,imageRevision=0,imageEpoch=0,sourceEpoch=0;
};
inline const char* Text(Cause cause)noexcept {
    switch(cause){case Cause::Resident:return "RESIDENT";case Cause::NoCreationOrUpload:return "NO_RETAINED_SOURCE_RECORD";
    case Cause::NoInitialBytes:return "NO_INITIAL_BYTES";case Cause::CaptureFailed:return "SOURCE_CAPTURE_FAILED";
    case Cause::Written:return "OBSERVED_WRITE_WITHOUT_PUBLICATION";case Cause::Evicted:return "COLD_SOURCE_EVICTED";
    case Cause::OpaqueLoss:return "UNKNOWN_SOURCE_WRITE_EPOCH";case Cause::Mapped:return "MAP_OPEN";
    case Cause::Pending:return "PUBLIC_API_RETURN_PENDING";case Cause::Contended:return "SOURCE_RECORD_CONTENDED";
    case Cause::RevisionExhausted:return "SOURCE_REVISION_EXHAUSTED";default:return "SOURCE_HOOK_DISABLED";}
}
inline constexpr GUID BufferKey={0x75872049,0x70bc,0x49f7,{0x9a,0x51,0xc8,0x81,0x7c,0x51,0x49,0x01}};
inline constexpr GUID LayoutKey={0x75872049,0x70bc,0x49f7,{0x9a,0x51,0xc8,0x81,0x7c,0x51,0x49,0x02}};
inline constexpr GUID HolderId={0x75872049,0x70bc,0x49f7,{0x9a,0x51,0xc8,0x81,0x7c,0x51,0x49,0x03}};
inline void Lost()noexcept {
    auto n=epoch.load();while(n!=UINT64_MAX&&!epoch.compare_exchange_weak(n,n+1)){}
}
inline bool CpuCopy(void* out,const void* in,size_t size)noexcept {
    if(!out||!in||!size)return false;
    __try{std::memcpy(out,in,size);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// A CPU version is a bounded interval map, not a zero-filled assertion about
// an entire allocation. Parts share immutable CPU storage across partial writes
// and buffer copies; only proven fetch ranges may be materialized for replay.
struct Storage {
    GenshinDepthRetention::Token charge;
    FramePipeline::CpuSnapshotBytes data;
    UINT size=0;
};
struct Part {
    std::shared_ptr<const Storage> storage;
    UINT begin=0,end=0,offset=0;
};
struct Image {
    GenshinDepthRetention::Token charge;
    D3D11_BUFFER_DESC desc{};
    std::vector<Part> parts; // sorted, non-overlapping, half-open byte intervals
    uint64_t epoch=0,revision=0;
    static constexpr size_t MaxParts=256;
    bool Covers(UINT begin,UINT end)const noexcept {
        if(begin>end||end>desc.ByteWidth)return false;
        const auto first=std::lower_bound(parts.begin(),parts.end(),begin,[](const Part& p,UINT value){return p.end<=value;});
        for(auto it=first;it!=parts.end();++it){const auto& p=*it;if(p.begin>begin)return false;
            begin=(std::min)(end,p.end);if(begin==end)return true;}
        return begin==end;
    }
    bool CopyTo(void* destination,UINT begin,UINT end)const noexcept {
        if(!destination||!Covers(begin,end))return false;
        auto* out=static_cast<unsigned char*>(destination);
        const auto first=std::lower_bound(parts.begin(),parts.end(),begin,[](const Part& p,UINT value){return p.end<=value;});
        for(auto it=first;it!=parts.end()&&it->begin<end;++it){const auto& p=*it;
            const auto b=(std::max)(begin,p.begin),e=(std::min)(end,p.end);
            if(b<e)std::memcpy(out+(b-begin),static_cast<const unsigned char*>(p.storage->data.data())+(p.offset+(b-p.begin)),e-b);}
        return true;
    }
};
struct Layout {
    GenshinDepthRetention::Token charge;
    std::array<D3D11_INPUT_ELEMENT_DESC,D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT> elements{};
    std::array<std::string,D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT> semantics;
    UINT count=0;
};
inline std::atomic<unsigned> activeReadMapCount{0};
// Object-private storage has no original-resource back reference or global
// strong-object cache. Retained Images are immutable even after later writes.
struct Record {
    GenshinDepthRetention::Token charge;
    std::mutex mutex;
    D3D11_BUFFER_DESC desc{};
    Cause cause=Cause::NoInitialBytes;
    std::shared_ptr<const Image> image;
    uint64_t revision=0,map=0,readSerial=0;unsigned pending=0;
    ID3D11DeviceContext* readContext=nullptr;DWORD readThread=0; // comparison only, no owning cycle
    bool exhausted=false;
    ~Record(){if(readContext)activeReadMapCount.fetch_sub(1);}
    std::atomic<uint64_t> touched{0};
    uint64_t Next()noexcept {
        if(exhausted||revision==UINT64_MAX){exhausted=true;return 0;}
        return ++revision;
    }
};
struct Holder final : IUnknown {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<Record> buffer;
    std::shared_ptr<const Layout> layout;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=IID_IUnknown&&id!=HolderId)return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return refs.fetch_add(1)+1;}
    ULONG STDMETHODCALLTYPE Release()override{const auto n=refs.fetch_sub(1)-1;if(!n)delete this;return n;}
};
inline ComPtr<Holder> Get(ID3D11DeviceChild* object,REFGUID key)noexcept {
    if(!object)return {};IUnknown* raw=nullptr;UINT size=sizeof(raw);
    if(FAILED(object->GetPrivateData(key,&size,&raw))||size!=sizeof(raw)||!raw)return {};
    ComPtr<IUnknown> owner;owner.Attach(raw);void* typed=nullptr;
    if(FAILED(owner->QueryInterface(HolderId,&typed)))return {};
    ComPtr<Holder> result;result.Attach(static_cast<Holder*>(static_cast<IUnknown*>(typed)));return result;
}
inline bool Supported(const D3D11_BUFFER_DESC& d)noexcept {
    constexpr UINT ia=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER;
    const bool geometry=(d.BindFlags&ia)&&!(d.BindFlags&~(ia|D3D11_BIND_SHADER_RESOURCE));
    const bool constant=d.BindFlags==D3D11_BIND_CONSTANT_BUFFER;
    const bool copySource=d.BindFlags==0;
    // A buffer with a possible unobserved GPU/external writer is never a CPU
    // source. No UAV/SO/shared/raw/structured/tiled alias is admitted here.
    return d.ByteWidth&&d.ByteWidth<=16u*1024*1024&&(geometry||constant||copySource)&&
        !d.MiscFlags&&!d.StructureByteStride&&
        ((d.Usage==D3D11_USAGE_IMMUTABLE&&!d.CPUAccessFlags)||
         (d.Usage==D3D11_USAGE_DEFAULT&&!d.CPUAccessFlags)||
         (d.Usage==D3D11_USAGE_DYNAMIC&&d.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE)||
         (copySource&&d.Usage==D3D11_USAGE_STAGING&&(d.CPUAccessFlags&D3D11_CPU_ACCESS_WRITE)&&
            !(d.CPUAccessFlags&~(D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE))));
}
// A tiny static IA input cannot be recreated after pressure eviction without
// another real application upload. Reserve its actual captured bytes until the
// resource is destroyed/written, independently of transient frame metadata.
inline bool Priority(const D3D11_BUFFER_DESC& d)noexcept {
    return d.ByteWidth<=4096&&(d.BindFlags&(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER))&&
        (d.Usage==D3D11_USAGE_DEFAULT||d.Usage==D3D11_USAGE_IMMUTABLE);
}
struct Records {std::mutex mutex;std::array<std::weak_ptr<Record>,4096> values;size_t cursor=0,sweep=0;};
inline Records& SourceRecords(){static Records value;return value;}
inline std::atomic<uint64_t> touchSerial{0},evictions{0},recordRefused{0};
inline bool Remember(const std::shared_ptr<Record>& record) {
    auto& records=SourceRecords();std::lock_guard lock(records.mutex);
    // Never overwrite a live weak entry: doing so used to orphan its image
    // from pressure accounting/reclamation while the resource still owned it.
    for(size_t n=0;n<records.values.size();++n){auto& slot=records.values[records.cursor++%records.values.size()];
        if(slot.expired()){record->touched.store(touchSerial.fetch_add(1));slot=record;return true;}}
    ++recordRefused;return false;
}
// At most 64 cold candidates per source admission failure, no full-scene
// three-pass scan. Never evict a priority source or frozen Raster/Map/Copy image.
inline void Trim()noexcept {
    auto& records=SourceRecords();std::unique_lock lock(records.mutex,std::try_to_lock);
    if(!lock.owns_lock())return;const auto now=touchSerial.load();
    for(unsigned n=0;n<64;++n)if(auto r=records.values[records.sweep++%records.values.size()].lock()){
        std::unique_lock recordLock(r->mutex,std::try_to_lock);
        if(recordLock.owns_lock()&&!Priority(r->desc)&&r->image&&r->image.use_count()==1&&!r->map&&!r->readContext&&!r->pending&&
           now-r->touched.load()>32){r->image.reset();r->cause=Cause::Evicted;++evictions;}
    }
}
inline GenshinDepthRetention::Token SourceCharge(uint64_t size,bool priority=false) {
    auto token=GenshinDepthRetention::Source(size,priority);
    if(!token){Trim();token=GenshinDepthRetention::Source(size,priority);}return token;
}
inline std::shared_ptr<Storage> Allocate(UINT size,bool priority=false) {
    if(!size||size>16u*1024*1024)return {};
    auto storage=std::make_shared<Storage>();storage->size=size;
    storage->charge=SourceCharge(uint64_t(size)+sizeof(Storage),priority);
    if(!storage->charge)return {};
    storage->data=FramePipeline::CpuSnapshotBytes(size,false);
    return storage;
}
inline std::shared_ptr<Image> NewImage(const D3D11_BUFFER_DESC& d,size_t partCapacity) {
    if(partCapacity>Image::MaxParts)return {};
    auto image=std::make_shared<Image>();image->desc=d;image->epoch=epoch.load();
    if(image->epoch==UINT64_MAX)return {};
    const auto metadata=sizeof(Image)+partCapacity*sizeof(Part);
    image->charge=SourceCharge(metadata,Priority(d));
    if(!image->charge)return {};image->parts.reserve(partCapacity);
    if(image->parts.capacity()>partCapacity)return {};return image;
}
inline size_t PartCount(const Image& image,UINT begin,UINT end)noexcept {
    size_t count=0;
    for(const auto& p:image.parts)if(p.begin<end&&p.end>begin)++count;
    return count;
}
inline void Clip(std::vector<Part>& out,const Image& image,UINT begin,UINT end,UINT destination) {
    for(const auto& p:image.parts){const UINT b=(std::max)(begin,p.begin),e=(std::min)(end,p.end);
        if(b<e){if(out.size()==Image::MaxParts)throw std::bad_alloc();
            out.push_back({p.storage,destination+b-begin,destination+e-begin,p.offset+b-p.begin});}}
}
inline std::shared_ptr<Image> Splice(const D3D11_BUFFER_DESC& d,const std::shared_ptr<const Image>& base,
    UINT begin,UINT end,const std::vector<Part>& replacement) {
    if(!Supported(d)||begin>end||end>d.ByteWidth)return {};
    const auto capacity=replacement.size()+(base?PartCount(*base,0,begin)+PartCount(*base,end,d.ByteWidth):0);
    auto image=NewImage(d,capacity);if(!image)return {};
    if(base)Clip(image->parts,*base,0,begin,0);
    if(replacement.size()>Image::MaxParts-image->parts.size())return {};
    image->parts.insert(image->parts.end(),replacement.begin(),replacement.end());
    if(base)Clip(image->parts,*base,end,d.ByteWidth,end);
    if(image->parts.size()>Image::MaxParts)return {};return image;
}
inline std::shared_ptr<Image> Freeze(const D3D11_BUFFER_DESC& d,const void* bytes,
    UINT begin=0,UINT end=UINT_MAX,const std::shared_ptr<const Image>& base={}) {
    if(end==UINT_MAX)end=d.ByteWidth;
    if(!Supported(d)||!bytes||begin>=end||end>d.ByteWidth)return {};
    auto storage=Allocate(end-begin,Priority(d));if(!storage||!CpuCopy(storage->data.data(),bytes,end-begin))return {};
    return Splice(d,base,begin,end,{{storage,begin,end,0}});
}
inline bool SameDevice(ID3D11DeviceContext* context,ID3D11DeviceChild* resource)noexcept {
    ComPtr<ID3D11Device> a,b;ComPtr<IUnknown> ai,bi;
    if(!context||!resource||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    context->GetDevice(&a);resource->GetDevice(&b);
    return a&&b&&SUCCEEDED(a.As(&ai))&&SUCCEEDED(b.As(&bi))&&ai==bi&&SUCCEEDED(a->GetDeviceRemovedReason());
}
inline std::shared_ptr<Record> Buffer(ID3D11Resource* resource,bool create=false) {
    ComPtr<ID3D11Buffer> buffer;if(!resource||FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))return {};
    if(auto h=Get(buffer.Get(),BufferKey))return h->buffer;
    if(!create||privateCreation)return {};
    D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);if(!Supported(d))return {};
    // Creation/first full upload is serialized by the resource's immediate
    // context. A concurrent non-owning writer cannot publish an old revision.
    auto r=std::make_shared<Record>();r->desc=d;
    r->charge=SourceCharge(sizeof(Record)+sizeof(Holder),Priority(d));
    // Protected small IA sources need no eviction index slot. Their private
    // Record/Image/Storage are byte-bounded by Source(), so a full ordinary
    // cache cannot discard a late-created fullscreen index buffer.
    if(!r->charge||(!Priority(d)&&!Remember(r)))return {};
    ComPtr<Holder> h;h.Attach(new Holder);h->buffer=r;
    if(FAILED(buffer->SetPrivateDataInterface(BufferKey,h.Get())))return {};
    return r;
}
inline void Created(ID3D11Buffer* resource,const D3D11_SUBRESOURCE_DATA* data)noexcept {
    if(!enabled.load()||privateCreation||!resource)return;
    try{auto r=Buffer(resource,true);if(!r)return;
        auto image=data?Freeze(r->desc,data->pSysMem):nullptr;
        std::lock_guard lock(r->mutex);const auto revision=r->Next();
        r->image.reset();r->cause=data?Cause::CaptureFailed:Cause::NoInitialBytes;
        if(image&&revision){image->revision=revision;r->image=std::move(image);r->cause=Cause::Resident;r->touched.store(touchSerial.fetch_add(1));}
    }catch(...){ } // Failed optional capture cannot revoke other resources' bytes.
}
inline void LayoutCreated(ID3D11InputLayout* resource,const D3D11_INPUT_ELEMENT_DESC* elements,
                          UINT count,const void* bytes,SIZE_T size)noexcept {
    if(!enabled.load()||privateCreation||!resource||!elements||!count||count>D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT||
       !bytes||!size||size>1024*1024)return;
    try{auto value=std::make_shared<Layout>();value->count=count;
        // The returned public layout plus copied descriptors is the source.
        // Replay uses the ACTUAL retained VS bytecode, never this creation's
        // unused full shader blob (previously charged/copied for every layout).
        value->charge=SourceCharge(sizeof(Layout)+count*257);
        if(!value->charge)return;
        for(UINT i=0;i<count;++i){
            D3D11_INPUT_ELEMENT_DESC d{};if(!CpuCopy(&d,elements+i,sizeof(d))||!d.SemanticName||
                d.InputSlot>=D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)return;
            // Copy caller-owned strings now; no borrowed SemanticName survives.
            char name[257]{};bool ended=false;
            for(unsigned j=0;j<256;++j){if(!CpuCopy(name+j,d.SemanticName+j,1))return;if(!name[j]){ended=true;break;}}
            if(!ended||!name[0])return;
            value->semantics[i]=name;value->elements[i]=d;
            value->elements[i].SemanticName=value->semantics[i].c_str();
        }
        ComPtr<Holder> h;h.Attach(new Holder);h->layout=std::move(value);
        resource->SetPrivateDataInterface(LayoutKey,h.Get());
    }catch(...){ }
}
inline std::shared_ptr<const Layout> ReadLayout(ID3D11InputLayout* resource)noexcept {
    auto h=Get(resource,LayoutKey);return h?h->layout:nullptr;
}
// Return the same invalidated record to the mapping adapter; descriptor and
// revision handling need not rediscover it through COM private data.
inline std::shared_ptr<Record> Written(ID3D11Resource* resource,bool unmap=false)noexcept {
    if(!enabled.load())return {};if(!resource){Lost();return {};}
    if(resource==mapInvalidation)return {};
    try{auto r=Buffer(resource);if(!r)return {};
        std::unique_lock lock(r->mutex,std::try_to_lock);if(!lock.owns_lock()){Lost();return {};}
        if(unmap&&r->readContext)return r; // exact read-only Unmap commits below
        r->image.reset();r->cause=Cause::Written;r->Next();
        if(!unmap){r->map=0;if(r->readContext)activeReadMapCount.fetch_sub(1);r->readContext=nullptr;r->readThread=0;}
        return r;
    }catch(...){Lost();return {};}
}
inline std::shared_ptr<const Image> Read(ID3D11Buffer* buffer,ReadStatus* status=nullptr)noexcept {
    ReadStatus observed;observed.attempted=true;observed.sourceEpoch=epoch.load();
    const auto finish=[&](Cause cause,std::shared_ptr<const Image> image={}) {
        observed.cause=cause;if(status)*status=observed;return image;
    };
    if(!enabled.load())return finish(Cause::Disabled);
    try{auto r=Buffer(buffer);if(!r)return finish(Cause::NoCreationOrUpload);
        std::unique_lock lock(r->mutex,std::try_to_lock);
        if(!lock.owns_lock())return finish(Cause::Contended);
        observed.sourceEpoch=epoch.load();observed.revision=r->revision;
        if(r->image){observed.imageRevision=r->image->revision;observed.imageEpoch=r->image->epoch;}
        if(r->map||r->readContext)return finish(Cause::Mapped);if(r->pending)return finish(Cause::Pending);
        if(r->exhausted)return finish(Cause::RevisionExhausted);
        if(!r->image)return finish(r->cause);
        if(r->image->epoch!=observed.sourceEpoch&&r->desc.Usage!=D3D11_USAGE_IMMUTABLE)return finish(Cause::OpaqueLoss);
        r->touched.store(touchSerial.fetch_add(1));return finish(Cause::Resident,r->image);
    }catch(...){return finish(Cause::CaptureFailed);}
}
// A preimage is read BEFORE the generic write boundary revokes the resource.
// It is reusable only across that exact one revision increment; reentry, loss,
// another writer or pending/mapped state prevents inheritance of old bytes.
inline std::shared_ptr<const Image> Before(ID3D11DeviceContext* c,ID3D11Resource* resource)noexcept {
    if(!enabled.load()||!GenshinLegacy::ReadNewInputObservationDemand().Requested()||!resource)return {};
    ComPtr<ID3D11Buffer> buffer;if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))||!SameDevice(c,buffer.Get()))return {};
    return Read(buffer.Get());
}
inline std::shared_ptr<const Image> Inherit(const Record& r,const std::shared_ptr<const Image>& before)noexcept {
    return before&&before->epoch==epoch.load()&&before->revision<UINT64_MAX&&
        r.revision==before->revision+1&&!r.map&&!r.readContext&&!r.pending&&!r.exhausted?before:nullptr;
}
struct Upload {
    Upload()=default;Upload(const Upload&)=delete;Upload& operator=(const Upload&)=delete;
    Upload(Upload&& other)noexcept { *this=std::move(other); }
    Upload& operator=(Upload&& other)noexcept {
        if(this!=&other){Cancel();record=std::move(other.record);image=std::move(other.image);
            source=std::move(other.source);sourceImage=std::move(other.sourceImage);
            revision=other.revision;capturedEpoch=other.capturedEpoch;thread=other.thread;
            pending=std::exchange(other.pending,false);}return *this;
    }
    std::shared_ptr<Record> record;
    std::shared_ptr<Image> image;
    ComPtr<ID3D11Buffer> source;
    std::shared_ptr<const Image> sourceImage;
    uint64_t revision=0,capturedEpoch=0;
    DWORD thread=0;bool pending=false;
    void Cancel()noexcept {
        if(!record||!pending)return;
        std::lock_guard lock(record->mutex);pending=false;
        if(record->pending)--record->pending;
        if(record->revision==revision){record->image.reset();record->cause=Cause::CaptureFailed;}
    }
    ~Upload(){Cancel();}
    void Returned()noexcept {
        if(!record||!pending)return;
        const bool sourceIntact=!source||Read(source.Get())==sourceImage;
        std::lock_guard lock(record->mutex);pending=false;
        if(record->pending)--record->pending;
        if(image&&sourceIntact&&thread==GetCurrentThreadId()&&revision&&record->revision==revision&&
           !record->map&&!record->readContext&&!record->pending&&!record->exhausted&&capturedEpoch==epoch.load()&&capturedEpoch!=UINT64_MAX){
            image->revision=revision;record->image=std::move(image);record->cause=Cause::Resident;record->touched.store(touchSerial.fetch_add(1));
        }else if(record->revision==revision){record->image.reset();record->cause=Cause::CaptureFailed;}
    }
};
inline Upload Publish(const std::shared_ptr<Record>& r,std::shared_ptr<Image> image,uint64_t expected) {
    Upload out;if(!r||!image)return out;
    std::lock_guard lock(r->mutex);
    if(r->map||r->readContext||r->pending||r->exhausted||r->revision!=expected||image->epoch!=epoch.load())return out;
    out.record=r;out.revision=r->Next();if(!out.revision)return {};
    out.image=std::move(image);out.capturedEpoch=epoch.load();out.thread=GetCurrentThreadId();
    ++r->pending;r->cause=Cause::Pending;out.pending=true;return out;
}
inline Upload Update(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub,
    const D3D11_BOX* box,const void* bytes,UINT flags=0,const std::shared_ptr<const Image>& before={})noexcept {
    Upload out;
    if(!enabled.load()||!GenshinLegacy::ReadNewInputObservationDemand().Requested()||sub||!SameDevice(context,resource)||
       (flags&~(D3D11_COPY_DISCARD|D3D11_COPY_NO_OVERWRITE))||
       flags==(D3D11_COPY_DISCARD|D3D11_COPY_NO_OVERWRITE))return out;
    try{
        auto r=Buffer(resource,true);if(!r||r->desc.Usage!=D3D11_USAGE_DEFAULT||r->desc.BindFlags==D3D11_BIND_CONSTANT_BUFFER)return out;
        ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;context->GetPredication(&predicate,&value);if(predicate)return out;
        UINT begin=0,end=r->desc.ByteWidth;
        if(box){D3D11_BOX b{};if(!CpuCopy(&b,box,sizeof(b))||b.top||b.front||b.bottom!=1||b.back!=1)return out;
            begin=b.left;end=b.right;}
        std::shared_ptr<const Image> base;uint64_t revision=0;
        {std::lock_guard lock(r->mutex);base=Inherit(*r,before);revision=r->revision;}
        if(flags&D3D11_COPY_DISCARD)base.reset();
        return Publish(r,Freeze(r->desc,bytes,begin,end,base),revision);
    }catch(...){return out;}
}
inline Upload Copy(ID3D11DeviceContext* context,ID3D11Resource* destination,ID3D11Resource* source,
    UINT sub,UINT x,UINT y,UINT z,UINT ss,const D3D11_BOX* box,UINT flags,
    const std::shared_ptr<const Image>& before,bool whole=false)noexcept {
    Upload out;if(!enabled.load()||!GenshinLegacy::ReadNewInputObservationDemand().Requested()||!destination||!source||sub||ss||y||z||
        !SameDevice(context,destination)||!SameDevice(context,source)||
        (flags&~(D3D11_COPY_DISCARD|D3D11_COPY_NO_OVERWRITE))||
        flags==(D3D11_COPY_DISCARD|D3D11_COPY_NO_OVERWRITE))return out;
    try{
        ComPtr<ID3D11Buffer> sb,db;
        if(FAILED(source->QueryInterface(IID_PPV_ARGS(&sb)))||FAILED(destination->QueryInterface(IID_PPV_ARGS(&db)))||sb==db)return out;
        ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;context->GetPredication(&predicate,&value);if(predicate)return out;
        auto r=Buffer(destination,true);auto sourceImage=Read(sb.Get());
        if(!r||!sourceImage||r->desc.Usage==D3D11_USAGE_IMMUTABLE||r->desc.Usage==D3D11_USAGE_DYNAMIC)return out;
        UINT begin=0,end=sourceImage->desc.ByteWidth;
        if(whole){if(x||box||flags||r->desc.ByteWidth!=end)return out;}
        else if(box){D3D11_BOX b{};if(!CpuCopy(&b,box,sizeof(b))||b.top||b.front||b.bottom!=1||b.back!=1)return out;
            begin=b.left;end=b.right;}
        if(begin>=end||end>sourceImage->desc.ByteWidth||uint64_t(x)+end-begin>r->desc.ByteWidth)return out;
        std::shared_ptr<const Image> base;uint64_t revision=0;
        {std::lock_guard lock(r->mutex);base=Inherit(*r,before);revision=r->revision;}
        if(flags&D3D11_COPY_DISCARD)base.reset();
        std::vector<Part> replacement;replacement.reserve(Image::MaxParts);
        Clip(replacement,*sourceImage,begin,end,x);
        out=Publish(r,Splice(r->desc,base,x,x+end-begin,replacement),revision);
        if(out.pending){out.source=sb;out.sourceImage=std::move(sourceImage);}return out;
    }catch(...){return out;}
}
struct Map {
    Map()=default;Map(const Map&)=default;Map(Map&&)noexcept=default;
    Map& operator=(Map other)noexcept {this->~Map();new(this) Map(std::move(other));return *this;}
    GenshinDepthRetention::Buffers<1> retention;
    ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Resource> resource;
    std::shared_ptr<Record> record;
    std::shared_ptr<Storage> shadow;
    std::shared_ptr<const Image> before;
    bool discard=false;
    void* driver=nullptr;UINT bytes=0;DWORD thread=0;uint64_t id=0,capturedEpoch=0;
};
using Maps=FramePipeline::MappedResourceJournal<Map,32,FramePipeline::MapConflict::Resource>;
inline Maps& ActiveMaps(){static Maps maps;return maps;}
inline void Mapped(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub,D3D11_MAP type,
    UINT flags,D3D11_MAPPED_SUBRESOURCE* mapped,HRESULT hr,const std::shared_ptr<const Image>& before={})noexcept {
    // Only a successful writable Map changes this source version. Failed Map
    // and READ leave the existing version intact; all replay/query revocations
    // at hook entry still happen. Unsupported successful writes still revoke.
    if(!enabled.load()||FAILED(hr))return;
    if(type==D3D11_MAP_READ){
        // Keep READ open until the exact returned Unmap. No shadow or source
        // allocation; failed bookkeeping remains conservative at Unmap.
        try{auto r=Buffer(resource);if(!r||sub||!SameDevice(context,resource))return;
            std::lock_guard lock(r->mutex);
            if(r->map||r->pending||r->readContext||r->readSerial==UINT64_MAX)return;
            ++r->readSerial;r->readContext=context;r->readThread=GetCurrentThreadId();activeReadMapCount.fetch_add(1);
        }catch(...){ }return;
    }
    auto r=Written(resource);
    // Stop new CPU shadows while retaining actual write invalidation and all
    // previously-returned application pointers in ActiveMaps. Creation images
    // stay observed: immutable geometry is needed by a future consumer.
    if(!GenshinLegacy::ReadNewInputObservationDemand().Requested())return;
    // Semantic CB mapping owns its own shadow. Its portable version was
    // revoked above; there is no portable shadow to authorize for this resource.
    if(r && r->desc.BindFlags==D3D11_BIND_CONSTANT_BUFFER)return;
    if(!mapped||!mapped->pData||sub||
       !SameDevice(context,resource)||
       (flags&~D3D11_MAP_FLAG_DO_NOT_WAIT))return;
    try{
        if(!r)r=Buffer(resource,true);if(!r||r->desc.BindFlags==D3D11_BIND_CONSTANT_BUFFER)return;
        const bool discard=type==D3D11_MAP_WRITE_DISCARD&&r->desc.Usage==D3D11_USAGE_DYNAMIC;
        const bool preserve=(type==D3D11_MAP_WRITE_NO_OVERWRITE&&r->desc.Usage==D3D11_USAGE_DYNAMIC)||
            ((type==D3D11_MAP_WRITE||type==D3D11_MAP_READ_WRITE)&&r->desc.Usage==D3D11_USAGE_STAGING);
        if(!discard&&!preserve)return;
        std::shared_ptr<const Image> base;uint64_t revision=0;
        {std::lock_guard lock(r->mutex);base=Inherit(*r,before);revision=r->revision;}
        // A preserving Map cannot invent any old bytes, including an explicit
        // application write that happens to equal our unknown fill value.
        if(preserve&&(!base||!base->Covers(0,r->desc.ByteWidth)))return;
        Map next;ComPtr<ID3D11Buffer> buffer;
        if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))||!next.retention.Add(buffer.Get()))return;
        next.context=context;next.resource=resource;next.record=r;next.driver=mapped->pData;
        next.bytes=r->desc.ByteWidth;next.thread=GetCurrentThreadId();next.capturedEpoch=epoch.load();
        next.discard=discard;next.before=std::move(base);next.shadow=Allocate(next.bytes);if(!next.shadow)return;
        if(discard)std::memset(next.shadow->data.data(),0,next.bytes);
        else if(!next.before->CopyTo(next.shadow->data.data(),0,next.bytes))return;
        auto* data=next.shadow->data.data();
        if(ActiveMaps().TryInsert({reinterpret_cast<uintptr_t>(context),reinterpret_cast<uintptr_t>(resource),sub},
            std::move(next),[&]{
            std::lock_guard lock(r->mutex);
            if(r->revision!=revision||r->map||r->readContext||r->pending||next.capturedEpoch!=epoch.load())return false;
            r->image.reset();r->cause=Cause::Mapped;r->map=next.id=r->Next();return next.id!=0;
        }))mapped->pData=data;
    }catch(...){ }
}
// Take BEFORE original Unmap: driver writeback is mandatory even after loss,
// thread mismatch or a failed semantic receipt. Never read write-combined RAM.
struct Unmap {
    Map map;
    Upload upload;
    std::shared_ptr<Record> readRecord;
    ID3D11DeviceContext* readContext=nullptr;DWORD readThread=0;uint64_t readSerial=0;
    void Returned()noexcept {
        upload.Returned();if(!readRecord)return;
        std::lock_guard lock(readRecord->mutex);
        if(readRecord->readContext==readContext&&readRecord->readThread==readThread&&
           readThread==GetCurrentThreadId()&&readRecord->readSerial==readSerial){
            readRecord->readContext=nullptr;readRecord->readThread=0;activeReadMapCount.fetch_sub(1);}
    }
};
inline Unmap BeforeUnmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT sub)noexcept {
    Unmap out;if(!ActiveMaps().HasEntries()&&!activeReadMapCount.load())return out;
    try{
        if(activeReadMapCount.load()&&!sub)if(auto r=Buffer(resource)){
            std::lock_guard lock(r->mutex);
            if(r->readContext==context&&r->readThread==GetCurrentThreadId()){
                out.readRecord=r;out.readContext=context;out.readThread=r->readThread;out.readSerial=r->readSerial;return out;}}
        if(!ActiveMaps().HasEntries())return out;
        out.map=ActiveMaps().Take({reinterpret_cast<uintptr_t>(context),reinterpret_cast<uintptr_t>(resource),sub});
        auto& m=out.map;if(!m.shadow)return out; // No extracted transaction owns a map id.
        // Own this exact id before optional metadata can allocate. Unwinding
        // revokes only our unfinished CPU receipt, never a newer/reentrant Map.
        // Keep the record independently of out: returning may move out.map.
        struct MapCleanup {
            std::shared_ptr<Record> record;uint64_t id=0;
            ~MapCleanup()noexcept {
                if(!id)return;std::lock_guard lock(record->mutex);
                if(record->map!=id)return;
                record->image.reset();record->cause=Cause::CaptureFailed;record->map=0;record->Next();
            }
        } cleanup{m.record,m.id};
        std::shared_ptr<Image> image;
        const auto* shadow=static_cast<const unsigned char*>(m.shadow->data.data());
        if(m.discard){
            // DISCARD makes the entire allocation undefined. Our zero-initialized
            // shadow is actually written to the driver; no unwritten driver RAM
            // is read or asserted. Share that concrete storage after Unmap.
            std::memcpy(m.driver,shadow,m.bytes);
            image=Splice(m.record->desc,{},0,m.bytes,{{m.shadow,0,m.bytes,0}});
        }else{
            // NO_OVERWRITE must never write untouched bytes still in flight.
            // Compare CPU images and write ONLY changed runs, each a subset of
            // bytes the application actually changed during this exact Map.
            for(const auto& p:m.before->parts){
                const auto* old=static_cast<const unsigned char*>(p.storage->data.data())+p.offset;
                FramePipeline::ForEachChangedByteRange(shadow+p.begin,old,p.end-p.begin,[&](size_t first,size_t last){
                    const auto begin=p.begin+static_cast<UINT>(first),end=p.begin+static_cast<UINT>(last);
                    std::memcpy(static_cast<unsigned char*>(m.driver)+begin,shadow+begin,end-begin);
                });
            }
            // Preserving Map was admitted only with a complete CPU preimage,
            // copied into this shadow before the pointer reached the caller.
            // Unmap ends its mutable lifetime. Publish that already charged
            // complete snapshot instead of packing/reallocating every changed
            // fragment and repeatedly clipping the previous interval map.
            // The driver still receives ONLY actual changed bytes above.
            image=Splice(m.record->desc,{},0,m.bytes,{{m.shadow,0,m.bytes,0}});
        }
        std::lock_guard lock(m.record->mutex);
        if(m.record->map!=m.id)return out; // A newer map owns its own revision.
        const bool exact=m.thread==GetCurrentThreadId()&&m.capturedEpoch==epoch.load();
        m.record->map=0;const auto revision=m.record->Next();cleanup.id=0;
        if(exact&&image){out.upload.record=m.record;out.upload.image=std::move(image);out.upload.revision=revision;
            out.upload.capturedEpoch=m.capturedEpoch;out.upload.thread=m.thread;
            ++m.record->pending;out.upload.pending=true;}
    }catch(...){ }return out;
}
} // namespace GenshinDepthPortable
