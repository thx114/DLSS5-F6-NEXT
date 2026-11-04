// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <cmath>
#include <SysUtils.h>
#include "GenshinDepthDraw.h"
#include "GenshinQueryScope.h"
#include "GenshinCpuWriterProbe.h"
#include <algorithm>
#include <cstring>

// Bounded observations only. No configuration, numerical convention or UV proof
// can be manufactured by this capture. Six bounded samples per process;
// two reserved blits plus four distinct source/shader observations. Not camera proof.
namespace GenshinDepthEvidence {
using Microsoft::WRL::ComPtr;
inline uint64_t ByteHash(const void* data,size_t size)noexcept {
    uint64_t hash=14695981039346656037ull;const auto* bytes=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ull;}return hash;
}
inline constexpr GUID BytesKey={0x123d6aa1,0x7171,0x4ab3,{0xba,0xd2,0x2e,0xf7,0x61,0x91,0x00,0x01}};
inline constexpr uint64_t SceneVs=0x778E7E69BB2060F5ull,PictureVs=0x504428132CA63202ull;
struct ExpectedDepth {
    ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Resource> resource;
    DWORD thread=0;UINT mip=0,format=0;bool sawDsv=false,sawRtv=false,sawCs=false;
    uint64_t deviceEpoch=0,producerEpoch=0,configEpoch=0,instance=0,generation=0,frame=0;
    uint64_t boundaries=0; // observation only; never a frame/semantic certificate
};
inline ExpectedDepth& Expected(){static thread_local ExpectedDepth expected;return expected;}
inline void ForgetDepthInput(ID3D11DeviceContext* context)noexcept {
    GenshinCpuWriterProbe::Revoke(context);
    if(Expected().context.Get()==context)Expected()={};
}
// Preserve only the last authenticated source LOCATION across a state/deferred
// boundary. The resource is held, so pointer reuse cannot change its identity.
// Its owner/frame are revoked; a later matching writer is still UNPROVED.
inline void RevokeDepthOwner(ID3D11DeviceContext* context)noexcept {
    GenshinCpuWriterProbe::Revoke(context);
    auto& expected=Expected();
    if(expected.context.Get()!=context || expected.thread!=GetCurrentThreadId())return;
    expected.instance=expected.generation=expected.frame=0;
    expected.sawDsv=expected.sawRtv=expected.sawCs=false;
    if(expected.boundaries!=UINT64_MAX)++expected.boundaries;
}
inline void ExpectDepthInput(ID3D11DeviceContext* context,uintptr_t resource,
    uint64_t deviceEpoch,uint64_t producerEpoch,uint64_t configEpoch,uint64_t instance,uint64_t generation,uint64_t frame,
    UINT mip,UINT format,float rawX,float rawY,float requestedX,float requestedY)noexcept {
    if(!context || !resource || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    // Native input stores a texture interface. View::GetResource may return a
    // different interface address for that same object: resolve its real resource
    // interface before diagnostic binding comparisons, never infer identity.
    ComPtr<ID3D11Resource> source;
    if(FAILED(reinterpret_cast<IUnknown*>(resource)->QueryInterface(IID_PPV_ARGS(&source))) || !source)return;
    auto& expected=Expected();
    const bool changed=expected.context.Get()!=context || expected.resource.Get()!=source.Get() ||
        expected.deviceEpoch!=deviceEpoch || expected.producerEpoch!=producerEpoch || expected.configEpoch!=configEpoch;
    if(changed){expected.sawDsv=expected.sawRtv=expected.sawCs=false;}
    expected.context=context;expected.resource=source;
    expected.thread=GetCurrentThreadId();expected.mip=mip;expected.format=format;
    expected.deviceEpoch=deviceEpoch;expected.producerEpoch=producerEpoch;expected.configEpoch=configEpoch;
    expected.instance=instance;expected.generation=generation;expected.frame=frame;
    // Retain the selected source location for real copy/clear correspondence.
    // Publishing a production SR receipt must not arm CPU payload/binding
    // diagnostic windows in every subsequent draw/update hook.
    if(changed || generation<=3 || generation%300==0){
        ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC d{};
        if(SUCCEEDED(expected.resource.As(&texture)))texture->GetDesc(&d);
        LOG_INFO("Genshin depth evidence source: pid={} generation={} resource={} desc={}x{} format={} bind_flags={} usage={} mips={} array={} samples={} first_failure={} dsv_seen={} rtv_seen={} cs_seen={} no_dsv_provenance_inferred=true",
            GetCurrentProcessId(),generation,resource,d.Width,d.Height,static_cast<unsigned>(d.Format),d.BindFlags,
            static_cast<unsigned>(d.Usage),d.MipLevels,d.ArraySize,d.SampleDesc.Count,
            expected.sawDsv||expected.sawRtv||expected.sawCs?"WRITER_SEMANTICS_UNPROVED":"NO_SOURCE_MATCHED_WRITER_YET",
            expected.sawDsv,expected.sawRtv,expected.sawCs);
    }
    static unsigned offers=0;
    if((changed || offers<4) && offers<12){++offers;
        LOG_INFO("Genshin depth evidence producer: pid={} tid={} context={} original_depth={} mip={} srv_format={} device_epoch={} owner_epoch={} config_epoch={} instance={} generation={} sourceFrame={} raw_jitter={},{} requested_jitter={},{} phase=UNPROVED deviceZ=UNPROVED",
            GetCurrentProcessId(),GetCurrentThreadId(),reinterpret_cast<uintptr_t>(context),resource,mip,format,
            deviceEpoch,producerEpoch,configEpoch,instance,generation,frame,rawX,rawY,requestedX,requestedY);
    }
}
struct ClearRecord {
    ID3D11DeviceContext* context=nullptr;DWORD thread=0;ComPtr<ID3D11Resource> resource;
    UINT flags=0,mip=0;float value=0;uint64_t sequence=0;const char* kind="DSV";
};
inline std::array<ClearRecord,8>& Clears(){static auto* records=new std::array<ClearRecord,8>;return *records;}
inline std::mutex& ClearMutex(){static auto* mutex=new std::mutex;return *mutex;}
// Hook forwarding seam; not an inferred clear from a descriptor, default, INI,
// numerical-maximum rule or current comparison function.
inline void ObserveDepthClear(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,UINT flags,float value)noexcept {
    if(!context || !view || !(flags&D3D11_CLEAR_DEPTH) || !std::isfinite(value) ||
       context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    const auto expected=Expected();if(expected.context.Get()!=context || expected.thread!=GetCurrentThreadId())return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    if(resource.Get()!=expected.resource.Get())return;
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};view->GetDesc(&desc);
    if(desc.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || desc.Texture2D.MipSlice!=expected.mip)return;
    std::lock_guard lock(ClearMutex());auto& records=Clears();ClearRecord* found=nullptr;
    for(auto& record:records)if(record.context==context && record.resource.Get()==resource.Get()){found=&record;break;}
    if(!found)for(auto& record:records)if(!record.context){found=&record;break;}
    if(!found)return;static uint64_t sequence=0;
    found->context=context;found->thread=GetCurrentThreadId();found->resource=resource;
    found->flags=flags;found->mip=desc.Texture2D.MipSlice;found->value=value;found->sequence=++sequence;
    static unsigned reports=0;if(reports<8){++reports;LOG_INFO("Genshin depth evidence clear: pid={} sequence={} context={} resource={} flags={} value={} projection=UNPROVED",
        GetCurrentProcessId(),found->sequence,reinterpret_cast<uintptr_t>(context),reinterpret_cast<uintptr_t>(expected.resource.Get()),flags,value);}
}
// RTV/UAV clears are distinct observations. Never reinterpret a vector or
// uint clear as a certified DeviceZ far value.
inline void ObserveSurfaceClear(ID3D11DeviceContext* context,ID3D11View* view,const char* kind,const void* values)noexcept {
    const auto& expected=Expected();
    if(!context || !view || !values || expected.context.Get()!=context || expected.thread!=GetCurrentThreadId())return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);if(resource.Get()!=expected.resource.Get())return;
    UINT mip=UINT_MAX;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11UnorderedAccessView> uav;
    if(SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&rtv)))){D3D11_RENDER_TARGET_VIEW_DESC d{};rtv->GetDesc(&d);
        if(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D)mip=d.Texture2D.MipSlice;}
    else if(SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&uav)))){D3D11_UNORDERED_ACCESS_VIEW_DESC d{};uav->GetDesc(&d);
        if(d.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2D)mip=d.Texture2D.MipSlice;}
    if(mip!=expected.mip)return;
    std::array<UINT,4> bits{};memcpy(bits.data(),values,sizeof(bits));
    static unsigned reports=0;if(reports<8){++reports;LOG_INFO("Genshin depth evidence clear vector: pid={} context={} resource={} mip={} kind={} bits={:08X},{:08X},{:08X},{:08X} semantics=UNPROVED",
        GetCurrentProcessId(),reinterpret_cast<uintptr_t>(context),reinterpret_cast<uintptr_t>(resource.Get()),mip,kind,bits[0],bits[1],bits[2],bits[3]);}
}
inline std::string Hex(const void* data,size_t bytes) {
    static constexpr char digits[]="0123456789ABCDEF";
    std::string text(bytes*2,'0');const auto* p=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<bytes;++i){text[2*i]=digits[p[i]>>4];text[2*i+1]=digits[p[i]&15];}return text;
}
inline void RetainVertex(ID3D11DeviceChild* shader,const void* bytes,size_t size,uint64_t hash) noexcept {
    if(!shader || !bytes || !size || size>64*1024)return;
    UINT existing=0;if(SUCCEEDED(shader->GetPrivateData(BytesKey,&existing,nullptr)) && existing)return;
    // Reserve room for the two known fullscreen VSs independently of the first
    // eight other creation callbacks. Object identity never inherits old bytes.
    static std::atomic_uint knownBytes{0},otherBytes{0};
    const bool target=hash==SceneVs || hash==PictureVs || hash==0x773B7BD7A2C6971Full || hash==0xB58064BC5041A048ull;
    auto& budget=target?knownBytes:otherBytes;
    const unsigned limit=target?256*1024:16*1024*1024;
    unsigned used=budget.load(std::memory_order_relaxed);
    while(used<=limit && size<=limit-used){
        if(budget.compare_exchange_weak(used,used+static_cast<unsigned>(size),std::memory_order_relaxed)){
            shader->SetPrivateData(BytesKey,static_cast<UINT>(size),bytes);return;
        }
    }
}
// Shared create-byte/reflection readers for the existing semantic and replay
// paths. Immutable shader objects own BytesKey; no live-stage substitution.
inline bool ReadShaderBytes(ID3D11DeviceChild* shader,std::vector<unsigned char>& bytes) {
    UINT size=0;if(!shader || FAILED(shader->GetPrivateData(BytesKey,&size,nullptr)) || !size || size>65536)return false;
    bytes.resize(size);const auto expected=size;
    return SUCCEEDED(shader->GetPrivateData(BytesKey,&size,bytes.data())) && size==expected;
}
// Failed cache misses / first bounded same-draw CB refusals only. Strong
// roots avoid pointer reuse; Present drains one record per boundary after all
// draw/collector locks. No draw I/O/disassembly/readback,
// no content hash and no additional shader-byte cache or capture pipeline.
struct RefusedShader {
    ComPtr<ID3D11DeviceChild> shader;ComPtr<ID3D11Device> device;ComPtr<ID3D11Buffer> cb;
    DWORD thread=0;uint64_t call=0,tick=0;UINT stage=0,failure=0,instruction=0,dependency=0,slot=UINT_MAX,type=UINT_MAX;
    std::array<char,40> opcode{};uint64_t cbRevision=0,cbLoss=0;UINT cbState=0,cbFirst=0,cbCount=0,trackedBuffers=0;
    D3D11_BUFFER_DESC cbDesc{};bool emitted=false;
};
struct RefusedShaders {
    std::mutex mutex;std::array<RefusedShader,16> samples;
    std::atomic<unsigned> programCount{0},bufferCount{0};
};
inline RefusedShaders& Refusals(){static RefusedShaders samples;return samples;}
inline void ObserveShader(ID3D11DeviceChild* shader,UINT stage,UINT failure,UINT instruction,UINT dependency,
    const std::array<char,40>& opcode,UINT slot,UINT type,uint64_t cbRevision=0,uint64_t cbLoss=0,UINT cbState=0,
    ID3D11Buffer* cb=nullptr,UINT cbFirst=0,UINT cbCount=0,UINT trackedBuffers=0)noexcept {
    if(!shader)return;
    try {
        auto& samples=Refusals();const bool buffer=failure>=0x10000u;
        auto& count=buffer?samples.bufferCount:samples.programCount;
        if(count.load(std::memory_order_relaxed)>=8)return;
        std::unique_lock lock(samples.mutex,std::try_to_lock);if(!lock.owns_lock() || count.load(std::memory_order_relaxed)>=8)return;
        const size_t first=buffer?8:0,last=buffer?16:8;
        for(size_t i=first;i<last;++i){const auto& sample=samples.samples[i];if(sample.shader.Get()==shader && sample.stage==stage)return;}
        for(size_t i=first;i<last;++i){auto& sample=samples.samples[i];if(!sample.shader){
            sample.shader=shader;shader->GetDevice(&sample.device);sample.thread=GetCurrentThreadId();
            sample.call=GenshinDepthMapping::CurrentDraw().observationCall;sample.tick=GetTickCount64();
            sample.stage=stage;sample.failure=failure;sample.instruction=instruction;sample.dependency=dependency;
            sample.opcode=opcode;sample.slot=slot;sample.type=type;
            sample.cbRevision=cbRevision;sample.cbLoss=cbLoss;sample.cbState=cbState;
            sample.cb=cb;sample.cbFirst=cbFirst;sample.cbCount=cbCount;sample.trackedBuffers=trackedBuffers;
            if(cb)cb->GetDesc(&sample.cbDesc);
            count.fetch_add(1,std::memory_order_relaxed);return;
        }}
    }catch(...){}
}
inline void DrainRefusedShader(ID3D11Device* device)noexcept {
    if(!device)return;
    try {
        RefusedShader selected;
        {auto& samples=Refusals();std::unique_lock lock(samples.mutex,std::try_to_lock);if(!lock.owns_lock())return;
            for(auto& sample:samples.samples)if(sample.shader && !sample.emitted && sample.device.Get()==device && sample.thread==GetCurrentThreadId()){
                sample.emitted=true;selected=sample;break;}}
        if(!selected.shader)return;
        std::vector<unsigned char> bytes;const bool available=ReadShaderBytes(selected.shader.Get(),bytes);
        LOG_INFO("R1067 refused shader bytes: pid={} call={} tick={} thread={} stage={} shader={} device={} failure={} instruction={} opcode={} dependency={} resource_slot={} resource_type={} cb_revision={} cb_loss={} cb_state={} cb={} cb_first={} cb_count={} tracked_buffers={} cb_bytes={} cb_usage={} cb_bind={} cb_cpu={} cb_misc={} state={} bytes={} raw_hex={} observation_only=true",
            GetCurrentProcessId(),selected.call,selected.tick,selected.thread,selected.stage,reinterpret_cast<uintptr_t>(selected.shader.Get()),
            reinterpret_cast<uintptr_t>(device),selected.failure,selected.instruction,selected.opcode.data(),selected.dependency,selected.slot,selected.type,
            selected.cbRevision,selected.cbLoss,selected.cbState,
            reinterpret_cast<uintptr_t>(selected.cb.Get()),selected.cbFirst,selected.cbCount,selected.trackedBuffers,
            selected.cbDesc.ByteWidth,unsigned(selected.cbDesc.Usage),selected.cbDesc.BindFlags,selected.cbDesc.CPUAccessFlags,selected.cbDesc.MiscFlags,
            available?"COMPLETE_CREATE_BYTES":"CREATE_BYTES_UNAVAILABLE",bytes.size(),available?Hex(bytes.data(),bytes.size()):std::string{});
        if(!available)return;
        ComPtr<ID3DBlob> assembly;const auto hr=D3DDisassemble(bytes.data(),bytes.size(),0,nullptr,&assembly);
        std::string text;
        if(SUCCEEDED(hr) && assembly && assembly->GetBufferSize()<=512*1024)
            text.assign(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());
        if(!text.empty() && !text.back())text.pop_back();
        LOG_INFO("R1067 refused shader assembly: pid={} call={} stage={} shader={} hr={} state={} instructions_begin\n{}\ninstructions_end observation_only=true",
            GetCurrentProcessId(),selected.call,selected.stage,reinterpret_cast<uintptr_t>(selected.shader.Get()),static_cast<int64_t>(hr),
            text.empty()?"UNAVAILABLE":"COMPLETE",text);
    }catch(...){}
}
inline bool MatchingSignature(ID3D11ShaderReflection* producer,ID3D11ShaderReflection* consumer,bool patch=false)noexcept {
    if(!producer || !consumer)return false;D3D11_SHADER_DESC pd{},cd{};
    if(FAILED(producer->GetDesc(&pd)) || FAILED(consumer->GetDesc(&cd)))return false;
    const UINT inputs=patch?cd.PatchConstantParameters:cd.InputParameters;
    const UINT outputs=patch?pd.PatchConstantParameters:pd.OutputParameters;
    for(UINT i=0;i<inputs;++i){D3D11_SIGNATURE_PARAMETER_DESC input{};
        if(FAILED(patch?consumer->GetPatchConstantParameterDesc(i,&input):consumer->GetInputParameterDesc(i,&input)))return false;
        if(!input.ReadWriteMask)continue;
        // Primitive/control-point/domain system inputs are generated by the
        // pipeline, never guessed values supplied by a previous shader.
        if(!patch && input.SemanticName && (!_stricmp(input.SemanticName,"SV_OutputControlPointID") ||
            !_stricmp(input.SemanticName,"SV_DomainLocation") || !_stricmp(input.SemanticName,"SV_PrimitiveID")))continue;
        bool found=false;
        for(UINT j=0;j<outputs;++j){D3D11_SIGNATURE_PARAMETER_DESC output{};
            if(FAILED(patch?producer->GetPatchConstantParameterDesc(j,&output):producer->GetOutputParameterDesc(j,&output)))return false;
            if(input.SemanticName && output.SemanticName && !_stricmp(input.SemanticName,output.SemanticName) &&
               input.SemanticIndex==output.SemanticIndex && input.Register==output.Register && input.Stream==output.Stream &&
               input.ComponentType==output.ComponentType && (input.ReadWriteMask&output.Mask)==input.ReadWriteMask){found=true;break;}
        }
        if(!found)return false;
    }
    return true;
}
inline bool TessellationSignature(ID3D11ShaderReflection* vs,ID3D11ShaderReflection* hs,
    ID3D11ShaderReflection* ds,D3D11_PRIMITIVE_TOPOLOGY topology)noexcept {
    D3D11_SHADER_DESC h{},d{};
    if(!hs || !ds || FAILED(hs->GetDesc(&h)) || FAILED(ds->GetDesc(&d)) ||
       topology<D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST ||
       topology>D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)return false;
    const UINT inputCount=UINT(topology)-UINT(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST)+1;
    // InputPrimitive describes HS input; cControlPoints describes HS output
    // and DS input. They need not be the same number.
    if(UINT(h.InputPrimitive)!=UINT(D3D_PRIMITIVE_1_CONTROL_POINT_PATCH)+inputCount-1 ||
       !h.cControlPoints || h.cControlPoints>32 || h.cControlPoints!=d.cControlPoints ||
       h.TessellatorDomain!=d.TessellatorDomain ||
       (d.TessellatorDomain!=D3D_TESSELLATOR_DOMAIN_TRI && d.TessellatorDomain!=D3D_TESSELLATOR_DOMAIN_QUAD &&
        d.TessellatorDomain!=D3D_TESSELLATOR_DOMAIN_ISOLINE))return false;
    return MatchingSignature(vs,hs) && MatchingSignature(hs,ds) && MatchingSignature(hs,ds,true);
}
enum class ShaderPresence { Null, PresentUncached, PresentCached };
inline ShaderPresence Presence(ID3D11DeviceChild* shader,UINT& bytes,HRESULT& hr)noexcept {
    bytes=0;if(!shader){hr=E_POINTER;return ShaderPresence::Null;}
    hr=shader->GetPrivateData(BytesKey,&bytes,nullptr);
    return SUCCEEDED(hr) && bytes && bytes<=64*1024?ShaderPresence::PresentCached:ShaderPresence::PresentUncached;
}
inline void ShaderLog(ID3D11DeviceChild* shader,uint64_t serial,const char* stage) noexcept {
    try {
        UINT bytes=0;HRESULT hr=S_OK;const auto presence=Presence(shader,bytes,hr);
        if(presence!=ShaderPresence::PresentCached){
            LOG_INFO("Genshin depth evidence shader: pid={} serial={} stage={} shader_state={} bytecode={} hr={}",
                GetCurrentProcessId(),serial,stage,presence==ShaderPresence::Null?"NULL_SHADER":"PRESENT_UNCACHED",
                presence==ShaderPresence::Null?"NOT_APPLICABLE":"UNAVAILABLE",static_cast<int64_t>(hr));return;
        }
        std::vector<unsigned char> code(bytes);const UINT expectedBytes=bytes;
        hr=shader->GetPrivateData(BytesKey,&bytes,code.data());
        if(FAILED(hr) || bytes!=expectedBytes){
            LOG_INFO("Genshin depth evidence shader raw: pid={} serial={} stage={} state=CACHE_READ_FAILED expected_bytes={} returned_bytes={} hr={} semantics=UNPROVED",
                GetCurrentProcessId(),serial,stage,expectedBytes,bytes,static_cast<int64_t>(hr));return;
        }
        // Complete actual create bytes, not a reconstructed program. Caller is
        // already bounded to first metadata + at most one fresh capture/slot.
        LOG_INFO("Genshin depth evidence shader raw: pid={} serial={} stage={} shader_object={} state=COMPLETE_CREATE_BYTES bytes={} hex_chars={} fnv1a64={:016X} raw_hex={} semantics=UNPROVED",
            GetCurrentProcessId(),serial,stage,reinterpret_cast<uintptr_t>(shader),bytes,size_t(bytes)*2,ByteHash(code.data(),bytes),Hex(code.data(),bytes));
        ComPtr<ID3DBlob> text;hr=D3DDisassemble(code.data(),bytes,0,nullptr,&text);
        std::string instructions;
        if(SUCCEEDED(hr) && text && text->GetBufferSize()<=128*1024)
            instructions.assign(static_cast<const char*>(text->GetBufferPointer()),text->GetBufferSize());
        if(!instructions.empty() && !instructions.back())instructions.pop_back();
        LOG_INFO("Genshin depth evidence shader: pid={} serial={} stage={} shader_state=PRESENT_CACHED hash={:016X} bytes={} disassemble_hr={} instructions_begin\n{}\ninstructions_end",
            GetCurrentProcessId(),serial,stage,ByteHash(code.data(),bytes),bytes,static_cast<int64_t>(hr),instructions);
    }catch(...){}
}
// These numeric keys are deduplication only, never resource/issuer authority.
// Each occupied sample pins the actual canonical objects so addresses cannot
// be reused for a different writer during this bounded process-lifetime set.
struct ObservationKey {
    uintptr_t context=0,resource=0,vs=0,ps=0,cs=0;
    uint64_t deviceEpoch=0,configEpoch=0;UINT mip=0;unsigned role=0;
    bool operator==(const ObservationKey& other)const noexcept {
        return context==other.context && resource==other.resource && vs==other.vs && ps==other.ps && cs==other.cs &&
            deviceEpoch==other.deviceEpoch && configEpoch==other.configEpoch && mip==other.mip && role==other.role;
    }
};
struct ObservationPlan {
    ObservationKey key{};bool metadataSeen=false,refusalLogged=false,readbackAttempted=false;
    uint64_t firstSerial=0,nextProbe=0;
    bool Due(uint64_t now)const noexcept {return !readbackAttempted && now>=nextProbe;}
    void Refused(uint64_t now)noexcept {nextProbe=now>UINT64_MAX-1000?UINT64_MAX:now+1000;}
};
inline bool CanonicalObject(IUnknown* object,ComPtr<IUnknown>& identity)noexcept {
    return !object || SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)));
}
inline bool LocationCurrent(ID3D11DeviceContext* context,DWORD thread,uint64_t deviceEpoch,uint64_t configEpoch)noexcept {
    const auto& expected=Expected();
    return context && expected.context.Get()==context && expected.resource && expected.thread==thread &&
        deviceEpoch && configEpoch && expected.deviceEpoch==deviceEpoch && expected.configEpoch==configEpoch;
}
struct Part {
    ComPtr<ID3D11Buffer> source,staging;
    UINT stage=0,slot=0,first=0,count=0,offset=0,bytes=0;
};
// Two records WITHIN each of the same six slots: first metadata and optional
// fresh readback after query refusal. No queue, resampling or frame authority.
struct RasterExport {
    uint64_t serial=0,call=0,sampledStamp=0,sampledLoss=0,originalStamp=0,originalLoss=0;
    uint64_t clear=0,returned=0,loss=0;
    bool apiReturned=false,rawMatched=false,claimLogged=false,readback=false,bindingsMatch=false;
};
inline uint64_t& PendingRasterExport(){static thread_local uint64_t call=0;return call;}
inline bool RasterExportPending(ID3D11DeviceContext* context)noexcept {
    const auto& d=GenshinDepthMapping::CurrentDraw();
    return d.observationCall && PendingRasterExport()==d.observationCall && d.context==context && d.thread==GetCurrentThreadId();
}
inline std::atomic_uint waitingClaimExports{0}; // at most 12, inside the six slots
struct Sample {
    ObservationPlan plan{};
    std::array<RasterExport,2> exports{};
    ComPtr<IUnknown> keyContext,keyResource,keyVs,keyPs,keyCs;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Resource> depthResource;
    ComPtr<ID3D11Query> query;
    std::array<Part,33> parts{}; // VS/PS b0..b13, VB0..3, IB
    UINT copiedBytes=0;
    uint64_t serial=0,generation=0,sourceFrame=0,deviceEpoch=0,configEpoch=0;
    DWORD thread=0;unsigned role=0,count=0;bool pending=false,reported=false;
};
// The bounded root also survives capture/storage destruction. A failed query
// cannot release source/staging objects still referenced by submitted work.
inline std::array<Sample,6>& Samples(){static auto* samples=new std::array<Sample,6>;return *samples;}
inline std::mutex& SamplesMutex(){static auto* mutex=new std::mutex;return *mutex;}
// Called after the sampled CPU metadata/readback submission and immediately
// before the real original draw. No raw collector mutex is held on entry.
inline void MarkSampledCall(ID3D11DeviceContext* context,uint64_t stamp,uint64_t loss)noexcept {
    const auto call=GenshinDepthMapping::CurrentDraw().observationCall;
    if(!call || PendingRasterExport()!=call)return;
    try{std::lock_guard lock(SamplesMutex());for(auto& s:Samples())
        if(s.thread==GetCurrentThreadId() && s.plan.key.context && GenshinDepthMapping::CurrentDraw().context==context)
            for(auto& e:s.exports)if(e.call==call && !e.apiReturned){e.sampledStamp=stamp;e.sampledLoss=loss;}
    }catch(...){}
}
inline void MarkOriginalCall(ID3D11DeviceContext* context,uint64_t stamp,uint64_t loss)noexcept {
    const auto call=GenshinDepthMapping::CurrentDraw().observationCall;
    if(!call || PendingRasterExport()!=call)return;
    try{std::lock_guard lock(SamplesMutex());for(auto& s:Samples())
        if(s.thread==GetCurrentThreadId() && GenshinDepthMapping::CurrentDraw().context==context)
            for(auto& e:s.exports)if(e.call==call && !e.apiReturned){e.originalStamp=stamp;e.originalLoss=loss;}
    }catch(...){}
}
// Lookup executes OUTSIDE SamplesMutex and uses the read-only, non-revoking
// R628 observation accessor. Only the sampled same API invocation may finish.
template<class Lookup>
inline void ExportRasterReturn(ID3D11DeviceContext* context,Lookup lookup)noexcept {
    const auto draw=GenshinDepthMapping::CurrentDraw();
    if(!draw.observationCall || PendingRasterExport()!=draw.observationCall || draw.context!=context || draw.thread!=GetCurrentThreadId())return;
    PendingRasterExport()=0;
    try{
        struct Request {unsigned slot=0,index=0;uint64_t serial=0;ComPtr<ID3D11Resource> resource;UINT mip=0;};
        std::array<Request,12> requests{};unsigned count=0;
        {std::lock_guard lock(SamplesMutex());for(unsigned k=0;k<Samples().size();++k){auto& s=Samples()[k];
            if(s.thread!=GetCurrentThreadId())continue;
            for(unsigned n=0;n<s.exports.size();++n){const auto& e=s.exports[n];
                if(e.call!=draw.observationCall || e.apiReturned)continue;
                auto& r=requests[count++];r.slot=k;r.index=n;r.serial=e.serial;r.mip=s.plan.key.mip;
                if(s.keyResource)s.keyResource.As(&r.resource);
            }}
        }
        for(unsigned k=0;k<count;++k){const auto& request=requests[k];auto interval=lookup(request.resource.Get(),request.mip);
            ComPtr<IUnknown> c,r,v,p;
            bool identities=false;
            if(interval)identities=CanonicalObject(interval->context.Get(),c) && CanonicalObject(interval->resource.Get(),r) &&
                CanonicalObject(interval->last.vs.Get(),v) && CanonicalObject(interval->last.ps.Get(),p);
            std::lock_guard lock(SamplesMutex());auto& s=Samples()[request.slot];auto& e=s.exports[request.index];
            if(e.serial!=request.serial || e.call!=draw.observationCall || e.apiReturned)continue;
            e.apiReturned=true;
            e.rawMatched=interval && identities && interval->clearReturned && interval->last.draw.observationCall==e.call &&
                interval->last.draw.context==context && interval->last.draw.thread==draw.thread &&
                interval->epochs.device==s.plan.key.deviceEpoch && interval->epochs.config==s.plan.key.configEpoch &&
                interval->clearDesc.Texture2D.MipSlice==s.plan.key.mip &&
                reinterpret_cast<uintptr_t>(c.Get())==s.plan.key.context && reinterpret_cast<uintptr_t>(r.Get())==s.plan.key.resource &&
                reinterpret_cast<uintptr_t>(v.Get())==s.plan.key.vs && reinterpret_cast<uintptr_t>(p.Get())==s.plan.key.ps;
            e.readback=s.plan.readbackAttempted && s.serial==e.serial;
            UINT clearBits=0;bool quiescent=false;
            if(e.rawMatched){e.clear=interval->clearSerial;e.returned=interval->returnedSerial;e.loss=interval->loss;
                waitingClaimExports.fetch_add(1,std::memory_order_relaxed);
                std::memcpy(&clearBits,&interval->clearValue,sizeof(clearBits));quiescent=interval->last.queryQuiescent;
                e.bindingsMatch=e.readback;
                for(unsigned n=0;n<28;++n){const auto& actual=interval->last.constants[n];const auto& sampled=s.parts[n];
                    if(actual.buffer.Get()!=sampled.source.Get() || (actual.buffer && (actual.first!=sampled.first || actual.count!=sampled.count)))e.bindingsMatch=false;
                }
            }
            const bool unchanged=e.sampledStamp && e.sampledStamp==e.originalStamp && e.sampledLoss==e.originalLoss;
            LOG_INFO("Genshin depth evidence returned link: pid={} serial={} observation_slot={} call={} context={} tid={} resource={} mip={} device_epoch={} config_epoch={} api_returned=true raw_r628={} clear_serial={} clear_bits={:08X} clear_flags={} returned_serial={} loss={} query_quiescent={} cb_readback_attempted={} cb_bindings_match={} sampled_stamp={} original_stamp={} no_tracked_boundary_gap={} cb_phase=BEFORE_OPTIONAL_PIPELINE_DRAIN claim=WAIT_ACTUAL_ASSOCIATION semantics=UNPROVED",
                GetCurrentProcessId(),e.serial,request.slot,e.call,reinterpret_cast<uintptr_t>(context),draw.thread,s.plan.key.resource,s.plan.key.mip,
                s.plan.key.deviceEpoch,s.plan.key.configEpoch,e.rawMatched?"MATCHED_SAME_CALL":"UNKNOWN",e.clear,clearBits,e.rawMatched?interval->clearFlags:0,e.returned,e.loss,
                quiescent,e.readback,e.bindingsMatch,e.sampledStamp,e.originalStamp,unchanged);
            if(e.rawMatched){const auto& raster=interval->last;const auto& actual=raster.draw;
                LOG_INFO("Genshin depth evidence returned raster: pid={} serial={} call={} kind={} count={} start={} base={} instances={} instance_start={} source_size={},{} source_format={} dsv_format={} dsv_flags={} depth_enable={} depth_write={} depth_func={} topology={} viewport_count={} semantics=UNPROVED",
                    GetCurrentProcessId(),e.serial,e.call,actual.kind,actual.count,actual.start,actual.base,actual.instances,actual.instanceStart,
                    interval->texture.Width,interval->texture.Height,static_cast<unsigned>(interval->texture.Format),static_cast<unsigned>(raster.viewDesc.Format),raster.viewDesc.Flags,
                    static_cast<bool>(raster.depthDesc.DepthEnable),static_cast<unsigned>(raster.depthDesc.DepthWriteMask),static_cast<unsigned>(raster.depthDesc.DepthFunc),static_cast<unsigned>(raster.topology),raster.viewportCount);
                for(UINT n=0;n<raster.viewportCount;++n){const auto& viewport=raster.viewports[n];
                    LOG_INFO("Genshin depth evidence returned viewport: pid={} serial={} call={} index={} rect={},{},{},{} depth={},{} semantics=UNPROVED",
                        GetCurrentProcessId(),e.serial,e.call,n,viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height,viewport.MinDepth,viewport.MaxDepth);
                }
            }
        }
    }catch(...){}
}
// Actual immutable R628 association only; caller invokes after Associate has
// released its mutex. A sampled earlier draw can belong to the same unbroken
// clear interval; this DOES NOT say it was the last writer or main camera.
template<class Evidence,class Frame>
inline void ExportClaim(const Evidence* evidence,const Frame& frame)noexcept {
    if(!waitingClaimExports.load(std::memory_order_relaxed) || !evidence || !evidence->Matches(frame))return;
    try{const auto& i=evidence->Get();const auto& token=evidence->InputToken();
        UINT jitterBitsX=0,jitterBitsY=0;
        std::memcpy(&jitterBitsX,&frame.rawJitterX,sizeof(jitterBitsX));std::memcpy(&jitterBitsY,&frame.rawJitterY,sizeof(jitterBitsY));
        ComPtr<IUnknown> c,r;if(!CanonicalObject(i.context.Get(),c) || !CanonicalObject(i.resource.Get(),r))return;
        std::lock_guard lock(SamplesMutex());for(unsigned k=0;k<Samples().size();++k){auto& s=Samples()[k];
            if(s.plan.key.context!=reinterpret_cast<uintptr_t>(c.Get()) || s.plan.key.resource!=reinterpret_cast<uintptr_t>(r.Get()) ||
               s.plan.key.mip!=token.mip || s.plan.key.deviceEpoch!=token.deviceEpoch || s.plan.key.configEpoch!=token.configEpoch ||
               s.thread!=GetCurrentThreadId())continue;
            for(auto& e:s.exports){if(!e.rawMatched || e.claimLogged || !e.clear || e.clear!=i.clearSerial ||
                e.loss!=i.loss || e.returned>i.returnedSerial)continue;
                e.claimLogged=true;
                waitingClaimExports.fetch_sub(1,std::memory_order_relaxed);
                LOG_INFO("Genshin depth evidence claim link: pid={} serial={} observation_slot={} call={} context={} tid={} resource={} mip={} clear_serial={} sampled_returned_serial={} claimed_last_returned_serial={} relation=RETURNED_DRAW_MEMBER_OF_CLAIMED_CLEAR_INTERVAL is_claimed_last_draw={} device_epoch={} owner_epoch={} config_epoch={} instance={} generation={} sourceFrame={} sr_draw_serial={} render_context={} token_tick={} srv_format={} render_size={},{} output_size={},{} raw_jitter={},{} raw_jitter_bits={:08X},{:08X} sr_constant_buffer={} sr_shader={:016X} jitter_phase=UNPROVED semantics=UNPROVED",
                    GetCurrentProcessId(),e.serial,k,e.call,reinterpret_cast<uintptr_t>(i.context.Get()),i.thread,s.plan.key.resource,token.mip,
                    e.clear,e.returned,i.returnedSerial,i.last.draw.observationCall==e.call,token.deviceEpoch,token.ownerEpoch,token.configEpoch,token.instance,
                    token.generation,token.frame,token.drawSerial,token.renderContext,token.tokenTick,token.srvFormat,
                    frame.renderWidth,frame.renderHeight,frame.outputWidth,frame.outputHeight,frame.rawJitterX,frame.rawJitterY,jitterBitsX,jitterBitsY,frame.constantBuffer,frame.shader);
            }}
    }catch(...){}
}
inline int SelectSlot(const std::array<Sample,6>& samples,const ObservationKey& key)noexcept {
    if(key.role<1 || key.role>5)return -1;
    const unsigned begin=key.role<=2?key.role-1:2,end=key.role<=2?key.role:6;
    int free=-1;
    for(unsigned i=begin;i<end;++i){
        if(samples[i].plan.key.role && samples[i].plan.key==key)return static_cast<int>(i);
        if(!samples[i].plan.key.role && free<0)free=static_cast<int>(i);
    }
    return free;
}
inline void Poll(ID3D11DeviceContext* context) noexcept {
    std::lock_guard lock(SamplesMutex());
    for(auto& sample:Samples()){
        if(!sample.pending || sample.reported || sample.thread!=GetCurrentThreadId() || sample.context.Get()!=context)continue;
        BOOL done=FALSE;auto hr=context->GetData(sample.query.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(hr==S_FALSE || (SUCCEEDED(hr) && !done))continue;
        if(FAILED(hr)){sample.reported=true;continue;} // quarantine, bounded by six roots
        bool all=true;
        for(auto& part:sample.parts){
            if(!part.staging)continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr=context->Map(part.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING){all=false;continue;}
            if(SUCCEEDED(hr)){
                try {LOG_INFO("Genshin depth evidence constants: pid={} serial={} generation={} sourceFrame={} role={} stage={} slot={} first_constant={} num_constants={} offset={} bytes={} source_buffer={} fnv1a64={:016X} raw_hex={} semantics=UNPROVED",
                    GetCurrentProcessId(),sample.serial,sample.generation,sample.sourceFrame,sample.role,part.stage,part.slot,
                    part.first,part.count,part.offset,part.bytes,reinterpret_cast<uintptr_t>(part.source.Get()),ByteHash(static_cast<const unsigned char*>(mapped.pData)+part.offset,part.bytes),Hex(static_cast<const unsigned char*>(mapped.pData)+part.offset,part.bytes));}
                catch(...){}context->Unmap(part.staging.Get(),0);
            }
            part.source.Reset();part.staging.Reset();
        }
        if(all){sample.reported=true;sample.pending=false;sample.query.Reset();sample.dsv.Reset();
            sample.depthResource.Reset();sample.context.Reset();}
    }
}
inline void Observe(ID3D11DeviceContext* context,uint64_t ps,uint64_t serial,uint64_t generation,
                    uint64_t frame,uint64_t deviceEpoch,uint64_t configEpoch,unsigned forcedRole=0,UINT dispatchX=0,UINT dispatchY=0,UINT dispatchZ=0) noexcept {
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    Poll(context);
    const auto expected=Expected();
    const bool sourceContext=expected.context.Get()==context && expected.thread==GetCurrentThreadId() && expected.resource &&
        expected.deviceEpoch==deviceEpoch && expected.configEpoch==configEpoch;
    const bool knownProfile=ps==0x773B7BD7A2C6971Full || ps==0xB58064BC5041A048ull;
    // Poll must retire existing samples even when this draw cannot acquire a role.
    // Without a source location or known blit profile, every role branch stays zero.
    if(!sourceContext && !knownProfile)return;
    ComPtr<ID3D11DepthStencilState> depth;UINT reference=0;context->OMGetDepthStencilState(&depth,&reference);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS;
    if(depth)depth->GetDesc(&ds);
    ComPtr<ID3D11DepthStencilView> dsv;context->OMGetRenderTargets(0,nullptr,&dsv);
    ComPtr<ID3D11Resource> actualDepth;if(dsv)dsv->GetResource(&actualDepth);
    unsigned role=0;UINT writerSlot=UINT_MAX;
    ComPtr<ID3D11Resource> matched;
    D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};if(dsv)dsv->GetDesc(&depthView);
    if(forcedRole==5 && sourceContext){
        std::array<ID3D11UnorderedAccessView*,8> views{};context->CSGetUnorderedAccessViews(0,8,views.data());
        for(UINT i=0;i<views.size();++i)if(views[i]){ComPtr<ID3D11Resource> r;views[i]->GetResource(&r);
            D3D11_UNORDERED_ACCESS_VIEW_DESC v{};views[i]->GetDesc(&v);
            if(r.Get()==expected.resource.Get() && v.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2D && v.Texture2D.MipSlice==expected.mip){role=5;matched=r;writerSlot=i;Expected().sawCs=true;}
            views[i]->Release();}
    }else if(!forcedRole){
        role=ps==0x773B7BD7A2C6971Full?1:ps==0xB58064BC5041A048ull?2:0;
        if(sourceContext && ds.DepthEnable && ds.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL &&
            !(depthView.Flags&D3D11_DSV_READ_ONLY_DEPTH) && actualDepth.Get()==expected.resource.Get() &&
            depthView.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D && depthView.Texture2D.MipSlice==expected.mip){
            role=3;matched=actualDepth;Expected().sawDsv=true;
        }
        if(sourceContext && !matched){
            std::array<ID3D11RenderTargetView*,8> views{};context->OMGetRenderTargets(8,views.data(),nullptr);
            for(UINT i=0;i<views.size();++i)if(views[i]){ComPtr<ID3D11Resource> r;views[i]->GetResource(&r);
                D3D11_RENDER_TARGET_VIEW_DESC v{};views[i]->GetDesc(&v);
                if(r.Get()==expected.resource.Get() && v.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && v.Texture2D.MipSlice==expected.mip){role=4;matched=r;writerSlot=i;Expected().sawRtv=true;}
                views[i]->Release();}
        }
    }
    // Known blits always keep their reserved slots even if a source also matches.
    if(!forcedRole && (ps==0x773B7BD7A2C6971Full || ps==0xB58064BC5041A048ull))
        role=ps==0x773B7BD7A2C6971Full?1:2;
    if(!role || !serial)return;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> pixel;ComPtr<ID3D11ComputeShader> cs;
    if(role==5)context->CSGetShader(&cs,nullptr,nullptr);
    else {context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&pixel,nullptr,nullptr);}
    ComPtr<IUnknown> contextId,resourceId,vsId,psId,csId;
    auto* source=matched?matched.Get():actualDepth.Get();
    if(!CanonicalObject(context,contextId) || !CanonicalObject(source,resourceId) ||
       !CanonicalObject(vs.Get(),vsId) || !CanonicalObject(pixel.Get(),psId) || !CanonicalObject(cs.Get(),csId))return;
    const ObservationKey key{reinterpret_cast<uintptr_t>(contextId.Get()),reinterpret_cast<uintptr_t>(resourceId.Get()),
        reinterpret_cast<uintptr_t>(vsId.Get()),reinterpret_cast<uintptr_t>(psId.Get()),reinterpret_cast<uintptr_t>(csId.Get()),
        deviceEpoch,configEpoch,expected.mip,role};
    std::lock_guard samplesLock(SamplesMutex());
    auto& samples=Samples();const int slot=SelectSlot(samples,key);if(slot<0)return;
    auto* sample=&samples[slot];const auto now=GetTickCount64();
    if(!sample->plan.Due(now))return;
    if(!sample->plan.key.role){
        sample->plan.key=key;sample->plan.firstSerial=serial;
        sample->keyContext=contextId;sample->keyResource=resourceId;
        sample->keyVs=vsId;sample->keyPs=psId;sample->keyCs=csId;
    }
    if(role>=3){generation=0;frame=0;} // a prior SR owner never authenticates this new native draw
    const auto refresh=[&](){
        sample->serial=serial;sample->generation=generation;sample->sourceFrame=frame;sample->role=role;
        sample->thread=GetCurrentThreadId();sample->deviceEpoch=deviceEpoch;sample->configEpoch=configEpoch;
        sample->context=context;sample->dsv=dsv;sample->depthResource=source;
    };
    const auto metadata=[&](){
    const auto args=GenshinDepthMapping::CurrentDraw();
    RasterExport* exported=nullptr;
    for(auto& e:sample->exports)if(!e.serial){exported=&e;break;}
    if(exported){exported->serial=serial;
        if(role!=5 && args.Matches(context,args.kind,args.count) && args.observationCall){
            exported->call=args.observationCall;PendingRasterExport()=args.observationCall;
        }
    }
    LOG_INFO("Genshin depth evidence observation link: pid={} serial={} observation_slot={} call={} context={} tid={} resource={} mip={} device_epoch={} config_epoch={} returned_link={} claim=WAIT_ACTUAL_ASSOCIATION semantics=UNPROVED",
        GetCurrentProcessId(),serial,slot,exported?exported->call:0,reinterpret_cast<uintptr_t>(context),GetCurrentThreadId(),
        key.resource,expected.mip,deviceEpoch,configEpoch,
        exported && exported->call?"WAIT_SAME_ORIGINAL_DRAW_RETURN":"UNSUPPORTED_NO_EXACT_RASTER_CALL");
    LOG_INFO("Genshin depth evidence arguments: pid={} serial={} context={} tid={} kind={} count={} start={} base={} instances={} instance_start={} exact={} expected_prior_generation={} expected_prior_frame={} dsv_mip={} dsv_format={} dsv_flags={}",
        GetCurrentProcessId(),serial,reinterpret_cast<uintptr_t>(context),GetCurrentThreadId(),args.kind,args.count,args.start,args.base,args.instances,args.instanceStart,
        role!=5 && args.context==context && args.thread==GetCurrentThreadId() && args.instances==1 && args.kind>=1 && args.kind<=4,
        expected.generation,expected.frame,depthView.Texture2D.MipSlice,static_cast<unsigned>(depthView.Format),depthView.Flags);
    if(role==5)ShaderLog(cs.Get(),serial,"CS");
    else {ShaderLog(vs.Get(),serial,"VS");ShaderLog(pixel.Get(),serial,"PS");}
    LOG_INFO("Genshin depth evidence writer: pid={} serial={} binding_kind={} slot={} original_resource={} mip={} dispatch={},{},{} vs_object={} ps_object={} cs_object={} observation_slot={} semantics=UNPROVED",
        GetCurrentProcessId(),serial,role==3?"DSV":role==4?"RTV":role==5?"CS_UAV":"SCENE_BLIT",writerSlot,
        reinterpret_cast<uintptr_t>(sample->depthResource.Get()),expected.mip,dispatchX,dispatchY,dispatchZ,
        key.vs,key.ps,key.cs,slot);
    // Actual source inputs for a generated plane can expose an earlier stage.
    // Binding is an observation, never proof that that input caused a Z value.
    for(UINT slot=0;slot<16 && role>=3;++slot){ComPtr<ID3D11ShaderResourceView> srv;
        if(role==5)context->CSGetShaderResources(slot,1,&srv);else context->PSGetShaderResources(slot,1,&srv);
        if(!srv)continue;ComPtr<ID3D11Resource> r;srv->GetResource(&r);D3D11_SHADER_RESOURCE_VIEW_DESC d{};srv->GetDesc(&d);
        LOG_INFO("Genshin depth evidence writer input: serial={} stage={} slot={} resource={} view_format={} view_dimension={} first_mip={}",
            serial,role==5?"CS":"PS",slot,reinterpret_cast<uintptr_t>(r.Get()),static_cast<unsigned>(d.Format),static_cast<unsigned>(d.ViewDimension),d.Texture2D.MostDetailedMip);
    }
    UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    context->RSGetViewports(&viewportCount,viewports.data());
    LOG_INFO("Genshin depth evidence draw: pid={} serial={} role={} generation={} sourceFrame={} device_epoch={} config_epoch={} context={} depth_resource={} dsv={} depth_enable={} depth_write={} depth_func={} stencil={} ref={} viewport_count={} clear=SEE_ASSOCIATION projection=UNPROVED draw_offsets=SEE_ARGUMENTS",
        GetCurrentProcessId(),serial,role,generation,frame,deviceEpoch,configEpoch,reinterpret_cast<uintptr_t>(context),
        reinterpret_cast<uintptr_t>(sample->depthResource.Get()),reinterpret_cast<uintptr_t>(dsv.Get()),
        static_cast<bool>(ds.DepthEnable),static_cast<unsigned>(ds.DepthWriteMask),static_cast<unsigned>(ds.DepthFunc),
        static_cast<bool>(ds.StencilEnable),reference,viewportCount);
    {std::lock_guard lock(ClearMutex());for(const auto& record:Clears())if(role==3 && record.context==context &&
        record.thread==GetCurrentThreadId() && record.resource.Get()==sample->depthResource.Get() && record.mip==depthView.Texture2D.MipSlice)
        LOG_INFO("Genshin depth evidence clear association: serial={} resource={} clear_sequence={} clear={} projection=UNPROVED",
            serial,reinterpret_cast<uintptr_t>(record.resource.Get()),record.sequence,record.value);}
    for(UINT i=0;i<viewportCount;++i){const auto& v=viewports[i];
        LOG_INFO("Genshin depth evidence viewport: serial={} index={} rect={},{},{},{} depth={},{}",
            serial,i,v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth);}
    };
    const bool first=!sample->plan.metadataSeen;
    if(first){refresh();metadata();sample->plan.metadataSeen=true;}
    // Unknown/active queries never consume the one readback attempt. Probe at
    // most once/second per slot, log refusal once, retain no stale GPU snapshot.
    GenshinQueryScope::PrivateWork queryScope(context);
    if(!queryScope){
        sample->plan.Refused(now);
        if(!sample->plan.refusalLogged){sample->plan.refusalLogged=true;
            LOG_INFO("Genshin depth evidence readback: pid={} serial={} skipped=QUERY_SCOPE_UNPROVED metadata_only=true probe_ms=1000 depth_semantics=UNPROVED",
                GetCurrentProcessId(),serial);}
        return;
    }
    // This is a fresh invocation: update metadata before copying its CURRENT
    // inputs. The first refused draw's constants are never replayed or relabelled.
    sample->plan.readbackAttempted=true;
    if(!first){refresh();metadata();}
    LOG_INFO("Genshin depth evidence readback: pid={} serial={} first_metadata_serial={} fresh_current_inputs=true slot={} semantics=UNPROVED",
        GetCurrentProcessId(),serial,sample->plan.firstSerial,slot);
    ComPtr<ID3D11DeviceContext1> context1;context->QueryInterface(IID_PPV_ARGS(&context1));
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    // Allocate the completion witness before recording any copy. Roots keep
    // source/staging on query error; no speculative release or forced flush.
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    if(FAILED(device->CreateQuery(&query,&sample->query))){sample->reported=true;return;}
    for(UINT stage=0;stage<2;++stage)for(UINT slot=0;slot<14;++slot){
        auto& part=sample->parts[stage*14+slot];part.stage=role==5?4:stage;part.slot=slot;part.count=4096;
        if(role==5 && stage)continue;
        if(role==5){if(context1)context1->CSGetConstantBuffers1(slot,1,&part.source,&part.first,&part.count);else context->CSGetConstantBuffers(slot,1,&part.source);}
        else if(context1){if(!stage)context1->VSGetConstantBuffers1(slot,1,&part.source,&part.first,&part.count);
            else context1->PSGetConstantBuffers1(slot,1,&part.source,&part.first,&part.count);}
        else if(!stage)context->VSGetConstantBuffers(slot,1,&part.source);
        else context->PSGetConstantBuffers(slot,1,&part.source);
        if(!part.source)continue;
        D3D11_BUFFER_DESC desc{};part.source->GetDesc(&desc);
        const uint64_t offset=uint64_t(part.first)*16;
        if(!desc.ByteWidth || desc.ByteWidth>65536 || offset>=desc.ByteWidth){part.source.Reset();continue;}
        part.offset=static_cast<UINT>(offset);
        part.bytes=(std::min)(desc.ByteWidth-part.offset,part.count*16);
        if(sample->copiedBytes>=1024*1024 || desc.ByteWidth>1024*1024-sample->copiedBytes){part.source.Reset();continue;}
        if(!part.bytes){part.source.Reset();continue;}
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateBuffer(&desc,nullptr,&part.staging))){part.source.Reset();continue;}
        context->CopyResource(part.staging.Get(),part.source.Get());sample->copiedBytes+=desc.ByteWidth;++sample->count;
    }
    for(UINT slot=0;slot<5 && role!=5;++slot){
        auto& part=sample->parts[28+slot];part.stage=slot<4?2:3;part.slot=slot<4?slot:0;
        UINT stride=0,offset=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        if(slot<4)context->IAGetVertexBuffers(slot,1,&part.source,&stride,&offset);
        else context->IAGetIndexBuffer(&part.source,&format,&offset);
        if(!part.source)continue;
        D3D11_BUFFER_DESC desc{};part.source->GetDesc(&desc);
        part.first=offset;part.count=stride;
        if(offset>=desc.ByteWidth){part.source.Reset();continue;}
        part.bytes=(std::min)(65536u,desc.ByteWidth-offset);part.bytes-=part.bytes%4;
        if(!part.bytes || sample->copiedBytes+part.bytes>1536*1024){part.source.Reset();continue;}
        D3D11_BUFFER_DESC copy{};copy.ByteWidth=part.bytes;copy.Usage=D3D11_USAGE_STAGING;copy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateBuffer(&copy,nullptr,&part.staging))){part.source.Reset();continue;}
        D3D11_BOX range{offset,0,0,offset+part.bytes,1,1};
        context->CopySubresourceRegion(part.staging.Get(),0,0,0,0,part.source.Get(),0,&range);
        sample->copiedBytes+=part.bytes;++sample->count;
        LOG_INFO("Genshin depth evidence IA: serial={} stage={} slot={} resource={} source_bytes={} offset={} copied_bytes={} stride={} index_format={} layout=SEE_VS_SIGNATURE",
            serial,part.stage,part.slot,reinterpret_cast<uintptr_t>(part.source.Get()),desc.ByteWidth,offset,part.bytes,stride,static_cast<unsigned>(format));
    }
    D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
    LOG_INFO("Genshin depth evidence budget: serial={} samples_max=6 shader_bytes_max=17039360 gpu_bytes={} topology={}",serial,sample->copiedBytes,static_cast<unsigned>(topology));
    if(!sample->count){sample->reported=true;sample->query.Reset();sample->context.Reset();sample->dsv.Reset();sample->depthResource.Reset();return;}
    context->End(sample->query.Get());sample->pending=true;
}
// Called once for an actual game draw before the optional SR pipeline can
// drain or reject it. A prior pipeline/receipt is not needed to inspect the
// next writer, and its old frame identity is never passed as current.
inline void ObserveRaster(ID3D11DeviceContext* context,uint64_t ps,uint64_t deviceEpoch,uint64_t configEpoch)noexcept {
    PendingRasterExport()=0; // a later identical draw must never finish an old sample
    auto& expected=Expected();
    if(expected.context.Get()==context &&
       (expected.deviceEpoch!=deviceEpoch || expected.configEpoch!=configEpoch))ForgetDepthInput(context);
    static std::atomic<uint64_t> serial{0x4000000000000000ull};
    Observe(context,ps,serial.fetch_add(1,std::memory_order_relaxed)+1,0,0,deviceEpoch,configEpoch);
}
// The real Legacy entry supplies CURRENT epochs after Config->native locking.
inline void ObserveCompute(ID3D11DeviceContext* context,UINT x,UINT y,UINT z,uint64_t deviceEpoch,uint64_t configEpoch)noexcept {
    if(!LocationCurrent(context,GetCurrentThreadId(),deviceEpoch,configEpoch)){ForgetDepthInput(context);return;}
    static std::atomic<uint64_t> serial{0x8000000000000000ull};
    Observe(context,0,serial.fetch_add(1,std::memory_order_relaxed)+1,0,0,deviceEpoch,configEpoch,5,x,y,z);
}
}
