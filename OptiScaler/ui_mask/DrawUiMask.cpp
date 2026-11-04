// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DrawUiMask.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <native/SharedFrame.h>
#include <inputs/GenshinFsrContract.h>
#include <SysUtils.h>
#include <shaders/Shader_Common.h>
#include <Util.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <string_view>
#include <algorithm>
#include <mutex>

namespace DrawUiMask {
using Microsoft::WRL::ComPtr;
namespace obs=DrawObservationDx11;
namespace {
struct Rule {uint64_t ps=0,psBytes=0,vs=0,vsBytes=0;UINT count=0;};
struct Configuration {Settings settings;std::array<Rule,16> rules{};size_t count=0;uint64_t epoch=0;};
std::atomic<std::shared_ptr<const Configuration>> configured;
std::atomic_uint64_t nextConfiguration{0};
std::atomic_bool enabled{false};
std::atomic_uint64_t nextCapture{0};
struct LifecycleStore {
    std::mutex mutex;
    std::array<ConsumerStatus,3> consumers{};
};
LifecycleStore& Observations() noexcept {static LifecycleStore value;return value;}
void Publish(const Receipt& receipt,Consumer consumer) noexcept {
    auto& history=Observations().consumers[static_cast<unsigned>(consumer)];
    if(receipt.capture>=history.latest.capture)history.latest=receipt;
    if(receipt.state==Lifecycle::Completed && receipt.capture>=history.completed.capture)history.completed=receipt;
    if(receipt.state==Lifecycle::Quarantined && receipt.capture>=history.quarantined.capture)history.quarantined=receipt;
}
inline constexpr char MaskShader[]=R"(
Texture2D<float4> beforeUi:register(t0);
Texture2D<float4> afterUi:register(t1);
RWTexture2D<float> mask:register(u0);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 uint w,h;mask.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 // Visible changes from the explicitly selected terminal UI draws only.
 // This is not UI opacity, depth, OCR, or an automatic draw classifier.
 float4 a=beforeUi.Load(int3(id.xy,0)),b=afterUi.Load(int3(id.xy,0));
 mask[id.xy]=any(a!=b)?1.0:0.0;
})";
inline constexpr char ComposeShader[]=R"(
Texture2D<float> mask:register(t0);
Texture2D<float4> preserve:register(t1);
RWTexture2D<float4> result:register(u0);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 uint w,h;result.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 if(mask.Load(int3(id.xy,0))>0.0)result[id.xy]=preserve.Load(int3(id.xy,0));
})";
bool Number(std::string_view text,uint64_t& n,int base) noexcept {
    if(text.empty())return false;const auto r=std::from_chars(text.data(),text.data()+text.size(),n,base);
    return r.ec==std::errc{} && r.ptr==text.data()+text.size() && n!=0;
}
bool Parse(Configuration& c) noexcept {
    std::string_view input=c.settings.rules;
    while(!input.empty()){
        if(c.count==c.rules.size())return false;
        const auto end=input.find(';');auto row=input.substr(0,end);
        std::array<uint64_t,5> fields{};
        for(unsigned i=0;i<fields.size();++i){const auto split=row.find(':');
            if(!Number(row.substr(0,split),fields[i],(i==0 || i==2)?16:10))return false;
            if(i+1==fields.size()){if(split!=std::string_view::npos)return false;}
            else {if(split==std::string_view::npos)return false;row.remove_prefix(split+1);}}
        if(fields[1]>65536 || fields[3]>65536 || fields[4]>UINT_MAX)return false;
        c.rules[c.count++]={fields[0],fields[1],fields[2],fields[3],static_cast<UINT>(fields[4])};
        if(end==std::string_view::npos)break;input.remove_prefix(end+1);if(input.empty())return false;
    }
    return c.count!=0;
}
DXGI_FORMAT Sdr(DXGI_FORMAT f) noexcept {
    if(f==DXGI_FORMAT_R8G8B8A8_TYPELESS || f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return DXGI_FORMAT_R8G8B8A8_UNORM;
    if(f==DXGI_FORMAT_B8G8R8A8_TYPELESS || f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return DXGI_FORMAT_B8G8R8A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}
void Barrier(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) noexcept {
    if(before==after)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&b);
}
struct Protection {
    ComPtr<ID3D12Resource> before,result;
    ID3D12Resource* target=nullptr; // identity only; caller owns its real submission
    ComPtr<ID3D12DescriptorHeap> heap;
    bool begun=false,used=false,composed=false;
    bool restore=false,lookReading=false;
    ID3D12GraphicsCommandList* list=nullptr; // identity inside the existing Begin/End
    Receipt receipt; // protected by Observations().mutex, never GPU ownership
};
struct CaptureState;
CaptureState& State() noexcept;
} // namespace

