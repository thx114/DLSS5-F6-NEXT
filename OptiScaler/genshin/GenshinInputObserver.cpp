// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinInputObserver.h"
#include <d3d11on12.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <climits>

namespace GenshinLegacy::RuntimeCapture {
inline constexpr GUID RawShaderKey={0xd9f2cf63,0x754c,0x4de9,{0xb4,0xaf,0x91,0x23,0x18,0x7c,0x44,0x65}};
inline constexpr GUID RawShaderIdKey={0xd9f2cf64,0x754c,0x4de9,{0xb4,0xaf,0x91,0x23,0x18,0x7c,0x44,0x65}};
inline constexpr size_t RawCacheBytes=16*1024*1024,MaxInputBytes=128*1024*1024,MaxPendingBytes=48*1024*1024,MaxPixelBytes=32*1024*1024;
inline constexpr unsigned MaxInputDraws=8,MaxGpuSlots=48,MaxPolls=120;
struct RawShaderId { uint64_t sequence=0,hash=0,bytes=0,deviceEpoch=0,sessionTick=0;unsigned stage=0; };
// Private data belongs to the COM child: address reuse cannot inherit a previous object's bytes.
// Cache accounting is monotonic for the session; it bounds memory even with heavy shader churn.
void RawShaderCreated(ID3D11DeviceChild* shader,const void* bytes,size_t size,uint64_t epoch,unsigned stage)noexcept {
    if(!Enabled() || !shader)return;
    if(stage>2 || !epoch)return;
    auto& s=Get();RawShaderId id{++s.rawCreateSequence,0,size,epoch,s.startTick,stage};
    if(!bytes || !size || size>MaxShaderBytes || s.rawCacheObjects.load()>=256 || size>RawCacheBytes-s.rawCacheBytes.load()){
        ++s.rawCacheDrops;return;
    }
    s.rawCacheBytes+=size;++s.rawCacheObjects;
    id.hash=Family::Hash(bytes,size);
    if(FAILED(shader->SetPrivateData(RawShaderKey,static_cast<UINT>(size),bytes)) ||
        FAILED(shader->SetPrivateData(RawShaderIdKey,sizeof(id),&id)))++s.rawCacheDrops;
}
inline Json StageIdentity(ID3D11DeviceChild* shader) {
    Json j={{"object",reinterpret_cast<uintptr_t>(shader)},{"status",shader?"UNKNOWN_CREATE_NOT_CAPTURED":"UNBOUND"}};
    if(!shader)return j;RawShaderId id{};UINT n=sizeof(id);
    if(SUCCEEDED(shader->GetPrivateData(RawShaderIdKey,&n,&id)) && n==sizeof(id)){
        j["createSequence"]=id.sequence;j["fnv64"]=id.hash;j["bytes"]=id.bytes;j["deviceEpoch"]=id.deviceEpoch;
        j["captureSessionTick"]=id.sessionTick;
        j["status"]=id.sessionTick==Get().startTick?"CREATE_RAW_CACHED":"CREATE_RAW_CACHED_PRIOR_CAPTURE";
    }
    return j;
}
struct ByteRange { UINT first=0,count=0,offset=0,bytes=0;bool valid=false; };
inline ByteRange EffectiveRange(UINT width,UINT first,UINT count)noexcept {
    ByteRange r{first,count};
    if(!width || width>65536 || width%16 || first>UINT_MAX/16 || count>UINT_MAX/16)return r;
    const uint64_t start=static_cast<uint64_t>(first)*16,requested=static_cast<uint64_t>(count)*16;
    r.offset=static_cast<UINT>((std::min)(start,static_cast<uint64_t>(width)));
    r.bytes=static_cast<UINT>((std::min)(requested,static_cast<uint64_t>(width)-r.offset));r.valid=true;return r;
}
inline unsigned BytesPerPixel(DXGI_FORMAT f)noexcept {
    switch(f){
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:case DXGI_FORMAT_R32G32B32A32_FLOAT:case DXGI_FORMAT_R32G32B32A32_UINT:case DXGI_FORMAT_R32G32B32A32_SINT:return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS:case DXGI_FORMAT_R32G32B32_FLOAT:case DXGI_FORMAT_R32G32B32_UINT:case DXGI_FORMAT_R32G32B32_SINT:return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R16G16B16A16_UINT:case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R32G32_UINT:case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:return 8;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_UINT:case DXGI_FORMAT_R8G8B8A8_SNORM:case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R10G10B10A2_UINT:case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16G16_UNORM:case DXGI_FORMAT_R16G16_UINT:case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_UINT:case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:case DXGI_FORMAT_X24_TYPELESS_G8_UINT:return 4;
    case DXGI_FORMAT_R8G8_TYPELESS:case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_UINT:case DXGI_FORMAT_R8G8_SNORM:case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_D16_UNORM:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_UINT:case DXGI_FORMAT_R16_SNORM:case DXGI_FORMAT_R16_SINT:return 2;
    case DXGI_FORMAT_R8_TYPELESS:case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_UINT:case DXGI_FORMAT_R8_SNORM:case DXGI_FORMAT_R8_SINT:case DXGI_FORMAT_A8_UNORM:return 1;
    default:return 0;
    }
}
inline bool PixelSize(UINT width,UINT height,unsigned bpp,size_t& row,size_t& total)noexcept {
    if(!width || !height || !bpp || width>MaxPixelBytes/bpp)return false;
    row=static_cast<size_t>(width)*bpp;if(height>MaxPixelBytes/row)return false;total=row*height;return true;
}
inline Json ViewBinding(ID3D11View* view) {
    Json j={{"view",reinterpret_cast<uintptr_t>(view)},{"resource",0}};if(!view)return j;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);j["resource"]=reinterpret_cast<uintptr_t>(resource.Get());
    D3D11_RESOURCE_DIMENSION kind{};resource->GetType(&kind);j["resourceDimension"]=static_cast<unsigned>(kind);
    ComPtr<ID3D11Texture2D> texture;if(SUCCEEDED(resource.As(&texture))){D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        j["texture2D"]={{"width",d.Width},{"height",d.Height},{"mips",d.MipLevels},{"arraySize",d.ArraySize},{"format",d.Format},
            {"sampleCount",d.SampleDesc.Count},{"sampleQuality",d.SampleDesc.Quality},{"bindFlags",d.BindFlags},{"miscFlags",d.MiscFlags}};}
    return j;
}
struct InputObserver::Storage final {
    struct Slot {
        ComPtr<ID3D11Resource> source,staging;
        ComPtr<ID3D11Query> query;
        DrawFact draw{};Json metadata;std::wstring name;
        size_t bytes=0,rowBytes=0;UINT height=1,offset=0;uint64_t queued=0;unsigned polls=0;
    };
    std::array<Slot*,MaxGpuSlots> slots_{};size_t pendingBytes_=0;uint64_t blob_=0;
    std::array<uint64_t,24> exportedShaders_{};unsigned shaderCount_=0;
    static void Quarantine(Slot*& slot)noexcept {
        if(!slot)return;
        // Deliberate bounded process-lifetime isolation: never destroy unfinished GPU commands on timeout/exit.
        // Monotonic session reservation bounds all abandoned source/staging/query leases.
        ++Get().gpuQuarantined;--Get().gpuPending;slot=nullptr;
    }
    void Emit(const DrawFact& draw,Json metadata,std::vector<uint8_t> raw={},std::wstring name={})noexcept {
        try{Job j;j.kind=Job::Kind::Artifact;j.draw=draw;j.eventTick=draw.observationTick;j.readbackTick=GetTickCount64();
            j.metadata=std::move(metadata);j.metadataBytes=j.metadata.dump().size();j.raw=std::move(raw);j.artifactName=std::move(name);
            if(j.metadataBytes>MaxArtifactMetadataBytes || !Enqueue(std::move(j)))++Get().inputDrops;
        }catch(...){++Get().allocationFailures;}
    }
    std::wstring Name(const DrawFact& d,const std::wstring& suffix){return L"input-"+std::to_wstring(d.drawSerial)+L"-"+std::to_wstring(++blob_)+suffix;}
    bool Reserve(size_t bytes)noexcept {
        auto& s=Get();if(bytes>MaxPendingBytes-pendingBytes_ || bytes>MaxInputBytes-s.inputBytesReserved.load())return false;
        // Process-wide monotonic budget also bounds quarantined leases across rearms.
        static size_t processReserved=0;
        if(bytes>MaxInputBytes-processReserved)return false;
        processReserved+=bytes;s.inputBytesReserved+=bytes;return true;
    }
    Slot** FreeSlot()noexcept {for(auto& slot:slots_)if(!slot)return &slot;return nullptr;}
    void Failure(const DrawFact& d,Json m,const char* why,HRESULT hr=S_OK) {
        ++Get().inputDrops;m["status"]=why;m["HRESULT"]=static_cast<int32_t>(hr);m["gpuReadbackComplete"]=false;Emit(d,std::move(m));
    }
    void FailureNoThrow(const DrawFact& d,const Json& metadata,const char* why,HRESULT hr=S_OK)noexcept {
        try{Failure(d,metadata,why,hr);}catch(...){++Get().allocationFailures;}
    }
    void Shader(const DrawFact& d,const char* stage,ID3D11DeviceChild* shader,Json& binding) {
        binding=StageIdentity(shader);if(!shader)return;RawShaderId id{};UINT size=sizeof(id);
        if(FAILED(shader->GetPrivateData(RawShaderIdKey,&size,&id)) || size!=sizeof(id) || id.deviceEpoch!=d.deviceEpoch || id.sessionTick!=Get().startTick)return;
        for(unsigned i=0;i<shaderCount_;++i)if(exportedShaders_[i]==id.sequence)return;
        if(shaderCount_>=exportedShaders_.size()){binding["status"]="SHADER_EXPORT_LIMIT";return;}
        std::vector<uint8_t> raw(static_cast<size_t>(id.bytes));UINT bytes=static_cast<UINT>(raw.size());
        if(FAILED(shader->GetPrivateData(RawShaderKey,&bytes,raw.data())) || bytes!=raw.size()){binding["status"]="RAW_CACHE_READ_FAILED";return;}
        exportedShaders_[shaderCount_++]=id.sequence;
        Json m={{"kind","target-bound-shader"},{"stage",stage},{"shader",binding},{"status","RAW_CREATE_BYTES"},{"gpuReadbackComplete",false}};
        Emit(d,std::move(m),std::move(raw),L"shader-"+std::to_wstring(id.sequence)+L".cso");
    }
    void Buffer(ID3D11Device* device,ID3D11DeviceContext* context,const DrawFact& d,ID3D11Buffer* source,const ByteRange& range,Json m) {
        ComPtr<ID3D11Device> owner;source->GetDevice(&owner);
        if(owner.Get()!=device){Failure(d,std::move(m),"UNKNOWN_CB_OWNER");return;}
        if(!range.valid){Failure(d,std::move(m),"UNKNOWN_INVALID_CB_RANGE");return;}
        if(!range.bytes){m["status"]="EMPTY_EFFECTIVE_RANGE";Emit(d,std::move(m));return;}
        auto** target=FreeSlot();D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
        if(!target || !Reserve(desc.ByteWidth)){Failure(d,std::move(m),"DROP_CB_SLOT_OR_BYTE_BUDGET");return;}
        auto job=std::make_unique<Slot>();ComPtr<ID3D11Buffer> staging;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        auto hr=device->CreateBuffer(&desc,nullptr,&staging);if(FAILED(hr)){Failure(d,std::move(m),"DROP_CB_STAGING_CREATE",hr);return;}
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};hr=device->CreateQuery(&q,&job->query);
        if(FAILED(hr)){Failure(d,std::move(m),"DROP_QUERY_CREATE",hr);return;}
        job->source=source;job->staging=staging;job->draw=d;job->metadata=std::move(m);job->metadata["effectiveByteOffset"]=range.offset;
        job->metadata["effectiveByteCount"]=range.bytes;job->bytes=desc.ByteWidth;job->rowBytes=range.bytes;job->offset=range.offset;
        job->name=Name(d,L".cb.bin");job->queued=GetTickCount64();
        pendingBytes_+=job->bytes;++Get().gpuPending;*target=job.release();
        context->CopyResource((*target)->staging.Get(),source);context->End((*target)->query.Get());
    }
    void Pixels(ID3D11Device* device,ID3D11DeviceContext* context,const DrawFact& d,ID3D11ShaderResourceView* view,unsigned index) {
        Json m={{"kind","target-input-pixels"},{"candidateInputIndex",index},{"candidateMeaning","colour/depth/motion candidate in legacy API order; semantics UNKNOWN"},
            {"binding",ViewBinding(view)},{"completeMutationCoverage",false},{"writerGeneration","UNKNOWN"}};
        if(!view){Failure(d,std::move(m),"UNKNOWN_UNBOUND_INPUT");return;}
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);m["viewFormat"]=v.Format;m["viewDimension"]=v.ViewDimension;
        if(v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D){Failure(d,std::move(m),"UNKNOWN_UNSUPPORTED_VIEW_DIMENSION");return;}
        ComPtr<ID3D11Resource> source;view->GetResource(&source);ComPtr<ID3D11Texture2D> texture;
        if(FAILED(source.As(&texture))){Failure(d,std::move(m),"UNKNOWN_NOT_TEXTURE2D");return;}
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);const UINT mip=v.Texture2D.MostDetailedMip;
        ComPtr<ID3D11Device> owner;source->GetDevice(&owner);
        if(owner.Get()!=device){Failure(d,std::move(m),"UNKNOWN_RESOURCE_OWNER");return;}
        if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || mip!=0 || mip>=desc.MipLevels ||
            (desc.MiscFlags&(D3D11_RESOURCE_MISC_TILED|D3D11_RESOURCE_MISC_TILE_POOL|D3D11_RESOURCE_MISC_HW_PROTECTED))){
            Failure(d,std::move(m),"UNKNOWN_MSAA_ARRAY_TILED_OR_PROTECTED");return;}
        size_t row=0,bytes=0;const UINT width=(std::max)(1u,desc.Width>>mip),height=(std::max)(1u,desc.Height>>mip);
        if(!PixelSize(width,height,BytesPerPixel(desc.Format),row,bytes)){Failure(d,std::move(m),"UNKNOWN_FORMAT_OR_SIZE_LIMIT");return;}
        auto** target=FreeSlot();if(!target || !Reserve(bytes)){Failure(d,std::move(m),"DROP_PIXEL_SLOT_OR_BYTE_BUDGET");return;}
        auto job=std::make_unique<Slot>();ComPtr<ID3D11Texture2D> staging;
        auto stageDesc=desc;stageDesc.Width=width;stageDesc.Height=height;stageDesc.ArraySize=stageDesc.MipLevels=1;
        stageDesc.Usage=D3D11_USAGE_STAGING;stageDesc.BindFlags=stageDesc.MiscFlags=0;stageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        auto hr=device->CreateTexture2D(&stageDesc,nullptr,&staging);if(FAILED(hr)){Failure(d,std::move(m),"UNKNOWN_STAGING_FORMAT_UNSUPPORTED",hr);return;}
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};hr=device->CreateQuery(&q,&job->query);
        if(FAILED(hr)){Failure(d,std::move(m),"DROP_QUERY_CREATE",hr);return;}
        m["resourceFormat"]=desc.Format;m["mip"]=mip;m["arraySlice"]=0;m["sourceSubresource"]=mip;
        m["sourceRectangle"]={0,0,width,height};m["tightRowBytes"]=row;m["height"]=height;m["sourceWriteWitness"]=WitnessJson(d.sourceWrites[index]);
        job->source=source;job->staging=staging;job->draw=d;job->metadata=std::move(m);job->bytes=bytes;job->rowBytes=row;job->height=height;
        job->name=Name(d,L".pixels.raw");job->queued=GetTickCount64();pendingBytes_+=bytes;++Get().gpuPending;*target=job.release();
        // Full selected subresource. No depth partial-box copy, resolve, format conversion or preview resampling.
        context->CopySubresourceRegion((*target)->staging.Get(),0,0,0,0,source.Get(),mip,nullptr);context->End((*target)->query.Get());
    }
