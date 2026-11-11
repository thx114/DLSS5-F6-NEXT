// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinDepthSemantic.h"
#include "GenshinDepthDraw.h"
#include "GenshinDepthPortableResources.h"
#include "../runtime/CpuIndexBounds.h"
#include <d3d11_3.h>
#include <functional>

namespace GenshinDepthMapping::Deferred {
using Microsoft::WRL::ComPtr;
inline std::atomic<unsigned> liveVersions{0};
struct Version {
    Version(){liveVersions.fetch_add(1,std::memory_order_relaxed);}
    ~Version(){liveVersions.fetch_sub(1,std::memory_order_relaxed);}
    ComPtr<IUnknown> identity;
    std::atomic<bool> changed{false};
    uint64_t epoch=0;
};
inline std::mutex versionMutex;
// Slots retain the original 512-live-version limit. Only the newest version of
// each canonical IUnknown identity is indexed; older live tokens keep their
// slots and strong identity owners, but are already permanently invalid.
struct VersionSlot {
    std::weak_ptr<Version> version;
    IUnknown* identity=nullptr; // Comparison key only; never dereferenced/owned here.
    size_t next=0;
};
inline std::array<VersionSlot,512> versions;
inline std::array<size_t,1024> versionBuckets{}; // One-based slot links; zero ends a chain.
// All index access stays under versionMutex, including the empty-table case.
inline size_t VersionBucket(IUnknown* identity)noexcept {
    return std::hash<IUnknown*>{}(identity)%versionBuckets.size();
}
inline size_t FindVersion(IUnknown* identity)noexcept {
    for(auto i=versionBuckets[VersionBucket(identity)];i;i=versions[i-1].next)
        if(versions[i-1].identity==identity)return i;
    return 0;
}
// Watch alone maintains/reclaims the index. Expiration never takes a lock in
// Version's destructor, and no index operation retains a strong GPU owner.
inline void UnindexVersion(size_t index)noexcept {
    if(!index)return;auto& slot=versions[index-1];if(!slot.identity)return;
    auto* link=&versionBuckets[VersionBucket(slot.identity)];
    while(*link && *link!=index)link=&versions[*link-1].next;
    if(*link)*link=slot.next;
    slot.identity=nullptr;slot.next=0;
}
inline std::atomic<uint64_t> versionEpoch{1};
inline void LoseVersions(bool unknownSourceWrite=true)noexcept {
    // Unknown resource/domain writes still revoke all mutable CPU sources.
    // Contention in THIS replay watch index loses its tokens only: the exact
    // source's own Record already received Written before this index lock.
    if(unknownSourceWrite)GenshinDepthPortable::Lost();
    auto n=versionEpoch.load();while(n!=UINT64_MAX && !versionEpoch.compare_exchange_weak(n,n+1)){}
}
inline void Written(ID3D11Resource* resource,bool unmap=false)noexcept {
    if(GenshinDepthSemantic::Internal())return;
    GenshinDepthPortable::Written(resource,unmap);
    if(!resource){LoseVersions();return;}
    ComPtr<IUnknown> identity;if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&identity)))){LoseVersions();return;}
    std::unique_lock lock(versionMutex,std::try_to_lock);if(!lock.owns_lock()){LoseVersions(false);return;}
    // QI and try-lock failure still lose the replay epoch. Do not add an
    // outer liveVersions fast return: Watch may be publishing its first token.
    const auto index=FindVersion(identity.Get());
    if(index)if(auto item=versions[index-1].version.lock())item->changed=true;
}
inline std::shared_ptr<Version> Watch(ID3D11Resource* resource) {
    if(!resource)return {};ComPtr<IUnknown> identity;
    if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&identity))))return {};
    std::unique_lock lock(versionMutex,std::try_to_lock);if(!lock.owns_lock()){LoseVersions(false);return {};}
    const auto epoch=versionEpoch.load();if(epoch==UINT64_MAX)return {};
    const auto index=FindVersion(identity.Get());
    if(index)if(auto item=versions[index-1].version.lock())
        if(item->epoch==epoch && !item->changed)return item;
    for(size_t i=0;i<versions.size();++i)if(versions[i].version.expired()){
        auto item=std::make_shared<Version>();item->identity=identity;item->epoch=epoch;
        // Any previous token for this identity was changed or belongs to an
        // older, permanently lost epoch. Mark it before unlinking so Written
        // only needs the newest token; retained old tokens still fail Intact.
        if(index){
            if(auto previous=versions[index-1].version.lock())previous->changed=true;
            UnindexVersion(index);
        }
        UnindexVersion(i+1);
        auto& slot=versions[i];slot.version=item;slot.identity=identity.Get();
        const auto bucket=VersionBucket(identity.Get());
        slot.next=versionBuckets[bucket];versionBuckets[bucket]=i+1;return item;
    }
    return {};
}
struct ExpectedRaster {
    ID3D11DeviceContext* context=nullptr;uint64_t call=0;
    std::array<ComPtr<IUnknown>,8> targets{};
};
inline thread_local ExpectedRaster expectedRaster;
inline void EndExpected(ID3D11DeviceContext* context)noexcept {
    if(!context || expectedRaster.context==context)expectedRaster={};
}
inline bool ExpectRaster(ID3D11DeviceContext* c)noexcept {
    expectedRaster={};if(!c)return false;
    std::array<ID3D11RenderTargetView*,8> views{};c->OMGetRenderTargets(8,views.data(),nullptr);
    bool valid=true;
    for(unsigned i=0;i<8;++i)if(views[i]){
        ComPtr<ID3D11Resource> resource;views[i]->GetResource(&resource);views[i]->Release();
        if(!resource || FAILED(resource.As(&expectedRaster.targets[i])))valid=false;
    }
    if(!valid){expectedRaster={};return false;}
    expectedRaster.context=c;expectedRaster.call=CurrentDraw().observationCall;return true;
}
// Only use immediately after Raster::Capture, with no intervening setters or
// callbacks. The captured Raster keeps every view alive for this original call.
inline bool ExpectRaster(ID3D11DeviceContext* c,const DrawArguments& draw,
                         const std::array<ComPtr<ID3D11RenderTargetView>,8>& views)noexcept {
    expectedRaster={};const auto& current=CurrentDraw();
    if(!c || draw.context!=c || current.context!=c || draw.thread!=GetCurrentThreadId() ||
       current.thread!=draw.thread || !draw.observationCall || draw.observationCall!=current.observationCall)return false;
    bool valid=true;
    for(unsigned i=0;i<8;++i)if(views[i]){
        ComPtr<ID3D11Resource> resource;views[i]->GetResource(&resource);
        if(!resource || FAILED(resource.As(&expectedRaster.targets[i])))valid=false;
    }
    if(!valid){expectedRaster={};return false;}
    expectedRaster.context=c;expectedRaster.call=draw.observationCall;return true;
}
inline bool Expected(ID3D11DeviceContext* c,ID3D11Resource* resource)noexcept {
    if(!resource || expectedRaster.context!=c || !expectedRaster.call ||
       expectedRaster.call!=CurrentDraw().observationCall)return false;
    ComPtr<IUnknown> id;if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&id))))return false;
    for(const auto& target:expectedRaster.targets)if(target && target.Get()==id.Get())return true;return false;
}
// Observe outputs independently of the depth-writer cache. Only the exact
// retained original call may advance its RTV chain without invalidating it.
inline void Outputs(ID3D11DeviceContext* c,bool compute,bool retained=false)noexcept {
    if(GenshinDepthSemantic::Internal())return;
    if(!liveVersions.load(std::memory_order_relaxed))return;
    if(!c || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){LoseVersions();return;}
    ComPtr<ID3D11Device> device;c->GetDevice(&device);if(!device || FAILED(device->GetDeviceRemovedReason())){LoseVersions();return;}
    if(!compute){std::array<ID3D11RenderTargetView*,8> views{};c->OMGetRenderTargets(8,views.data(),nullptr);
        for(auto* view:views)if(view){ComPtr<ID3D11Resource> resource;view->GetResource(&resource);view->Release();
            if(!retained || !Expected(c,resource.Get()))Written(resource.Get());}}
    // SO is another GPU write boundary, even when the retained original RTV
    // is exempt. Getter references are owned here; no offsets/state are changed.
    if(!compute){std::array<ID3D11Buffer*,D3D11_SO_BUFFER_SLOT_COUNT> targets{};
        c->SOGetTargets(static_cast<UINT>(targets.size()),targets.data());
        for(auto* target:targets)if(target){Written(target);target->Release();}}
    const UINT count=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
    std::array<ID3D11UnorderedAccessView*,64> uavs{};
    if(compute)c->CSGetUnorderedAccessViews(0,count,uavs.data());
    else c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,count,uavs.data());
    for(auto* view:uavs)if(view){ComPtr<ID3D11Resource> resource;view->GetResource(&resource);view->Release();Written(resource.Get());}
}
inline bool Target(ID3D11Resource* resource)noexcept {
    ComPtr<ID3D11Texture2D> texture;if(!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))return false;
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    // Object identity is not storage identity for shared/tiled allocations.
    return d.Usage==D3D11_USAGE_DEFAULT && !d.CPUAccessFlags && !d.MiscFlags;
}
inline bool Intact(const std::shared_ptr<Version>& version)noexcept {
    return version && !version->changed.load() && version->epoch==versionEpoch.load() && version->epoch!=UINT64_MAX;
}
// Per-closed-device immutable object reuse. Strong original keys exclude
// pointer reuse; bounded eviction only drops cache references, never Work/Node
// GPU roots. No application resource is rebound on a foreign device.
struct PortableCache {
    struct Objects {
        std::array<ComPtr<ID3D11DeviceChild>,7> original,clone;
    };
    struct Buffer {
        GenshinDepthRetention::Buffers<1> retention;
        std::weak_ptr<const void> image;
        ComPtr<ID3D11Buffer> object;
    };
    std::array<Objects,32> objects{};size_t objectCursor=0;
    std::array<Buffer,128> buffers{};size_t bufferCursor=0;
    ComPtr<IUnknown> device;
    void DropBuffers()noexcept {
        for(auto& entry:buffers){entry.object.Reset();entry.image.reset();entry.retention={};}
    }
};
// CPU records only. No command lists, GPU copies, private draws or query edits
// are permitted while collecting an application's original raster call.
struct Raster {
    // Tokens precede every buffer owner, including geometry version identities.
    GenshinDepthRetention::Buffers<254> retention;
    Raster()=default;Raster(const Raster&)=default;Raster(Raster&&)noexcept=default;
    Raster& operator=(Raster other)noexcept {this->~Raster();new(this) Raster(std::move(other));return *this;}
    DrawArguments draw;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilView> dsv;
    std::array<ComPtr<ID3D11RenderTargetView>,8> rtvs;
    std::array<ComPtr<ID3D11Buffer>,32> vertices;
    ComPtr<ID3D11Buffer> indices;
    std::array<std::shared_ptr<Version>,33> geometry;
    std::array<ComPtr<ID3D11Buffer>,42> originalConstants,constants; // VS, HS, DS
    std::array<std::shared_ptr<GenshinDepthSemantic::detail::Payload>,42> payload;
    std::array<uint64_t,42> revisions{};
    std::array<ComPtr<ID3D11ShaderResourceView>,128> psViews;
    std::array<ComPtr<ID3D11SamplerState>,16> samplers;
    std::array<UINT,32> strides{},offsets{};
    std::array<UINT,42> first{},count{};
    std::array<D3D11_VIEWPORT,16> viewports;
    std::array<D3D11_RECT,16> scissors;
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    DXGI_FORMAT indexFormat=DXGI_FORMAT_UNKNOWN;
    UINT indexOffset=0,viewportCount=16,scissorCount=16,reference=0,sampleMask=0;
    FLOAT factors[4]{};uint64_t loss=0;
    bool returned=false;
    struct PortableInputs {
        std::shared_ptr<const GenshinDepthPortable::Layout> layout;
        std::array<std::shared_ptr<const GenshinDepthPortable::Image>,33> geometry;
        std::array<std::vector<unsigned char>,3> shaders;
        std::array<D3D11_INPUT_ELEMENT_DESC,32> elements{};
        UINT elementCount=0;uint32_t slots=0;
        enum class Failure {None,Layout,VertexBytes,IndexBytes,ShaderBytes,ChangedBeforeReturn,ConstantPayload,
            LayoutSignature,LayoutFormat,FetchRange,WrongDevice};
        Failure failure=Failure::None;UINT slot=UINT_MAX;
        GenshinDepthPortable::ReadStatus sourceStatus; // captured at the actual failed source read
        bool ready=false,dependenciesKnown=false;
        static const char* Text(Failure f)noexcept {
            switch(f){
            case Failure::None:return "READY";case Failure::Layout:return "LAYOUT_CREATION_MISSING";
            case Failure::VertexBytes:return "VERTEX_CPU_VERSION_MISSING";case Failure::IndexBytes:return "INDEX_CPU_VERSION_MISSING";
            case Failure::ShaderBytes:return "ACTUAL_SHADER_BYTES_MISSING";case Failure::ChangedBeforeReturn:return "IA_VERSION_CHANGED_DURING_DRAW";
            case Failure::LayoutSignature:return "LAYOUT_VS_SIGNATURE_MISMATCH";case Failure::LayoutFormat:return "LAYOUT_FORMAT_OR_CLASS_UNSUPPORTED";
            case Failure::FetchRange:return "IA_FETCH_RANGE_UNPROVEN";case Failure::WrongDevice:return "IA_LAYOUT_SHADER_DEVICE_MISMATCH";
            default:return "CB_PAYLOAD_UNAVAILABLE";}
        }
    } portable;
    static UINT ElementBytes(DXGI_FORMAT format)noexcept {
        switch(format){
        case DXGI_FORMAT_R32G32B32A32_FLOAT:case DXGI_FORMAT_R32G32B32A32_UINT:case DXGI_FORMAT_R32G32B32A32_SINT:return 16;
        case DXGI_FORMAT_R32G32B32_FLOAT:case DXGI_FORMAT_R32G32B32_UINT:case DXGI_FORMAT_R32G32B32_SINT:return 12;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R32G32_UINT:case DXGI_FORMAT_R32G32_SINT:return 8;
        case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R10G10B10A2_UINT:case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16G16_UNORM:case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_UINT:case DXGI_FORMAT_R32_SINT:return 4;
        case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_UINT:case DXGI_FORMAT_R8G8_SNORM:case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_UINT:case DXGI_FORMAT_R16_SNORM:case DXGI_FORMAT_R16_SINT:return 2;
        case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_UINT:case DXGI_FORMAT_R8_SNORM:case DXGI_FORMAT_R8_SINT:return 1;
        default:return 0;
        }
    }
    static UINT ElementAlignment(DXGI_FORMAT format)noexcept {
        switch(format){
        case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16G16_UNORM:case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_UINT:case DXGI_FORMAT_R16_SNORM:case DXGI_FORMAT_R16_SINT:return 2;
        case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_UINT:case DXGI_FORMAT_R8G8_SNORM:case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_UINT:case DXGI_FORMAT_R8_SNORM:case DXGI_FORMAT_R8_SINT:return 1;
        default:return 4;
        }
    }
    void CapturePortable()noexcept {
        try{
            const auto fail=[&](PortableInputs::Failure failure,UINT slot=UINT_MAX){portable.failure=failure;portable.slot=slot;};
            using F=PortableInputs::Failure;
            ID3D11DeviceChild* stages[]{vs.Get(),hs.Get(),ds.Get()};
            for(unsigned i=0;i<3;++i)if(stages[i]){
                if(!GenshinDepthPortable::SameDevice(draw.context,stages[i])){fail(F::WrongDevice,i);return;}
                if(!GenshinDepthEvidence::ReadShaderBytes(stages[i],portable.shaders[i])){fail(F::ShaderBytes,i);return;}
            }
            using S=GenshinDepthSemantic::detail::Stage;
            const auto program=GenshinDepthSemantic::detail::Shader(vs.Get(),S::ControlVertex);
            D3D11_SHADER_DESC sd{};
            if(!program||!program->reflection||FAILED(program->reflection->GetDesc(&sd))){fail(F::ShaderBytes);return;}
            bool needsLayout=false;
            for(UINT i=0;i<sd.InputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC input{};
                if(FAILED(program->reflection->GetInputParameterDesc(i,&input))){fail(F::LayoutSignature);return;}
                if(input.ReadWriteMask&&input.SystemValueType!=D3D_NAME_VERTEX_ID&&input.SystemValueType!=D3D_NAME_INSTANCE_ID)needsLayout=true;
            }
            if(needsLayout){
                if(!layout){fail(F::Layout);return;}
                if(!GenshinDepthPortable::SameDevice(draw.context,layout.Get())){fail(F::WrongDevice);return;}
                portable.layout=GenshinDepthPortable::ReadLayout(layout.Get());
                if(!portable.layout){fail(F::Layout);return;}
            }
            std::array<D3D11_INPUT_ELEMENT_DESC,32> resolved{};
            if(portable.layout){
                // Resolve APPEND against ALL original elements before removing
                // unread semantics. Filtering must not change a later offset.
                std::array<UINT,32> ends{};
                for(UINT i=0;i<portable.layout->count;++i){auto e=portable.layout->elements[i];const auto size=ElementBytes(e.Format);
                    if(!size||e.InputSlot>=32||
                       (e.InputSlotClass!=D3D11_INPUT_PER_VERTEX_DATA&&e.InputSlotClass!=D3D11_INPUT_PER_INSTANCE_DATA)||
                       (e.InputSlotClass==D3D11_INPUT_PER_VERTEX_DATA&&e.InstanceDataStepRate)){
                        fail(F::LayoutFormat,e.InputSlot);return;}
                    const UINT alignment=ElementAlignment(e.Format);
                    if(e.AlignedByteOffset==D3D11_APPEND_ALIGNED_ELEMENT){
                        if(ends[e.InputSlot]>UINT_MAX-alignment){fail(F::FetchRange,e.InputSlot);return;}
                        e.AlignedByteOffset=(ends[e.InputSlot]+alignment-1)&~(alignment-1);
                    }
                    if(e.AlignedByteOffset>UINT_MAX-size){fail(F::FetchRange,e.InputSlot);return;}
                    ends[e.InputSlot]=e.AlignedByteOffset+size;resolved[i]=e;
                }
            }
            for(UINT i=0;i<sd.InputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC input{};
                if(FAILED(program->reflection->GetInputParameterDesc(i,&input))){fail(F::LayoutSignature);return;}
                if(!input.ReadWriteMask||input.SystemValueType==D3D_NAME_VERTEX_ID||input.SystemValueType==D3D_NAME_INSTANCE_ID)continue;
                bool found=false;
                if(portable.layout)for(UINT j=0;j<portable.layout->count;++j){const auto& e=resolved[j];
                    if(e.SemanticIndex==input.SemanticIndex&&!_stricmp(e.SemanticName,input.SemanticName)){
                        if(found||portable.elementCount==portable.elements.size()){fail(F::LayoutSignature);return;}
                        found=true;portable.elements[portable.elementCount++]=e;portable.slots|=uint32_t(1)<<e.InputSlot;
                    }
                }
                if(!found){fail(F::LayoutSignature);return;}
            }
            portable.dependenciesKnown=true;
            const bool indexed=draw.kind==2||draw.kind==4;
            uint64_t firstVertex=draw.start,lastVertex=uint64_t(draw.start)+draw.count-1;
            if(indexed){
                if(!indices||!GenshinDepthPortable::SameDevice(draw.context,indices.Get())){fail(F::IndexBytes,32);return;}
                auto image=GenshinDepthPortable::Read(indices.Get(),&portable.sourceStatus);portable.geometry[32]=image;
                if(!image){fail(F::IndexBytes,32);return;}
                const UINT width=indexFormat==DXGI_FORMAT_R16_UINT?2:indexFormat==DXGI_FORMAT_R32_UINT?4:0;
                const uint64_t begin=uint64_t(indexOffset)+uint64_t(draw.start)*width,end=begin+uint64_t(draw.count)*width;
                if(!width||begin>UINT_MAX||end>UINT_MAX||!image->Covers(UINT(begin),UINT(end))){fail(F::FetchRange,32);return;}
                const bool strip=topology==D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP||topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP||
                    topology==D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ||topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
                // Decode only actual CPU-proven index bytes, never a driver Map
                // or indirect argument readback in the application's query domain.
                FramePipeline::CpuIndexBounds bounds;
                if(!FramePipeline::ReadCpuIndexBounds(UINT(begin),draw.count,width,draw.base,strip,
                    [&](void* target,UINT low,UINT high){return image->CopyTo(target,low,high);},bounds)){
                    fail(F::FetchRange,32);return;
                }
                firstVertex=bounds.first;lastVertex=bounds.last;
            }
            if(lastVertex>UINT_MAX){fail(F::FetchRange);return;}
            for(UINT slot=0;slot<32;++slot)if(portable.slots&(uint32_t(1)<<slot)){
                if(!vertices[slot]||!GenshinDepthPortable::SameDevice(draw.context,vertices[slot].Get())){fail(F::WrongDevice,slot);return;}
                auto image=GenshinDepthPortable::Read(vertices[slot].Get(),&portable.sourceStatus);portable.geometry[slot]=image;
                if(!image){fail(F::VertexBytes,slot);return;}
                for(UINT i=0;i<portable.elementCount;++i){const auto& e=portable.elements[i];if(e.InputSlot!=slot)continue;
                    // A nonzero instance base with a divisor other than one
                    // remains unsupported rather than guessing driver indexing.
                    if(e.InputSlotClass==D3D11_INPUT_PER_INSTANCE_DATA&&draw.instanceStart&&e.InstanceDataStepRate!=1){fail(F::FetchRange,slot);return;}
                    const uint64_t low=e.InputSlotClass==D3D11_INPUT_PER_INSTANCE_DATA?(draw.kind>=3?draw.instanceStart:0):firstVertex;
                    const uint64_t high=e.InputSlotClass==D3D11_INPUT_PER_INSTANCE_DATA?low:lastVertex;
                    const uint64_t begin=uint64_t(offsets[slot])+low*strides[slot]+e.AlignedByteOffset;
                    const uint64_t end=uint64_t(offsets[slot])+high*strides[slot]+e.AlignedByteOffset+ElementBytes(e.Format);
                    if(begin>UINT_MAX||end>UINT_MAX||!image->Covers(UINT(begin),UINT(end))){fail(F::FetchRange,slot);return;}
                }
            }
            portable.ready=true;
        }catch(...){portable.failure=PortableInputs::Failure::VertexBytes;}
    }
    bool PortableReturned()noexcept {
        if(!portable.ready)return false;
        for(unsigned i=0;i<33;++i)if(portable.geometry[i]){
            auto* b=i==32?indices.Get():vertices[i].Get();
            if(GenshinDepthPortable::Read(b)!=portable.geometry[i]){
                portable.ready=false;portable.failure=PortableInputs::Failure::ChangedBeforeReturn;portable.slot=i;return false;}}
        return true;
    }
    // Clone only pre-raster inputs consumed by the existing UV mapper. Colour
    // SRVs/RTVs remain original metadata roots, never copied or rebound across
    // devices and never sampled by a replacement colour renderer.
    static bool ExecutionDevice(ID3D11Device* device,ID3D11DeviceContext* execution)noexcept {
        ComPtr<ID3D11Device> actual;ComPtr<IUnknown> a,b;execution->GetDevice(&actual);
        return actual&&SUCCEEDED(actual.As(&a))&&SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&b)))&&a==b&&SUCCEEDED(actual->GetDeviceRemovedReason());
    }
    bool MaterializePortable(ID3D11Device* device,ID3D11DeviceContext1* execution,Raster& out,PortableCache& cache)const {
        if(!device||!execution||!returned||!portable.ready||!ExecutionDevice(device,execution))return false;
        ComPtr<IUnknown> executionId;
        if(FAILED(device->QueryInterface(IID_PPV_ARGS(&executionId)))||(cache.device&&cache.device!=executionId))return false;
        cache.device=executionId;
        namespace S=GenshinDepthSemantic::detail;S::Access access;
        if(!access||Preflight(draw.context,access.s,nullptr,true).readiness!=Readiness::Ready)return false;
        GenshinDepthPortable::PrivateCreation privateCloning;
        out.draw=draw;out.draw.context=execution;out.context=execution;
        out.strides=strides;out.offsets=offsets;out.first=first;out.count=count;
        out.topology=topology;out.indexFormat=indexFormat;out.indexOffset=indexOffset;
        out.viewports=viewports;out.viewportCount=viewportCount;out.scissors=scissors;out.scissorCount=scissorCount;
        out.reference=reference;out.sampleMask=sampleMask;std::copy(std::begin(factors),std::end(factors),out.factors);
        const std::array<ComPtr<ID3D11DeviceChild>,7> originals{vs,hs,ds,layout,raster,depth,blend};
        PortableCache::Objects* objects=nullptr;
        for(auto& entry:cache.objects){bool same=true;
            for(unsigned i=0;i<7;++i)if(entry.original[i].Get()!=originals[i].Get()){same=false;break;}
            if(same){objects=&entry;break;}}
        if(objects){
            if(FAILED(objects->clone[0].As(&out.vs)))return false;
            if(hs&&FAILED(objects->clone[1].As(&out.hs)))return false;
            if(ds&&FAILED(objects->clone[2].As(&out.ds)))return false;
            if(portable.elementCount&&FAILED(objects->clone[3].As(&out.layout)))return false;
            if(raster&&FAILED(objects->clone[4].As(&out.raster)))return false;
            if(depth&&FAILED(objects->clone[5].As(&out.depth)))return false;
            if(blend&&FAILED(objects->clone[6].As(&out.blend)))return false;
        }else {
            if(portable.shaders[0].empty()||FAILED(device->CreateVertexShader(portable.shaders[0].data(),portable.shaders[0].size(),nullptr,&out.vs)))return false;
            if(hs&&FAILED(device->CreateHullShader(portable.shaders[1].data(),portable.shaders[1].size(),nullptr,&out.hs)))return false;
            if(ds&&FAILED(device->CreateDomainShader(portable.shaders[2].data(),portable.shaders[2].size(),nullptr,&out.ds)))return false;
            ID3D11DeviceChild* stages[]{out.vs.Get(),out.hs.Get(),out.ds.Get()};
            for(unsigned i=0;i<3;++i)if(stages[i]&&FAILED(stages[i]->SetPrivateData(GenshinDepthEvidence::BytesKey,
                static_cast<UINT>(portable.shaders[i].size()),portable.shaders[i].data())))return false;
            if(portable.elementCount&&FAILED(device->CreateInputLayout(portable.elements.data(),portable.elementCount,
                portable.shaders[0].data(),portable.shaders[0].size(),&out.layout)))return false;
            if(raster){
                ComPtr<ID3D11RasterizerState2> source2;ComPtr<ID3D11RasterizerState1> source1;
                const auto hr2=raster.As(&source2),hr1=raster.As(&source1);
                if(SUCCEEDED(hr2)){
                    D3D11_RASTERIZER_DESC2 d{};source2->GetDesc2(&d);ComPtr<ID3D11Device3> d3;ComPtr<ID3D11RasterizerState2> next;
                    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&d3)))||FAILED(d3->CreateRasterizerState2(&d,&next))||FAILED(next.As(&out.raster)))return false;
                }else if(hr2!=E_NOINTERFACE)return false;
                else if(SUCCEEDED(hr1)){
                    D3D11_RASTERIZER_DESC1 d{};source1->GetDesc1(&d);ComPtr<ID3D11Device1> d1;ComPtr<ID3D11RasterizerState1> next;
                    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&d1)))||FAILED(d1->CreateRasterizerState1(&d,&next))||FAILED(next.As(&out.raster)))return false;
                }else if(hr1!=E_NOINTERFACE)return false;
                else {D3D11_RASTERIZER_DESC d{};raster->GetDesc(&d);if(FAILED(device->CreateRasterizerState(&d,&out.raster)))return false;}
            }
            if(depth){D3D11_DEPTH_STENCIL_DESC d{};depth->GetDesc(&d);if(FAILED(device->CreateDepthStencilState(&d,&out.depth)))return false;}
            if(blend){ComPtr<ID3D11BlendState1> source1;const auto hr=blend.As(&source1);
                if(SUCCEEDED(hr)){
                    D3D11_BLEND_DESC1 d{};source1->GetDesc1(&d);
                    // Known Scene/Compose require a direct colour sample. A
                    // logic operation cannot be silently read as BlendEnable=0.
                    if(d.RenderTarget[0].LogicOpEnable)return false;
                    ComPtr<ID3D11Device1> d1;ComPtr<ID3D11BlendState1> next;
                    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&d1)))||FAILED(d1->CreateBlendState1(&d,&next))||FAILED(next.As(&out.blend)))return false;
                }else if(hr!=E_NOINTERFACE)return false;
                else {D3D11_BLEND_DESC d{};blend->GetDesc(&d);if(FAILED(device->CreateBlendState(&d,&out.blend)))return false;}
            }
            auto& entry=cache.objects[cache.objectCursor++%cache.objects.size()];entry.original=originals;
            entry.clone={out.vs,out.hs,out.ds,out.layout,out.raster,out.depth,out.blend};
        }
        const auto buffer=[&](std::shared_ptr<const void> image,D3D11_BUFFER_DESC d,const void* bytes,ComPtr<ID3D11Buffer>& outBuffer){
            for(const auto& entry:cache.buffers)if(entry.image.lock()==image&&entry.object){outBuffer=entry.object;return out.retention.Add(outBuffer.Get());}
            d.Usage=D3D11_USAGE_IMMUTABLE;d.CPUAccessFlags=0;D3D11_SUBRESOURCE_DATA data{bytes,0,0};
            // Eviction drops only cache references; actual submitted Work/Node
            // roots still own their immutable buffers through physical fences.
            for(auto& entry:cache.buffers)if(entry.image.expired()){
                entry.object.Reset();entry.image.reset();entry.retention={};
            }
            ComPtr<ID3D11Buffer> created;if(FAILED(device->CreateBuffer(&d,&data,&created)))return false;
            if(!out.retention.Add(created.Get())){cache.DropBuffers();if(!out.retention.Add(created.Get()))return false;}
            auto& entry=cache.buffers[cache.bufferCursor++%cache.buffers.size()];entry.object.Reset();entry.image.reset();entry.retention={};
            if(!entry.retention.Add(created.Get()))return false;entry.object=created;entry.image=std::move(image);outBuffer=std::move(created);return true;
        };
        for(unsigned i=0;i<33;++i)if(portable.geometry[i]){
            const auto& image=*portable.geometry[i];ComPtr<ID3D11Buffer> b;
            bool cached=false;
            for(const auto& entry:cache.buffers)if(entry.image.lock()==portable.geometry[i]&&entry.object){
                b=entry.object;cached=true;if(!out.retention.Add(b.Get()))return false;break;}
            if(!cached){
                auto expanded=GenshinDepthPortable::Allocate(image.desc.ByteWidth);if(!expanded)return false;
                std::memset(expanded->data.data(),0,image.desc.ByteWidth);
                // Unknown padding is never fetched: CapturePortable proved all
                // actual VS/layout/index address ranges before creating this image.
                for(const auto& p:image.parts)std::memcpy(expanded->data.data()+p.begin,
                    p.storage->data.data()+p.offset,p.end-p.begin);
                auto d=image.desc;d.BindFlags&=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER;
                if(!buffer(portable.geometry[i],d,expanded->data.data(),b))return false;
            }
            if(i==32)out.indices=std::move(b);else out.vertices[i]=std::move(b);
        }
        for(unsigned i=0;i<payload.size();++i)if(payload[i]){
            D3D11_BUFFER_DESC d{};originalConstants[i]->GetDesc(&d);
            if(!buffer(payload[i],d,payload[i]->bytes.data(),out.constants[i]))return false;
        }
        return true;
    }
    static bool Geometry(ID3D11Buffer* buffer,unsigned slot=UINT_MAX,bool index=false)noexcept {
        if(!buffer)return true;D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);
        // R831: capability flags are not writes. Extended IA storage must be
        // CPU-inaccessible DEFAULT with no shared/raw/structured alias flags.
        // Outputs versions actual CS/OM UAV and SO writes, including the original
        // draw; Map/copy/update/clear/discard and opaque boundaries revoke too.
        // Capture still rejects active OM UAV/SO and unsupported shader stages.
        constexpr UINT ia=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER;
        constexpr UINT extra=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_UNORDERED_ACCESS;
        const bool extended=(d.BindFlags&extra)!=0;
        const bool supported=d.ByteWidth && (d.BindFlags&ia) && !(d.BindFlags&~(ia|extra)) &&
            !d.MiscFlags && !d.StructureByteStride &&
            (extended?(d.Usage==D3D11_USAGE_DEFAULT && !d.CPUAccessFlags):
                (d.Usage==D3D11_USAGE_IMMUTABLE || d.Usage==D3D11_USAGE_DEFAULT || d.Usage==D3D11_USAGE_DYNAMIC));
        // One descriptor observation per IA role; reuse the original getter and predicate.
        if(!supported){
            static std::array<std::atomic<bool>,2> reported{};
            if(!reported[index?1:0].exchange(true,std::memory_order_relaxed))try{
                const auto call=CurrentDraw();
                // Failure-only receipt from the original descriptor; never grants admission.
                const UINT allowed=ia|extra,forbidden=d.BindFlags&~allowed;
                const char* first=!d.ByteWidth?"byte_width":!(d.BindFlags&ia)?"ia_bind":
                    forbidden?"forbidden_bind":d.MiscFlags?"misc_flags":d.StructureByteStride?"structure_byte_stride":
                    extended?(d.Usage!=D3D11_USAGE_DEFAULT?"extended_usage":d.CPUAccessFlags?"extended_cpu_access":"UNKNOWN"):
                        "ia_usage";
                LOG_INFO("R827 phase18 geometry refusal: pid={} role={} slot={} call={} context={} thread={} byte_width={} usage={} bind={} cpu={} misc={} stride={} first_predicate={} bind_hex=0x{:X} allowed_bind=0x{:X} forbidden_bind=0x{:X} forbidden_uav={} extended={} no_authority=true",
                    GetCurrentProcessId(),index?"index":"vertex",slot,call.observationCall,static_cast<void*>(call.context),call.thread,
                    d.ByteWidth,static_cast<unsigned>(d.Usage),d.BindFlags,d.CPUAccessFlags,d.MiscFlags,d.StructureByteStride,
                    first,d.BindFlags,allowed,forbidden,(forbidden&D3D11_BIND_UNORDERED_ACCESS)!=0,extended);
            }catch(...){ }
        }
        return supported;
    }
    template<class T,size_t N>static void Adopt(std::array<ComPtr<T>,N>& out,std::array<T*,N>& raw)noexcept {
        for(size_t i=0;i<N;++i)out[i].Attach(raw[i]);
    }
    // R827 phase14: first occurrence per refusal stage, observation only.
    // State/Buffer scalar reads are permitted only under the caller's existing Access lock.
    enum class RefusalStage : unsigned { CaptureEntry,CaptureShaderStages,CaptureDevice,CaptureUnsupported,CaptureViewport,CaptureVertexGeometry,CaptureVertexWatch,CaptureIndexGeometry,CaptureIndexWatch,CaptureShaderSize,CaptureShaderBytes,CaptureReflect,CaptureReflectDesc,CaptureBindingDesc,CaptureBindingType,CaptureAccessContext,CaptureCbFind,CaptureCbMappingUnknown,CaptureCbMapped,CaptureCbCount,CaptureCbRange,CaptureCbPayloadFailed,CaptureLoss,MaterializeReturned,MaterializeContext,MaterializeThread,MaterializeGeometry,MaterializeAccess,MaterializeSemanticContext,MaterializeLossEpoch,MaterializeCurrentLoss,MaterializePayloadPending,MaterializePayloadFailed,MaterializeBufferUnsupported,MaterializePayloadSize,MaterializeCreateBuffer, Count };
    bool Refuse(RefusalStage stage,unsigned slot=UINT_MAX,
                const GenshinDepthSemantic::detail::State* state=nullptr,
                const GenshinDepthSemantic::detail::Buffer* buffer=nullptr)const noexcept {
        static std::array<std::atomic<bool>,static_cast<unsigned>(RefusalStage::Count)> reported{};
        if(reported[static_cast<unsigned>(stage)].exchange(true,std::memory_order_relaxed))return false;
        static constexpr const char* names[]{ "CaptureEntry","CaptureShaderStages","CaptureDevice","CaptureUnsupported","CaptureViewport","CaptureVertexGeometry","CaptureVertexWatch","CaptureIndexGeometry","CaptureIndexWatch","CaptureShaderSize","CaptureShaderBytes","CaptureReflect","CaptureReflectDesc","CaptureBindingDesc","CaptureBindingType","CaptureAccessContext","CaptureCbFind","CaptureCbMappingUnknown","CaptureCbMapped","CaptureCbCount","CaptureCbRange","CaptureCbPayloadFailed","CaptureLoss","MaterializeReturned","MaterializeContext","MaterializeThread","MaterializeGeometry","MaterializeAccess","MaterializeSemanticContext","MaterializeLossEpoch","MaterializeCurrentLoss","MaterializePayloadPending","MaterializePayloadFailed","MaterializeBufferUnsupported","MaterializePayloadSize","MaterializeCreateBuffer" };
        unsigned occupied=0,mapped=0,unknown=0,payloads=0,external=0,ready=0,failed=0,flights=0,flightPayloads=0;
        long long payloadRefs=0;
        if(state){
            for(const auto& b:state->buffers)if(b.object){
                ++occupied;if(b.mapped)++mapped;if(!b.mappingKnown)++unknown;
                if(b.payload){++payloads;const auto refs=b.payload.use_count();payloadRefs+=refs;
                    if(refs>1)++external;if(b.payload->ready)++ready;if(b.payload->failed)++failed;}
            }
            for(const auto& f:state->flights)if(f.queued){++flights;for(const auto& p:f.destination)if(p)++flightPayloads;}
        }
        auto* input=slot<originalConstants.size()?originalConstants[slot].Get():nullptr;
        D3D11_BUFFER_DESC desc{};bool descKnown=false;
        if(buffer){desc=buffer->desc;descKnown=true;}
        // The sole added resource getter: bounded failure-only descriptor observation,
        // explicitly required for a null Find result; never feeds any admission gate.
        else if(stage==RefusalStage::CaptureCbFind && input){input->GetDesc(&desc);descKnown=true;}
        // Payload readiness/bytes are shared semantic state: observe only while its existing mutex is held.
        const auto* p=state?(buffer?buffer->payload.get():(slot<payload.size()?payload[slot].get():nullptr)):nullptr;
        try {
            LOG_INFO("R827 phase14 raster first refusal: pid={} stage={} call={} context={} thread={} slot={} input={} semantic_locked={} occupied={} capacity={} mapped_buffers={} mapping_unknown_buffers={} payload_buffers={} external_payload_buffers={} payload_refs={} ready_payloads={} failed_payloads={} queued_flights={} flight_payloads={} retained_loss={} state_loss_epoch={} current_loss={} version_epoch={} live_versions={} cb_known={} cb_mapping_known={} cb_mapped={} cb_revision={} retained_revision={} payload_present={} payload_ready={} payload_failed={} payload_bytes={} first={} count={} desc_known={} byte_width={} usage={} bind={} cpu={} misc={} stride={} no_authority=true",
                GetCurrentProcessId(),names[static_cast<unsigned>(stage)],draw.observationCall,static_cast<void*>(draw.context),draw.thread,slot,static_cast<void*>(input),
                state!=nullptr,occupied,state?state->buffers.size():0,mapped,unknown,payloads,external,payloadRefs,ready,failed,flights,flightPayloads,
                loss,state?state->lossEpoch:0,GenshinDepthSemantic::detail::loss.load(),versionEpoch.load(),liveVersions.load(std::memory_order_relaxed),
                buffer!=nullptr,buffer?buffer->mappingKnown:false,buffer?buffer->mapped:false,buffer?buffer->revision:0,
                slot<revisions.size()?revisions[slot]:0,p!=nullptr,p?p->ready:false,p?p->failed:false,p?p->bytes.size():0,
                slot<first.size()?first[slot]:0,slot<count.size()?count[slot]:0,descKnown,desc.ByteWidth,static_cast<unsigned>(desc.Usage),desc.BindFlags,desc.CPUAccessFlags,desc.MiscFlags,desc.StructureByteStride);
        }catch(...){ } // Observation cannot change a refused operation's result.
        return false;
    }
    bool CaptureCandidate(ID3D11DeviceContext* c) {
        draw=CurrentDraw();if(!c || !draw.Matches(c,draw.kind,draw.count) || FAILED(c->QueryInterface(IID_PPV_ARGS(&context))))return Refuse(RefusalStage::CaptureEntry);
        ComPtr<ID3D11GeometryShader> gs;UINT hsClasses=0,dsClasses=0;
        c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&hs,nullptr,&hsClasses);c->DSGetShader(&ds,nullptr,&dsClasses);
        if(gs || hsClasses || dsClasses || bool(hs)!=bool(ds))return Refuse(RefusalStage::CaptureShaderStages);
        std::array<ID3D11Buffer*,4> so{};c->SOGetTargets(4,so.data());bool unsupported=false;
        UINT soCount=0,omUavCount=0,vsSrvCount=0;
        for(auto* item:so)if(item){++soCount;unsupported=true;item->Release();}
        ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;c->GetPredication(&predicate,&value);if(predicate)unsupported=true;
        ComPtr<ID3D11Device> device;c->GetDevice(&device);if(!device)return Refuse(RefusalStage::CaptureDevice);
        const UINT slots=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
        std::array<ID3D11UnorderedAccessView*,64> uav{};c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,uav.data());
        for(auto* item:uav)if(item){++omUavCount;unsupported=true;item->Release();}
        // R830: bound but unread VS SRVs are not shader dependencies. The
        // actual bytecode reflection below still rejects every non-CB input.
        // Count residual bindings only on an already refused path; they do
        // not grant replay or numerical/camera authority.
        std::array<ID3D11ClassInstance*,256> classes{};UINT n=256;c->VSGetShader(&vs,classes.data(),&n);
        for(auto* item:classes)if(item)item->Release();if(n)unsupported=true;
        const UINT vsClassCount=n;
        classes={};n=256;c->PSGetShader(&ps,classes.data(),&n);
        for(auto* item:classes)if(item)item->Release();if(n || !vs || !ps || unsupported){
            std::array<ID3D11ShaderResourceView*,128> vsViews{};c->VSGetShaderResources(0,128,vsViews.data());
            for(auto* item:vsViews)if(item){++vsSrvCount;item->Release();}
            // Counts/flags come only from the existing collection above, before refusal.
            static std::atomic<bool> reported{false};
            if(!reported.exchange(true,std::memory_order_relaxed))try{
                LOG_INFO("R827 phase18 capture unsupported: pid={} call={} context={} thread={} unsupported={} so_count={} predicated={} predicate_value={} om_uav_count={} om_uav_slots={} vs_srv_count={} vs_class_count={} ps_class_count={} vs_present={} ps_present={} no_authority=true",
                    GetCurrentProcessId(),draw.observationCall,static_cast<void*>(draw.context),draw.thread,
                    unsupported,soCount,predicate.Get()!=nullptr,value,omUavCount,slots,vsSrvCount,vsClassCount,n,
                    vs.Get()!=nullptr,ps.Get()!=nullptr);
            }catch(...){ }
            return Refuse(RefusalStage::CaptureUnsupported);
        }
        c->IAGetInputLayout(&layout);c->RSGetState(&raster);c->IAGetPrimitiveTopology(&topology);
        const bool patch=topology>=D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && topology<=D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
        if(bool(ds)!=patch)return Refuse(RefusalStage::CaptureShaderStages);
        c->RSGetViewports(&viewportCount,viewports.data());c->RSGetScissorRects(&scissorCount,scissors.data());
        if(!viewportCount || viewportCount>16 || scissorCount>16)return Refuse(RefusalStage::CaptureViewport);
        c->OMGetDepthStencilState(&depth,&reference);c->OMGetBlendState(&blend,factors,&sampleMask);
        std::array<ID3D11RenderTargetView*,8> targets{};c->OMGetRenderTargets(8,targets.data(),&dsv);Adopt(rtvs,targets);
        std::array<ID3D11Buffer*,32> buffers{};c->IAGetVertexBuffers(0,32,buffers.data(),strides.data(),offsets.data());Adopt(vertices,buffers);
        c->IAGetIndexBuffer(&indices,&indexFormat,&indexOffset);
        CapturePortable();
        // With the actual VS+creation layout proving the dependency set, stale
        // bindings are not inputs. A missing proof retains the old conservative
        // capture behavior. No unknown buffer is promoted to a known version.
        if(portable.dependenciesKnown){
            for(UINT i=0;i<32;++i)if(!(portable.slots&(uint32_t(1)<<i)))vertices[i].Reset();
            if(draw.kind!=2&&draw.kind!=4)indices.Reset();
        }
        for(unsigned i=0;i<32;++i)if(vertices[i]){if(!(portable.ready&&portable.geometry[i])&&!Geometry(vertices[i].Get(),i,false))return Refuse(RefusalStage::CaptureVertexGeometry);geometry[i]=Watch(vertices[i].Get());if(!geometry[i])return Refuse(RefusalStage::CaptureVertexWatch);}
        if(indices){if(!(portable.ready&&portable.geometry[32])&&!Geometry(indices.Get(),0,true))return Refuse(RefusalStage::CaptureIndexGeometry);geometry[32]=Watch(indices.Get());if(!geometry[32])return Refuse(RefusalStage::CaptureIndexWatch);}
        std::array<bool,42> used{};
        // Complete real pre-raster dependencies, each stage in one 14-slot
        // getter. VS-only does no additional HS/DS constant-buffer calls.
        for(UINT stage=0;stage<(ds?3u:1u);++stage){
            std::array<ID3D11Buffer*,14> cb{};const UINT base=stage*14;
            switch(stage){
                case 0:context->VSGetConstantBuffers1(0,14,cb.data(),first.data()+base,count.data()+base);break;
                case 1:context->HSGetConstantBuffers1(0,14,cb.data(),first.data()+base,count.data()+base);break;
                case 2:context->DSGetConstantBuffers1(0,14,cb.data(),first.data()+base,count.data()+base);break;
            }
            for(UINT i=0;i<14;++i)originalConstants[base+i].Attach(cb[i]);
            using Stage=GenshinDepthSemantic::detail::Stage;
            ID3D11DeviceChild* shader=stage==0?static_cast<ID3D11DeviceChild*>(vs.Get()):
                stage==1?static_cast<ID3D11DeviceChild*>(hs.Get()):static_cast<ID3D11DeviceChild*>(ds.Get());
            const auto program=GenshinDepthSemantic::detail::Shader(shader,
                stage==0?Stage::ControlVertex:stage==1?Stage::Hull:Stage::Domain);
            if(!program || !program->interfaceValid || !program->reflection)return Refuse(RefusalStage::CaptureReflect);
            D3D11_SHADER_DESC sd{};if(FAILED(program->reflection->GetDesc(&sd)))return Refuse(RefusalStage::CaptureReflectDesc);
            for(UINT i=0;i<sd.BoundResources;++i){D3D11_SHADER_INPUT_BIND_DESC resource{};
                if(FAILED(program->reflection->GetResourceBindingDesc(i,&resource)))return Refuse(RefusalStage::CaptureBindingDesc);
                if(resource.Type!=D3D_SIT_CBUFFER || resource.BindCount!=1 || resource.BindPoint>=14)return Refuse(RefusalStage::CaptureBindingType);
                used[base+resource.BindPoint]=true;
            }
        }
        namespace S=GenshinDepthSemantic::detail;S::Access access;if(!access || !S::Context(access.s,c))return Refuse(RefusalStage::CaptureAccessContext,UINT_MAX,access.lock.owns_lock()?&access.s:nullptr);loss=access.s.lossEpoch;
        for(unsigned i=0;i<used.size();++i)if(used[i]){
            auto* b=S::Find(access.s,originalConstants[i].Get(),true);
            if(!b)return Refuse(RefusalStage::CaptureCbFind,i,&access.s);
            if(!b->mappingKnown)return Refuse(RefusalStage::CaptureCbMappingUnknown,i,&access.s,b);
            if(b->mapped)return Refuse(RefusalStage::CaptureCbMapped,i,&access.s,b);
            if(!count[i])return Refuse(RefusalStage::CaptureCbCount,i,&access.s,b);
            if(uint64_t(first[i])*16>=b->desc.ByteWidth)return Refuse(RefusalStage::CaptureCbRange,i,&access.s,b);
            if(!b->payload)b->payload=S::NewPayload(b->desc.ByteWidth);
            if(!b->payload)return Refuse(RefusalStage::CaptureCbPayloadFailed,i,&access.s,b);
            if(b->payload->failed)return Refuse(RefusalStage::CaptureCbPayloadFailed,i,&access.s,b);payload[i]=b->payload;revisions[i]=b->revision;
        }
        std::array<ID3D11ShaderResourceView*,128> views{};c->PSGetShaderResources(0,128,views.data());Adopt(psViews,views);
        std::array<ID3D11SamplerState*,16> states{};c->PSGetSamplers(0,16,states.data());Adopt(samplers,states);
        const bool sameLoss=loss==S::loss.load();
        if(!sameLoss)return Refuse(RefusalStage::CaptureLoss,UINT_MAX,&access.s);
        if(!retention.Add(draw.indirectBuffer.Get()) || !retention.Add(indices.Get()))return false;
        for(const auto& v:vertices)if(!retention.Add(v.Get()))return false;
        for(const auto& cb:originalConstants)if(!retention.Add(cb.Get()))return false;
        // Buffer-backed views own allocations too, even if replay later refuses.
        const auto retainView=[&](ID3D11View* view){
            if(!view)return true;ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
            if(!resource)return false;ComPtr<ID3D11Buffer> buffer;
            const auto hr=resource.As(&buffer);
            return SUCCEEDED(hr)?retention.Add(buffer.Get()):hr==E_NOINTERFACE;
        };
        for(const auto& view:psViews)if(!retainView(view.Get()))return false;
        for(const auto& view:rtvs)if(!retainView(view.Get()))return false;
        return true;
    }
    bool Capture(ID3D11DeviceContext* c) {
        *this={};Raster candidate;
        // Refused partial getter owners must not become persistent snapshots.
        if(!candidate.CaptureCandidate(c))return false;
        *this=std::move(candidate);return true;
    }
    bool Returned(ID3D11DeviceContext* c) {
        namespace S=GenshinDepthSemantic::detail;S::Access access;
        if(!access || returned || !S::Context(access.s,c) || loss!=access.s.lossEpoch || loss!=S::loss.load() ||
           draw.context!=c || draw.thread!=GetCurrentThreadId() || draw.observationCall!=CurrentDraw().observationCall)return false;
        for(unsigned i=0;i<payload.size();++i)if(payload[i]){
            auto* b=S::Find(access.s,originalConstants[i].Get());
            if(!b || !b->mappingKnown || b->mapped || b->revision!=revisions[i] || b->payload!=payload[i] || payload[i]->failed)return false;
        }
        for(unsigned i=0;i<payload.size();++i)if(payload[i]){auto* b=S::Find(access.s,originalConstants[i].Get());
            b->returnedPayload=payload[i];b->returnedRevision=revisions[i];b->returnedLoss=loss;}
        PortableReturned(); // optional migration failure never weakens or blocks the game-device route
        returned=true;return true;
    }
    enum class Readiness:unsigned {Ready,Pending,Invalid};
    struct PreflightResult {
        Readiness readiness;RefusalStage stage=RefusalStage::MaterializeReturned;unsigned slot=UINT_MAX;
    };
    // Caller holds the existing semantic Access lock for the whole inspection.
    // No Find, retention charge, allocation, GPU work or refusal logging here.
    PreflightResult Preflight(ID3D11DeviceContext* c,const GenshinDepthSemantic::detail::State& state,
                              std::array<D3D11_BUFFER_DESC,42>* descriptors=nullptr,bool immutableGeometry=false)const noexcept {
        if(!returned)return {Readiness::Invalid,RefusalStage::MaterializeReturned};
        if(draw.context!=c)return {Readiness::Invalid,RefusalStage::MaterializeContext};
        if(draw.thread!=GetCurrentThreadId())return {Readiness::Invalid,RefusalStage::MaterializeThread};
        if(!immutableGeometry)for(const auto& item:geometry)if(item && !Intact(item))return {Readiness::Invalid,RefusalStage::MaterializeGeometry};
        namespace S=GenshinDepthSemantic::detail;
        if(!S::Context(state,c))return {Readiness::Invalid,RefusalStage::MaterializeSemanticContext};
        if(state.lossEpoch!=loss)return {Readiness::Invalid,RefusalStage::MaterializeLossEpoch};
        if(loss!=S::loss.load())return {Readiness::Invalid,RefusalStage::MaterializeCurrentLoss};
        unsigned pending=UINT_MAX;
        for(unsigned i=0;i<payload.size();++i)if(payload[i]){
            // Inspect every real dependency before returning Pending: a later
            // failed/unsupported/size-invalid slot must win over an earlier wait.
            if(payload[i]->failed)return {Readiness::Invalid,RefusalStage::MaterializePayloadFailed,i};
            if(!originalConstants[i])return {Readiness::Invalid,RefusalStage::MaterializeBufferUnsupported,i};
            D3D11_BUFFER_DESC d{};originalConstants[i]->GetDesc(&d);
            if(!S::Supported(d))return {Readiness::Invalid,RefusalStage::MaterializeBufferUnsupported,i};
            if(payload[i]->bytes.size()!=d.ByteWidth)return {Readiness::Invalid,RefusalStage::MaterializePayloadSize,i};
            if(descriptors)(*descriptors)[i]=d;
            if(!payload[i]->ready && pending==UINT_MAX)pending=i;
        }
        if(pending!=UINT_MAX)return {Readiness::Pending,RefusalStage::MaterializePayloadPending,pending};
        return {Readiness::Ready};
    }
    bool Materialize(ID3D11DeviceContext* c) {
        namespace S=GenshinDepthSemantic::detail;S::Access access;
        if(!access)return Refuse(RefusalStage::MaterializeAccess,UINT_MAX,access.lock.owns_lock()?&access.s:nullptr);
        // Recheck under the same lock as creation; admission is not a promise
        // that a prior inspection remains valid after releasing its lock.
        // Only actual payload slots are filled and consumed after Ready.
        std::array<D3D11_BUFFER_DESC,42> descriptors;
        const auto check=Preflight(c,access.s,&descriptors);
        if(check.readiness!=Readiness::Ready)return Refuse(check.stage,check.slot,&access.s);
        for(unsigned i=0;i<payload.size();++i)if(payload[i]){
            if(constants[i]){if(!retention.Add(constants[i].Get()))return false;continue;}
            auto d=descriptors[i];
            d.Usage=D3D11_USAGE_IMMUTABLE;d.CPUAccessFlags=0;
            D3D11_SUBRESOURCE_DATA data{payload[i]->bytes.data(),0,0};
            ComPtr<ID3D11Buffer> next;
            if(FAILED(access.s.device->CreateBuffer(&d,&data,&next)))return Refuse(RefusalStage::MaterializeCreateBuffer,i,&access.s);
            if(!retention.Add(next.Get()))return false;
            constants[i]=std::move(next);
        }
        return true;
    }
    void Apply()const noexcept {
        auto* c=context.Get();c->ClearState();
        c->VSSetShader(vs.Get(),nullptr,0);c->HSSetShader(hs.Get(),nullptr,0);c->DSSetShader(ds.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->IASetInputLayout(layout.Get());c->IASetPrimitiveTopology(topology);
        std::array<ID3D11Buffer*,32> vb{};for(unsigned i=0;i<32;++i)vb[i]=vertices[i].Get();
        c->IASetVertexBuffers(0,32,vb.data(),strides.data(),offsets.data());c->IASetIndexBuffer(indices.Get(),indexFormat,indexOffset);
        std::array<ID3D11Buffer*,42> cb{};for(unsigned i=0;i<cb.size();++i)cb[i]=constants[i].Get();
        c->VSSetConstantBuffers1(0,14,cb.data(),first.data(),count.data());
        if(ds){c->HSSetConstantBuffers1(0,14,cb.data()+14,first.data()+14,count.data()+14);
            c->DSSetConstantBuffers1(0,14,cb.data()+28,first.data()+28,count.data()+28);}
        c->RSSetState(raster.Get());c->RSSetViewports(viewportCount,viewports.data());c->RSSetScissorRects(scissorCount,scissors.data());
        c->OMSetDepthStencilState(depth.Get(),reference);c->OMSetBlendState(blend.Get(),factors,sampleMask);
        std::array<ID3D11RenderTargetView*,8> targets{};for(unsigned i=0;i<8;++i)targets[i]=rtvs[i].Get();c->OMSetRenderTargets(8,targets.data(),dsv.Get());
        std::array<ID3D11ShaderResourceView*,128> views{};for(unsigned i=0;i<128;++i)views[i]=psViews[i].Get();c->PSSetShaderResources(0,128,views.data());
        std::array<ID3D11SamplerState*,16> states{};for(unsigned i=0;i<16;++i)states[i]=samplers[i].Get();c->PSSetSamplers(0,16,states.data());
    }
};
}