class Frame final {
public:
    obs::Owner owner;
    uint64_t configEpoch=0,capture=0;
    Settings settings;
    UINT width=0,height=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    native::SharedTexture scene,mask;
    ID3D11Texture2D* target=nullptr; // identity only; never retained across ResizeBuffers
    ComPtr<ID3D11Texture2D> after;
    ComPtr<ID3D11ShaderResourceView> sceneSrv,afterSrv;
    ComPtr<ID3D11UnorderedAccessView> maskUav;
    ComPtr<ID3D11ComputeShader> maskShader;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<Protection,3> protection;
    bool armed=false,sealed=false,revoked=false;
    UINT returned=0;
    ~Frame(){scene.Reset();mask.Reset();}
    bool Create(ID3D11Device* device,const D3D11_TEXTURE2D_DESC& source) noexcept {
        obs::InternalScope internal;
        format=Sdr(source.Format);width=source.Width;height=source.Height;
        if(format!=DXGI_FORMAT_R8G8B8A8_UNORM || !width || !height || source.MipLevels!=1 || source.ArraySize!=1 ||
           source.SampleDesc.Count!=1 || source.SampleDesc.Quality)return false;
        if(!scene.Create(device,width,height,format,D3D11_BIND_SHADER_RESOURCE) ||
           !mask.Create(device,width,height,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS))return false;
        auto desc=source;desc.Format=format;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&after)) ||
           FAILED(device->CreateShaderResourceView(scene.Tex11(),nullptr,&sceneSrv)) ||
           FAILED(device->CreateShaderResourceView(after.Get(),nullptr,&afterSrv)) ||
           FAILED(device->CreateUnorderedAccessView(mask.Tex11(),nullptr,&maskUav)))return false;
        ComPtr<ID3DBlob> code;code.Attach(CompileShader(MaskShader,"main","cs_5_0"));
        return code && SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&maskShader));
    }
    bool Open(ID3D12Device* device) noexcept {
        if(device12)return device12.Get()==device;
        if(!device || !scene.Open(device,true) || !mask.Open(device,true))return false;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
        ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2};
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable={2,ranges};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=1;signature.pParameters=&parameter;
        ComPtr<ID3DBlob> serialized,error,code;
        if(FAILED(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) ||
           FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
        code.Attach(CompileShader(ComposeShader,"main","cs_5_0"));if(!code)return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
        desc.CS={code->GetBufferPointer(),code->GetBufferSize()};
        if(FAILED(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline))))return false;
        device12=device;return true;
    }
    bool MatchesColor(ID3D12Resource* color) const noexcept {
        if(!device12 || !color)return false;
        const auto d=color->GetDesc();ComPtr<ID3D12Device> owner12;
        if(FAILED(color->GetDevice(IID_PPV_ARGS(&owner12))) || owner12.Get()!=device12.Get() ||
           d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=width || d.Height!=height ||
           d.MipLevels!=1 || d.DepthOrArraySize!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality ||
           Sdr(d.Format)!=format)return false;
        return format==DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    bool Prepare(Consumer which,ID3D12Resource* color) noexcept {
        auto& p=protection[static_cast<unsigned>(which)];
        if(p.target)return p.target==color;
        if(p.begun || !MatchesColor(color))return false;
        const auto d=color->GetDesc();
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        auto texture=d;texture.Format=format;texture.Flags=D3D12_RESOURCE_FLAG_NONE;
        if(which!=Consumer::Fg && !p.before && FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&p.before))))return false;
        texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(!p.result && FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&p.result))))return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(!p.heap && FAILED(device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p.heap))))return false;
        p.target=color;return true;
    }
    void Compose(Consumer which,ID3D12GraphicsCommandList* cmd,ID3D12Resource* preserve,ID3D12Resource* color,D3D12_RESOURCE_STATES state) noexcept {
        auto& p=protection[static_cast<unsigned>(which)];
        // Seed the private UNORM result with exact encoded color bytes, then
        // overwrite only masked texels. No source SRV or extra full image is
        // needed; typed sRGB/typeless game targets are copied by format family.
        Barrier(cmd,color,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd,p.result.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(p.result.Get(),color);
        Barrier(cmd,p.result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmd,color,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        auto handle=p.heap->GetCPUDescriptorHandleForHeapStart();const auto stride=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        srv.Format=DXGI_FORMAT_R8_UNORM;device12->CreateShaderResourceView(mask.Res12(),&srv,handle);handle.ptr+=stride;
        srv.Format=format;device12->CreateShaderResourceView(preserve,&srv,handle);handle.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;uav.Format=format;
        device12->CreateUnorderedAccessView(p.result.Get(),nullptr,&uav,handle);
        Barrier(cmd,mask.Res12(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,preserve,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        ID3D12DescriptorHeap* heaps[]{p.heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipeline.Get());
        cmd->SetComputeRootDescriptorTable(0,p.heap->GetGPUDescriptorHandleForHeapStart());
        cmd->Dispatch((width+7)/8,(height+7)/8,1);
        Barrier(cmd,p.result.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,preserve,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,mask.Res12(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
};

namespace {
struct CaptureState {
    std::shared_ptr<const Configuration> config;
    std::array<FramePtr,BUFFER_COUNT+2> pool;
    FramePtr capture,sealed;
    std::array<ID3D11Texture2D*,8> buffers{};
    IDXGISwapChain* chain=nullptr; // comparison only; no private swapchain lifetime
    const char* status="disabled";
    bool rejectedTail=false;
};
CaptureState& State() noexcept {static thread_local CaptureState state;return state;}
// Weak borrowing only. NativeProducer's held input owns Frame across its real
// final Signal, completion or quarantine; this context grants no GPU lifetime.
thread_local std::weak_ptr<Frame> layerLookFrame;
FramePtr LayerLookFrame(ID3D12GraphicsCommandList* cmd,ID3D12Resource* color,UINT width,UINT height) noexcept {
    auto f=layerLookFrame.lock();
    if(!Enabled() || !Selected(f,Consumer::Filter) || !cmd || cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
       f->configEpoch!=nextConfiguration.load(std::memory_order_acquire) || f->width!=width || f->height!=height ||
       !f->mask.Res12())return {};
    const auto& p=f->protection[static_cast<unsigned>(Consumer::Nr)];
    return p.begun && !p.lookReading && p.list==cmd && p.target==color ? f : FramePtr{};
}
void Invalidate(ID3D11DeviceContext*) noexcept {
    auto& s=State();if(s.capture && s.capture->armed){s.capture->revoked=true;s.rejectedTail=true;s.status="non-ui-write-after-ui";}
}
void Before(ID3D11DeviceContext* context,const obs::Draw& draw,const obs::Owner& owner,obs::CapturedState& actual,uint64_t& token) noexcept {
    auto& s=State();auto* frame=s.capture.get();
    if(!frame || !s.config || !Enabled() || !owner.Matches(context) || frame->owner.epoch!=owner.epoch || frame->revoked || s.rejectedTail)return;
    if(s.config->epoch!=nextConfiguration.load(std::memory_order_acquire)){Invalidate(context);return;}
    // Only explicitly supplied shapes reach getters. After the first selected
    // UI draw, any unselected draw conservatively revokes the ENTIRE tail,
    // including draws into another target: no all-draw OM/IB/PS scan.
    bool shape=false;
    if(draw.countKnown && draw.count && draw.instances==1 && static_cast<unsigned>(draw.kind)<=4)
        for(size_t i=0;i<s.config->count;++i)shape|=s.config->rules[i].count==draw.count;
    if(!shape){Invalidate(context);return;}
    const auto pixel=actual.PixelIdentity();bool psMatch=false;
    for(size_t i=0;i<s.config->count;++i){const auto& r=s.config->rules[i];
        psMatch|=r.count==draw.count && r.ps==pixel.hash && r.psBytes==pixel.bytes;}
    if(!psMatch){Invalidate(context);return;}
    const auto vertex=actual.VertexIdentity();bool match=false;
    for(size_t i=0;i<s.config->count;++i){const auto& r=s.config->rules[i];
        match|=r.count==draw.count && r.ps==pixel.hash && r.psBytes==pixel.bytes && r.vs==vertex.hash && r.vsBytes==vertex.bytes;}
    if(!match){Invalidate(context);return;}
    ComPtr<ID3D11RenderTargetView> view;context->OMGetRenderTargets(1,&view,nullptr);
    if(!view){Invalidate(context);return;}
    D3D11_RENDER_TARGET_VIEW_DESC vd{};view->GetDesc(&vd);
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> target;if(FAILED(resource.As(&target)) || vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice){Invalidate(context);return;}
    bool knownBuffer=false;for(const auto& buffer:s.buffers)knownBuffer|=buffer==target.Get();
    D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
    if(!knownBuffer || td.Width!=frame->width || td.Height!=frame->height || Sdr(td.Format)!=frame->format ||
       Sdr(vd.Format)!=frame->format || (frame->armed && frame->target!=target.Get())){Invalidate(context);return;}
    if(!frame->armed){obs::InternalScope internal;
        context->CopyResource(frame->scene.Tex11(),target.Get());frame->target=target.Get();frame->armed=true;}
    token=owner.epoch;
}
void After(ID3D11DeviceContext* context,const obs::Draw&,const obs::Owner& owner,uint64_t token,obs::Completion result) noexcept {
    if(!token)return;auto& s=State();auto f=s.capture;
    if(!f || token!=owner.epoch || f->owner.epoch!=owner.epoch)return;
    if(result!=obs::Completion::OriginalReturned){Invalidate(context);s.status="selected-draw-not-original-returned";return;}
    ++f->returned;
}
void Seal(ID3D11DeviceContext* context,ID3D11Texture2D* picture) noexcept {
    auto& s=State();auto f=s.capture;s.sealed.reset();
    if(!f || !f->armed || f->revoked || !f->returned || f->target!=picture || !f->owner.Matches(context))return;
    obs::InternalScope internal;
    // Save and restore the exact compute slots touched. OM/IA/VS/PS stay intact.
    ComPtr<ID3D11ComputeShader> shader;std::array<ID3D11ClassInstance*,256> classes{};UINT classCount=static_cast<UINT>(classes.size());
    context->CSGetShader(&shader,classes.data(),&classCount);
    std::array<ID3D11ShaderResourceView*,2> previous{};ID3D11UnorderedAccessView* previousUav=nullptr;
    context->CSGetShaderResources(0,2,previous.data());context->CSGetUnorderedAccessViews(0,1,&previousUav);
    context->CopyResource(f->after.Get(),picture);
    ID3D11ShaderResourceView* inputs[]{f->sceneSrv.Get(),f->afterSrv.Get()};auto* output=f->maskUav.Get();
    context->CSSetShader(f->maskShader.Get(),nullptr,0);context->CSSetShaderResources(0,2,inputs);context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
    context->Dispatch((f->width+7)/8,(f->height+7)/8,1);
    ID3D11ShaderResourceView* empty[2]{};ID3D11UnorderedAccessView* emptyUav=nullptr;
    context->CSSetUnorderedAccessViews(0,1,&emptyUav,nullptr);context->CSSetShaderResources(0,2,empty);
    context->CSSetShader(shader.Get(),classes.data(),classCount);context->CSSetShaderResources(0,2,previous.data());
    context->CSSetUnorderedAccessViews(0,1,&previousUav,nullptr);
    for(auto* c:classes)if(c)c->Release();for(auto* v:previous)if(v)v->Release();if(previousUav)previousUav->Release();
    f->sealed=true;f->armed=false;s.sealed=f;s.status="sealed-selected-ui-tail-visible-difference";
}
void Recorded(const FramePtr& f,Consumer consumer) noexcept {
    std::lock_guard lock(Observations().mutex);
    auto& receipt=f->protection[static_cast<unsigned>(consumer)].receipt;
    receipt={Lifecycle::Pending,f->capture,f->owner.frame,f->owner.epoch,f->configEpoch,
        reinterpret_cast<uintptr_t>(f->owner.context.Get()),0,0,0};
    Publish(receipt,consumer);
}
const obs::Consumer callbacks{Before,After,nullptr,Revoke,Invalidate};
} // namespace

void Configure(const Settings& settings) noexcept {
    try {
        auto c=std::make_shared<Configuration>();c->settings=settings;c->epoch=++nextConfiguration;
        const bool game=GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
        const bool active=settings.enabled && (settings.fg || settings.nr || settings.filters) && game && Parse(*c);
        configured.store(std::move(c),std::memory_order_release);enabled.store(active,std::memory_order_release);
        obs::Subscribe(obs::Slot::UiMask,&callbacks,active);
    }catch(...){enabled.store(false,std::memory_order_release);obs::Subscribe(obs::Slot::UiMask,nullptr,false);}
}
bool Enabled() noexcept{return enabled.load(std::memory_order_acquire);}
void Revoke() noexcept {
    layerLookFrame.reset();
    auto& s=State();if(s.capture)s.capture->revoked=true;s.capture.reset();s.sealed.reset();
    s.config.reset();s.chain=nullptr;s.buffers.fill(nullptr);for(auto& slot:s.pool)slot.reset();s.status="revoked";
}
void Present(IDXGISwapChain* chain,ID3D11DeviceContext* context) noexcept {
    if(!Enabled() || !chain || !context){Revoke();return;}
    try {
        auto& s=State();auto config=configured.load(std::memory_order_acquire);
        if(!config)return;
        ComPtr<IDXGISwapChain3> indexed;UINT index=0;
        if(SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&indexed))))index=indexed->GetCurrentBackBufferIndex();
        ComPtr<ID3D11Texture2D> target;if(FAILED(chain->GetBuffer(index,IID_PPV_ARGS(&target)))){Revoke();return;}
        Seal(context,target.Get());
        // The public owner has already advanced at this Present. Matching the
        // previous epoch was checked inside Seal; next draw uses the new epoch.
        if(!obs::owner || !obs::owner->Matches(context)){Revoke();return;}
        D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);
        if(s.chain!=chain || std::none_of(s.buffers.begin(),s.buffers.end(),[&](const auto& b){return b==target.Get();})){
            s.buffers.fill(nullptr);DXGI_SWAP_CHAIN_DESC cd{};
            if(FAILED(chain->GetDesc(&cd)) || cd.BufferCount>s.buffers.size()){Revoke();return;}
            for(UINT i=0;i<cd.BufferCount;++i){ComPtr<ID3D11Texture2D> buffer;
                if(FAILED(chain->GetBuffer(i,IID_PPV_ARGS(&buffer)))){Revoke();return;}
                s.buffers[i]=buffer.Get();}
            s.chain=chain;
        }
        s.capture.reset();s.config=std::move(config);s.rejectedTail=false;
        // Pool slots remain owned by real NativeProducer/OwnedRuntime/FG
        // retirement records. If all are busy, skip; never wait or overwrite.
        for(auto& slot:s.pool)if(!slot || slot.use_count()==1){
            if(!slot || slot->width!=desc.Width || slot->height!=desc.Height || slot->format!=Sdr(desc.Format) || slot->owner.device.Get()!=obs::owner->device.Get()){
                auto fresh=std::make_shared<Frame>();if(!fresh->Create(obs::owner->device.Get(),desc)){s.status="unsupported-format-or-resource-creation";return;}slot=std::move(fresh);}
            // Immutable frame descriptors from an earlier submission are never
            // edited while any actual consumer holds that pool entry.
            slot->owner=*obs::owner;slot->settings=s.config->settings;slot->configEpoch=s.config->epoch;
            slot->capture=nextCapture.fetch_add(1,std::memory_order_relaxed)+1;
            slot->target=nullptr;slot->armed=slot->sealed=slot->revoked=false;slot->returned=0;
            for(auto& p:slot->protection){p.begun=p.used=p.composed=p.restore=p.lookReading=false;
                p.target=nullptr;p.list=nullptr;p.receipt={};}
            s.capture=slot;return;
        }
        s.status="all-mask-owners-in-flight";
    }catch(...){Revoke();}
}
FramePtr Acquire(ID3D11DeviceContext* context,ID3D11Texture2D* target,ID3D12Device* device) noexcept {
    auto f=State().sealed;
    if(!Enabled() || !f || !f->sealed || f->revoked || !f->owner.Matches(context) ||
       f->target!=target || f->configEpoch!=nextConfiguration.load(std::memory_order_acquire) || !f->Open(device))return {};
    return f;
}
bool Selected(const FramePtr& f,Consumer c) noexcept {
    return f && f->sealed && !f->revoked &&
        (c==Consumer::Nr?f->settings.nr:c==Consumer::Filter?f->settings.filters:f->settings.fg);
}
bool Begin(const FramePtr& f,Consumer c,ID3D12GraphicsCommandList* cmd,ID3D12Resource* color,D3D12_RESOURCE_STATES state) noexcept {
    // A failed/new bracket must not lend a previous callback's frame.
    if(c==Consumer::Nr)layerLookFrame.reset();
    const bool restore=Selected(f,c);
    const bool layerReader=c==Consumer::Nr && Selected(f,Consumer::Filter);
    if((!restore && !layerReader) || !cmd || cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
    if(restore ? !f->Prepare(c,color) : !f->MatchesColor(color))return false;
    auto& p=f->protection[static_cast<unsigned>(c)];if(p.begun || p.used)return false;
    p.used=true;p.restore=restore;p.target=color;p.list=cmd;
    if(restore){
        Barrier(cmd,color,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd,p.before.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(p.before.Get(),color);
        Barrier(cmd,p.before.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,color,D3D12_RESOURCE_STATE_COPY_SOURCE,state);Recorded(f,c);
    }
    // A Filter-only bracket allocates/copies nothing and publishes no receipt.
    // Layer-only reads have no separate completion receipt. NR global compose
    // and the Filter runtime retain their own existing observation semantics.
    p.begun=true;if(layerReader)layerLookFrame=f;return true;
}
bool End(const FramePtr& f,Consumer c,ID3D12GraphicsCommandList* cmd,ID3D12Resource* color,D3D12_RESOURCE_STATES state) noexcept {
    if(!f || !cmd)return false;auto& p=f->protection[static_cast<unsigned>(c)];
    if(!p.begun || p.list!=cmd || p.target!=color || p.lookReading)return false;
    if(p.restore){
        f->Compose(c,cmd,p.before.Get(),color,state);
        Barrier(cmd,p.result.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd,color,state,D3D12_RESOURCE_STATE_COPY_DEST);cmd->CopyResource(color,p.result.Get());
        Barrier(cmd,color,D3D12_RESOURCE_STATE_COPY_DEST,state);
        Barrier(cmd,p.result.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        p.composed=true;
    }
    p.begun=false;p.list=nullptr;if(c==Consumer::Nr)layerLookFrame.reset();return true;
}
bool LayerLookAvailable(ID3D12GraphicsCommandList* cmd,ID3D12Resource* color,UINT width,UINT height) noexcept {
    return bool(LayerLookFrame(cmd,color,width,height));
}
LayerLookRead::LayerLookRead(ID3D12GraphicsCommandList* cmd,ID3D12Resource* color,UINT width,UINT height) noexcept
    : _frame(LayerLookFrame(cmd,color,width,height)),_list(cmd) {
    if(!_frame)return;
    _frame->protection[static_cast<unsigned>(Consumer::Nr)].lookReading=true;
    Barrier(_list,_frame->mask.Res12(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
LayerLookRead::~LayerLookRead() {
    if(!_frame)return;
    Barrier(_list,_frame->mask.Res12(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    _frame->protection[static_cast<unsigned>(Consumer::Nr)].lookReading=false;
}
ID3D12Resource* LayerLookRead::Resource() const noexcept {return _frame?_frame->mask.Res12():nullptr;}
ID3D12Resource* BuildHudless(const FramePtr& f,ID3D12GraphicsCommandList* cmd,ID3D12Resource* color,D3D12_RESOURCE_STATES state) noexcept {
    if(!Selected(f,Consumer::Fg) || !cmd || cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT || !f->Prepare(Consumer::Fg,color))return nullptr;
    auto& p=f->protection[static_cast<unsigned>(Consumer::Fg)];if(p.used)return nullptr;p.used=true;
    Recorded(f,Consumer::Fg);
    f->Compose(Consumer::Fg,cmd,f->scene.Res12(),color,state);p.composed=true;
    return f->protection[static_cast<unsigned>(Consumer::Fg)].result.Get();
}
ID3D12Resource* Hudless(const FramePtr& f) noexcept {
    return Selected(f,Consumer::Fg) && f->protection[static_cast<unsigned>(Consumer::Fg)].used?
        f->protection[static_cast<unsigned>(Consumer::Fg)].result.Get():nullptr;
}
void Observe(const FramePtr& f,Consumer consumer,Lifecycle state,uintptr_t owner,uint64_t generation,uint64_t submission) noexcept {
    const auto index=static_cast<unsigned>(consumer);
    if(!f || index>=f->protection.size() || submission==UINT64_MAX)return;
    const bool abandoned=state==Lifecycle::Unknown && !owner && !generation && !submission;
    if(!abandoned && (!owner || !generation))return;
    if((state==Lifecycle::Submitted || state==Lifecycle::Completed) && !submission)return;
    std::lock_guard lock(Observations().mutex);
    auto& receipt=f->protection[index].receipt;
    if(!receipt.capture || receipt.capture!=f->capture || receipt.configuration!=f->configEpoch ||
       receipt.state==Lifecycle::Completed || receipt.state==Lifecycle::Quarantined)return;
    if(!abandoned && receipt.owner && (receipt.owner!=owner || receipt.generation!=generation ||
       (submission && receipt.submission && receipt.submission!=submission)))return;
    // A completed upload, recording, tag or CPU callback cannot establish use.
    if(state==Lifecycle::Completed && receipt.state!=Lifecycle::Submitted)return;
    if(state==Lifecycle::Pending || (state==Lifecycle::Unknown && receipt.state==Lifecycle::Submitted))return;
    // Begin's pre-effect copy alone is not a mask consumer. End/BuildHudless
    // must have actually recorded the compose before any submission claim.
    if(state==Lifecycle::Submitted && !f->protection[index].composed)state=Lifecycle::Unknown;
    if(!abandoned){receipt.owner=owner;receipt.generation=generation;}
    if(submission)receipt.submission=submission;
    receipt.state=state;
    Publish(receipt,consumer);
}
LifecycleStatus ReadLifecycle() noexcept {
    LifecycleStatus result;
    std::unique_lock lock(Observations().mutex,std::try_to_lock);
    if(!lock.owns_lock())return result;
    result.readable=true;result.enabled=Enabled();
    result.configuration=nextConfiguration.load(std::memory_order_acquire);
    result.consumers=Observations().consumers;
    return result;
}
const char* Status() noexcept{return State().status;}
} // namespace DrawUiMask