public:
    ~Storage(){for(auto& slot:slots_)Quarantine(slot);}
    void Revoke()noexcept {for(auto* slot:slots_)if(slot)slot->draw.invalidated=true;}
    void CheckGeneration(uint64_t(*generation)(uint64_t))noexcept {
        for(auto* slot:slots_)if(slot && generation(slot->draw.instance)!=slot->draw.generation)slot->draw.invalidated=true;
    }
    void Queue(ID3D11Device* device,ID3D11DeviceContext* context,const DrawFact& d,const std::array<ID3D11ShaderResourceView*,3>& inputs)noexcept {
        if(!Enabled() || !device || !context || d.configEpoch!=Get().configEpoch)return;
        if(reinterpret_cast<uintptr_t>(device)!=d.device || reinterpret_cast<uintptr_t>(context)!=d.context)return;
        auto& s=Get();++s.inputOffers;if(s.inputDraws.fetch_add(1)>=MaxInputDraws){++s.inputDrops;return;}
        try{
            Json m={{"kind","target-pre-draw-bindings"},{"gpuReadbackComplete",false},{"completeMutationCoverage",false},
                {"capturePoint","before original Draw; immutable API binding facts"},{"uncoveredWrites",{"deferred contexts","command lists","indirect","aliases","unhooked APIs"}},
                {"cameraConvention","UNKNOWN; no default near/far/FOV/exposure"},{"dispatchEvidence","NONE: this is an actual draw snapshot; bound CS is not a dispatch"}};
            if(GetCurrentThreadId()!=d.thread || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){Failure(d,std::move(m),"UNKNOWN_WRONG_THREAD_OR_DEFERRED");return;}
            ComPtr<ID3D11Device> owner;context->GetDevice(&owner);if(owner.Get()!=device){Failure(d,std::move(m),"UNKNOWN_DEVICE_OWNER");return;}
            const auto removed=device->GetDeviceRemovedReason();if(FAILED(removed)){Failure(d,std::move(m),"DROP_DEVICE_REMOVED",removed);return;}
            ComPtr<ID3D11On12Device> on12;if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&on12)))){Failure(d,std::move(m),"UNKNOWN_ON12_UNSUPPORTED");return;}
            ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11ComputeShader> cs;
            context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);context->CSGetShader(&cs,nullptr,nullptr);
            Shader(d,"VS",vs.Get(),m["VS"]);Shader(d,"PS",ps.Get(),m["PS"]);Shader(d,"CS",cs.Get(),m["CS"]);
            ComPtr<ID3D11InputLayout> layout;context->IAGetInputLayout(&layout);m["inputLayoutObject"]=reinterpret_cast<uintptr_t>(layout.Get());m["inputLayoutDescription"]="UNKNOWN_NOT_HOOKED";
            std::array<ID3D11RenderTargetView*,8> rtvs{};ComPtr<ID3D11DepthStencilView> dsv;context->OMGetRenderTargets(8,rtvs.data(),&dsv);
            m["MRT"]=Json::array();for(unsigned i=0;i<8;++i){ComPtr<ID3D11RenderTargetView> r;r.Attach(rtvs[i]);auto b=ViewBinding(r.Get());b["slot"]=i;
                if(r){D3D11_RENDER_TARGET_VIEW_DESC desc{};r->GetDesc(&desc);b["viewFormat"]=desc.Format;b["viewDimension"]=desc.ViewDimension;
                    b["rawViewDescriptionHex"]=Hex(&desc,sizeof(desc));}m["MRT"].push_back(std::move(b));}
            m["DSV"]=ViewBinding(dsv.Get());if(dsv){D3D11_DEPTH_STENCIL_VIEW_DESC desc{};dsv->GetDesc(&desc);m["DSV"]["viewFormat"]=desc.Format;m["DSV"]["rawViewDescriptionHex"]=Hex(&desc,sizeof(desc));}
            UINT count=16;std::array<D3D11_VIEWPORT,16> viewports{};context->RSGetViewports(&count,viewports.data());m["viewports"]=Json::array();
            for(UINT i=0;i<(std::min)(count,16u);++i){const auto& v=viewports[i];m["viewports"].push_back({v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth});}
            count=16;std::array<D3D11_RECT,16> rects{};context->RSGetScissorRects(&count,rects.data());m["scissors"]=Json::array();
            for(UINT i=0;i<(std::min)(count,16u);++i){const auto& r=rects[i];m["scissors"].push_back({r.left,r.top,r.right,r.bottom});}
            // Reserve pixel slots before potentially numerous bound CBs.
            for(unsigned i=0;i<3;++i)Pixels(device,context,d,inputs[i],i);
            ComPtr<ID3D11DeviceContext1> c1;context->QueryInterface(IID_PPV_ARGS(&c1));m["CB"]=Json::array();
            for(unsigned stage=0;stage<3;++stage){std::array<ID3D11Buffer*,14> buffers{};std::array<UINT,14> first{},counts{};
                if(c1){if(stage==0)c1->VSGetConstantBuffers1(0,14,buffers.data(),first.data(),counts.data());
                    if(stage==1)c1->PSGetConstantBuffers1(0,14,buffers.data(),first.data(),counts.data());
                    if(stage==2)c1->CSGetConstantBuffers1(0,14,buffers.data(),first.data(),counts.data());}
                else{if(stage==0)context->VSGetConstantBuffers(0,14,buffers.data());if(stage==1)context->PSGetConstantBuffers(0,14,buffers.data());if(stage==2)context->CSGetConstantBuffers(0,14,buffers.data());}
                for(unsigned slot=0;slot<14;++slot){ComPtr<ID3D11Buffer> buffer;buffer.Attach(buffers[slot]);const char* name=stage==0?"VS":stage==1?"PS":"CS";
                    Json b={{"stage",name},{"slot",slot},{"resource",reinterpret_cast<uintptr_t>(buffer.Get())},{"rangeUnits","16-byte constants"}};
                    if(buffer){D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);if(!c1)counts[slot]=desc.ByteWidth/16;
                        auto range=EffectiveRange(desc.ByteWidth,first[slot],counts[slot]);b["bufferByteWidth"]=desc.ByteWidth;b["firstConstant"]=first[slot];b["numConstants"]=counts[slot];
                        b["effectiveByteOffset"]=range.offset;b["effectiveByteCount"]=range.bytes;b["rangeKnown"]=range.valid;b["rangeSource"]=c1?"GetConstantBuffers1":"base API whole buffer";
                        auto payload=b;payload["kind"]="target-bound-cb-raw";Buffer(device,context,d,buffer.Get(),range,std::move(payload));}
                    m["CB"].push_back(std::move(b));}
            }
            Emit(d,std::move(m));
        }catch(...){++s.allocationFailures;}
    }
    bool Idle()const noexcept{for(const auto* slot:slots_)if(slot)return false;return true;}
    void Poll(ID3D11Device* device,ID3D11DeviceContext* context)noexcept {
        if(!device || !context)return;
        for(auto& slot:slots_)if(slot){
            if(GetCurrentThreadId()!=slot->draw.thread || reinterpret_cast<uintptr_t>(device)!=slot->draw.device || reinterpret_cast<uintptr_t>(context)!=slot->draw.context){++Get().wrongThread;continue;}
            if(FAILED(device->GetDeviceRemovedReason())){FailureNoThrow(slot->draw,slot->metadata,"DROP_DEVICE_REMOVED");pendingBytes_-=slot->bytes;--Get().gpuPending;delete slot;slot=nullptr;continue;}
            ++slot->polls;
            if(!Enabled())slot->draw.invalidated=true;
            // Informational only: pending query ownership remains on this thread.
            if(slot->polls==MaxPolls || (GetTickCount64()-slot->queued>2000 && slot->polls==1))++Get().gpuSlow;
            BOOL done=FALSE;const auto hr=QueryComplete(context,slot->query.Get(),&done);
            if(FAILED(hr)){slot->draw.invalidated=true;
                if(slot->polls==1)FailureNoThrow(slot->draw,slot->metadata,"EVENT_QUERY_FAILED_RETAINED",hr);
                continue;}
            if(hr!=S_OK || !done)continue;
            D3D11_MAPPED_SUBRESOURCE map{};const auto mapped=context->Map(slot->staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
            if(mapped==DXGI_ERROR_WAS_STILL_DRAWING)continue;
            if(FAILED(mapped)){FailureNoThrow(slot->draw,slot->metadata,"DROP_NONBLOCKING_MAP_FAILED",mapped);pendingBytes_-=slot->bytes;--Get().gpuPending;delete slot;slot=nullptr;continue;}
            try{
                if(!map.pData || (slot->height>1 && map.RowPitch<slot->rowBytes))Failure(slot->draw,slot->metadata,"DROP_INVALID_MAPPED_PITCH");
                else{std::vector<uint8_t> raw(slot->rowBytes*slot->height);for(UINT y=0;y<slot->height;++y)
                    memcpy(raw.data()+y*slot->rowBytes,static_cast<const uint8_t*>(map.pData)+slot->offset+static_cast<size_t>(y)*map.RowPitch,slot->rowBytes);
                    slot->metadata["mappedRowPitch"]=map.RowPitch;slot->metadata["mappedDepthPitch"]=map.DepthPitch;slot->metadata["gpuReadbackComplete"]=true;
                    slot->metadata["status"]=slot->draw.invalidated?"RAW_COMPLETED_OWNER_GENERATION_INVALIDATED":"RAW_COMPLETED";
                    Emit(slot->draw,std::move(slot->metadata),std::move(raw),slot->name);++Get().gpuCompleted;}
            }catch(...){++Get().allocationFailures;}
            context->Unmap(slot->staging.Get(),0);pendingBytes_-=slot->bytes;--Get().gpuPending;delete slot;slot=nullptr;
        }
    }
};
bool InputObserver::Idle()const noexcept {return !storage_ || storage_->Idle();}
InputObserver::InputObserver()=default;
InputObserver::~InputObserver()=default;
void InputObserver::Revoke()noexcept{if(storage_)storage_->Revoke();}
void InputObserver::CheckGeneration(uint64_t(*generation)(uint64_t))noexcept{if(storage_ && generation)storage_->CheckGeneration(generation);}
void InputObserver::Queue(ID3D11Device* device,ID3D11DeviceContext* context,const DrawFact& draw,
                          const std::array<ID3D11ShaderResourceView*,3>& inputs)noexcept {
    if(!Enabled())return;
    try{if(!storage_)storage_=std::make_unique<Storage>();storage_->Queue(device,context,draw,inputs);}
    catch(...){++Get().allocationFailures;}
}
bool InputObserver::Poll(ID3D11Device* device,ID3D11DeviceContext* context)noexcept {
    if(storage_)storage_->Poll(device,context);return Idle();
}
}
