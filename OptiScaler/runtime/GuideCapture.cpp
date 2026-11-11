// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuideCapture.h"
#include "../shaders/preview/GuidePreviewShader.h"
#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <mutex>

namespace GuideCapture {
namespace PreviewDetail {bool Poll();bool PrepareOne();bool Executing(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,ExecutionReceipt&);bool Submitted(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,const ExecutionReceipt&);bool Reset(ID3D12GraphicsCommandList*);}
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned Slots=3,MaxWidth=160,MaxHeight=100,SampleBytes=32;
constexpr size_t MaxPixels=MaxWidth*MaxHeight,MaxText=4096;
struct Constants {uint32_t colour[4],depth[4],motion[4],answer[4],width,height,flags,pad;};
static_assert(sizeof(Constants)==80);
constexpr char Shader[]=R"(
cbuffer Params:register(b0) {uint4 cr;uint4 dr;uint4 mr;uint4 ar;uint w;uint h;uint flags;uint unused;}
Texture2D<float4> colour:register(t0);
Texture2D<float> depth:register(t1);
Texture2D<float2> motion:register(t2);
Texture2D<float4> contribution:register(t3);
RWStructuredBuffer<float4> result:register(u0);
uint2 address(uint2 p,uint4 r){return r.xy+min(r.zw-1,(uint2)((float2(p)+0.5)*float2(r.zw)/float2(w,h)));}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
 if(p.x>=w||p.y>=h)return;uint i=(p.y*w+p.x)*2;
 float4 fused=(flags&8)?colour.Load(int3(address(p.xy,cr),0)):float4(0,0,0,0);
 result[i]=fused;
 float z=(flags&1)?depth.Load(int3(address(p.xy,dr),0)):-1;
 float2 v=(flags&2)?motion.Load(int3(address(p.xy,mr),0)):float2(0,0);
 float a=-1;if(flags&4){float4 fresh=contribution.Load(int3(address(p.xy,ar),0));
 if(flags&16){if(all(isfinite(fused.rgb))&&all(isfinite(fresh.rgb)))
 a=max(max(abs(fused.r-fresh.r),abs(fused.g-fresh.g)),abs(fused.b-fresh.b));}
 else a=fresh.r;}
 result[i+1]=float4(z,v,a);
})";

struct Pipeline {ComPtr<ID3D12Device> device;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;};
enum class Phase {Ready,Recording,Recorded,Executing,NeedsSignal,Signalling,WaitingGpu,Decoding,Decoded,Published,Discarded,Cleaning,Quarantined};
struct Key {uint64_t source=0,feature=0,configuration=0,owner=0,tick=0,serial=0;};
bool SameDomain(const Key& a,const Key& b){return a.feature==b.feature&&a.configuration==b.configuration&&a.owner==b.owner;}
bool Earlier(const Key& a,const Key& b){
    if(SameDomain(a,b)&&a.source&&b.source&&a.source!=b.source)return a.source<b.source;
    return a.serial<b.serial;
}
struct Batch {
    std::shared_ptr<Pipeline> pipeline;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12Resource>,4> inputs;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12Resource> output,readback;ComPtr<ID3D12Fence> fence;
    Image image;Key key;uint64_t generation=0,value=0,pairId=0;unsigned pairIndex=0;
    Phase phase=Phase::Ready;bool reset=false,pre=false,post=false,compare=false;
    D3D12_RESOURCE_STATES outputState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,recordStartOutputState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
struct Cursor {Key key;bool valid=false;};
struct Lane {
    std::array<std::shared_ptr<Batch>,Slots> batches;
    ComPtr<ID3D12Device> requestedDevice;
    std::atomic<const char*> status{"No current full-frame sample"};std::atomic<uint64_t> last{0};
    std::array<Cursor,16> cursors{}; // worker only, bounded per-domain ordering
    uint64_t retryAfter=0; // worker-only backoff for unsupported devices
    std::shared_ptr<const Image> latest;Key latestKey;
    std::atomic<uint64_t> pairDeadline{0},pairId{0};
    std::atomic<unsigned> pairCaptured{0};
    Key pairFirst;uint64_t pairEpoch=0;
    std::array<std::shared_ptr<const Image>,2> pairImages;
};
struct State {
    // Lifecycle lock: bounded state transitions only. No D3D calls, allocation,
    // compilation, Map, image copies, or reference destruction under this lock.
    std::mutex lifecycle,publication;
    std::atomic<uint64_t> demand{0};std::atomic<bool> calibration{false},running{false};
    std::atomic<unsigned> streams{0};
    std::atomic<uint64_t> historyMeasurementDeadline{0};
    // Exact active recordings; unmatched hook calls never take lifecycle.
    std::array<std::atomic<ID3D12CommandList*>,Slots*static_cast<unsigned>(Stream::Count)> active{};
    std::array<Lane,static_cast<unsigned>(Stream::Count)> lanes;
    uint64_t serial=0,generation=0;
    std::vector<std::shared_ptr<Pipeline>> pipelines; // worker only, max four
    std::deque<std::shared_ptr<const Image>> sources;
    HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
};
// Uncertain executable generations deliberately stay pinned for process life.
State& Storage(){static auto* s=new State;return *s;}
void Wake(State& s){if(s.wake)SetEvent(s.wake);}
void Status(Lane& lane,const char* text){lane.status.store(text,std::memory_order_release);}
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    if(!resource||a==b)return;D3D12_RESOURCE_BARRIER r{};r.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    r.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&r);
}
bool Region(ID3D12Resource* resource,GuideDiagnostics::Rect& r){
    if(!resource)return false;const auto d=resource->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||d.MipLevels!=1||
       d.SampleDesc.Count!=1||(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||d.Width>UINT32_MAX)return false;
    if(!r.width&&!r.height)r={0,0,static_cast<unsigned>(d.Width),d.Height};
    return r.width&&r.height&&r.x<d.Width&&r.y<d.Height&&r.width<=d.Width-r.x&&r.height<=d.Height-r.y;
}
DXGI_FORMAT ViewFormat(DXGI_FORMAT f,unsigned channel){
    if(channel==1){switch(f){
    case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R32_TYPELESS:return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:case DXGI_FORMAT_R16_TYPELESS:return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_R24G8_TYPELESS:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default:break;}}
    switch(f){case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R11G11B10_FLOAT:return f;
    default:return DXGI_FORMAT_UNKNOWN;}
}
bool Rgb(DXGI_FORMAT f){switch(f){
    case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R11G11B10_FLOAT:return true;
    default:return false;}
}
std::shared_ptr<Pipeline> GetPipeline(State& s,ID3D12Device* device){
    for(auto& p:s.pipelines)if(p->device.Get()==device)return p;
    if(s.pipelines.size()>=4)return {};
    ComPtr<ID3DBlob> code,error,serialized;
    if(FAILED(D3DCompile(Shader,sizeof(Shader)-1,"F6 guide diagnostics",nullptr,nullptr,"main","cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return {};
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,4}};
    D3D12_ROOT_PARAMETER roots[2]{};roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    roots[0].DescriptorTable={2,ranges};roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[1].Constants={0,0,20};D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=roots;
    if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)))return {};
    auto p=std::make_shared<Pipeline>();p->device=device;
    if(FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&p->root))))return {};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=p->root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p->pso))))return {};
    s.pipelines.push_back(p);return p;
}
std::shared_ptr<Batch> Prepare(State& s,ID3D12Device* device){
    auto p=GetPipeline(s,device);if(!p)return {};
    auto b=std::make_shared<Batch>();b->pipeline=std::move(p);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&b->heap))))return {};
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=MaxPixels*SampleBytes;
    bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&b->output))))return {};
    hp.Type=D3D12_HEAP_TYPE_READBACK;bd.Flags=D3D12_RESOURCE_FLAG_NONE;
    if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&b->readback)))||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&b->fence))))return {};
    auto& im=b->image;im.colour.reserve(MaxPixels);im.depth.reserve(MaxPixels);im.motion.reserve(MaxPixels);im.contribution.reserve(MaxPixels);
    im.stamp.reason.reserve(MaxText);im.stamp.colourChoice.reserve(MaxText);im.stamp.resetReason.reserve(MaxText);
    return b;
}
bool Decode(Batch& b){
    auto& im=b.image;const size_t count=size_t(im.width)*im.height,bytes=count*SampleBytes;
    const D3D12_RANGE range{0,bytes};void* data=nullptr;
    if(FAILED(b.readback->Map(0,&range,&data))||!data)return false;
    const auto* v=static_cast<const float*>(data);
    im.colour.resize(count);im.depth.resize(count);im.motion.resize(count);im.contribution.resize(count);
    size_t valid=0,changed=0;
    for(size_t i=0;i<count;++i){
        im.colour[i]={v[i*8],v[i*8+1],v[i*8+2],v[i*8+3]};im.depth[i]=v[i*8+4];
        im.motion[i]={v[i*8+5],v[i*8+6]};const float delta=v[i*8+7];im.contribution[i]=delta;
        if(std::isfinite(delta)&&delta>=0){++valid;if(delta>ContributionThreshold)++changed;}
    }
    const D3D12_RANGE written{0,0};b.readback->Unmap(0,&written);
    im.completedTick=GuideDiagnostics::Now();im.valid=true;im.stamp.stage=GuideDiagnostics::Stage::Completed;
    // Keep stamp.tick: late completion must not rejuvenate stale source data.
    if(im.stream==Stream::History&&b.compare){
        im.stamp.contributionKnown=valid!=0;im.stamp.contribution=valid?float(changed)/float(valid):-1;
        im.stamp.coverage=count?float(valid)/float(count):0;
        im.stamp.reason=valid?"Actual fused/fresh RGB changed sample ratio; abs max RGB > 1e-5 (not blend weight)":
            "Actual fused/fresh comparison completed; no finite valid RGB samples";
    }
    return true;
}
bool AdvanceCursor(Lane& lane,const Key& key){
    Cursor* target=nullptr;for(auto& c:lane.cursors){
        if(c.valid&&SameDomain(c.key,key)){target=&c;break;}
        if(!c.valid&&!target)target=&c;
    }
    if(!target)target=&*std::min_element(lane.cursors.begin(),lane.cursors.end(),[](auto& a,auto& b){return a.key.serial<b.key.serial;});
    if(target->valid&&SameDomain(target->key,key)&&key.source&&target->key.source&&key.source<=target->key.source)return false;
    target->key=key;target->valid=true;return true;
}
// The only caller is the worker. No expensive operation holds lifecycle.
bool Poll(State& s){
    bool pending=false;
    for(auto& lane:s.lanes)for(unsigned slot=0;slot<Slots;++slot){
        std::shared_ptr<Batch> b;Phase phase;
        {std::lock_guard lock(s.lifecycle);b=lane.batches[slot];if(!b)continue;phase=b->phase;
            if(phase==Phase::NeedsSignal)b->phase=Phase::Signalling;}
        if(phase==Phase::NeedsSignal){
            // This post-Execute receipt may follow Reset. Signal the exact
            // queue; later work covered by this fence is conservatively safe.
            const HRESULT hr=b->queue->Signal(b->fence.Get(),b->value);
            std::lock_guard lock(s.lifecycle);
            if(FAILED(hr)){b->phase=Phase::Quarantined;Status(lane,"Diagnostic Signal failed; resources retained");}
            else if(b->phase==Phase::Signalling)b->phase=Phase::WaitingGpu;
            pending=true;continue;
        }
        if(phase==Phase::WaitingGpu){
            const auto completed=b->fence->GetCompletedValue();
            if(completed==UINT64_MAX){std::lock_guard lock(s.lifecycle);b->phase=Phase::Quarantined;
                Status(lane,"Diagnostic device removed; resources retained");continue;}
            if(completed<b->value){pending=true;continue;}
            {std::lock_guard lock(s.lifecycle);if(b->phase!=Phase::WaitingGpu)continue;b->phase=Phase::Decoding;}
            const bool ok=Decode(*b);
            {std::lock_guard lock(s.lifecycle);if(b->phase==Phase::Decoding)b->phase=Phase::Decoded;
                if(!ok){b->image.valid=false;Status(lane,"Completed diagnostic readback Map failed");}}
            pending=true;continue;
        }
        // Decoded storage remains owned until Reset, which explicitly wakes
        // this worker. GPU completion needs no further 2ms polling here.
    }
    // Physical slots are not temporal order. Defer a decoded image while a
    // lower source in the same domain is pending; slots [4,2,3] emit [2,3,4].
    for(auto& lane:s.lanes)for(unsigned attempt=0;attempt<Slots;++attempt){
        std::shared_ptr<Batch> chosen;
        {std::lock_guard lock(s.lifecycle);
            for(auto& b:lane.batches)if(b&&b->phase==Phase::Decoded&&b->reset&&(!chosen||Earlier(b->key,chosen->key)))chosen=b;
            if(!chosen)break;bool blocked=false;
            for(auto& b:lane.batches)if(b&&b!=chosen&&b->phase!=Phase::Ready&&b->phase!=Phase::Published&&b->phase!=Phase::Discarded&&b->phase!=Phase::Cleaning&&
                b->phase!=Phase::Quarantined&&SameDomain(b->key,chosen->key)&&Earlier(b->key,chosen->key))blocked=true;
            // GPU waits above already request polling. Recorded/decoded older
            // sources instead advance on Execute/post/Reset events; repeatedly
            // polling cannot supply the missing receipt or close a recording.
            if(blocked)break;chosen->phase=Phase::Published;
        }
        if(!chosen->image.valid||!AdvanceCursor(lane,chosen->key))continue;
        std::shared_ptr<const Image> image=std::make_shared<Image>(chosen->image),old,discarded;
        if(image->stream==Stream::History&&chosen->compare)GuideDiagnostics::Publish(GuideDiagnostics::Consumer::NrHistory,image->stamp);
        {std::lock_guard lock(s.publication);
            const bool newer=!lane.latest||(SameDomain(lane.latestKey,chosen->key)?!Earlier(chosen->key,lane.latestKey):
                (chosen->key.tick>lane.latestKey.tick||(chosen->key.tick==lane.latestKey.tick&&chosen->key.serial>lane.latestKey.serial)));
            if(newer){old=std::move(lane.latest);lane.latest=image;lane.latestKey=chosen->key;}
            if(chosen->pairId&&chosen->pairId==lane.pairId.load(std::memory_order_acquire)){
                lane.pairImages[chosen->pairIndex]=image;
            }
            if(image->stream==Stream::Source&&s.calibration.load(std::memory_order_acquire)){
                if(s.sources.size()==8){discarded=std::move(s.sources.front());s.sources.pop_front();}
                s.sources.push_back(image);
            }
        }
        Status(lane,"Same-frame full image; actual capture queue fence completed");
    }
    for(auto& lane:s.lanes)for(unsigned slot=0;slot<Slots;++slot){
        std::shared_ptr<Batch> b;
        bool discarded=false;
        {std::lock_guard lock(s.lifecycle);b=lane.batches[slot];if(!b||!b->reset||
            (b->phase!=Phase::Published&&b->phase!=Phase::Discarded))continue;
            discarded=b->phase==Phase::Discarded;b->phase=Phase::Cleaning;}
        // Actual queue fence completion AND this generation's successful Reset.
        // Under the mandatory pre-Execute contract, a successful Reset without
        // pre is instead proof this recording was discarded, never submitted.
        if(discarded)b->outputState=b->recordStartOutputState;
        s.active[static_cast<unsigned>(&lane-s.lanes.data())*Slots+slot].store(nullptr,std::memory_order_release);
        b->list.Reset();b->queue.Reset();for(auto& input:b->inputs)input.Reset();
        {std::lock_guard lock(s.lifecycle);b->reset=b->pre=b->post=false;b->phase=Phase::Ready;}
    }
    return pending;
}
bool PrepareOne(State& s){
    for(auto& lane:s.lanes)for(unsigned slot=0;slot<Slots;++slot){
        if(!OfflineWanted(static_cast<Stream>(&lane-s.lanes.data())))continue;
        if(lane.retryAfter>GuideDiagnostics::Now())continue;
        ComPtr<ID3D12Device> device;std::shared_ptr<Batch> old;
        {std::lock_guard lock(s.lifecycle);device=lane.requestedDevice;auto& b=lane.batches[slot];
            if(!device||(b&&(b->phase!=Phase::Ready||b->pipeline->device.Get()==device.Get())))continue;old=std::move(b);}
        auto prepared=Prepare(s,device.Get());const bool ok=bool(prepared);
        {std::lock_guard lock(s.lifecycle);if(!lane.batches[slot]&&lane.requestedDevice.Get()==device.Get())lane.batches[slot].swap(prepared);}
        if(!ok){lane.retryAfter=GuideDiagnostics::Now()+1000;Status(lane,"Diagnostic async preparation failed or device cache full");}return true;
    }
    return false;
}
DWORD WINAPI Worker(void* context){
    auto& s=Storage();const auto module=static_cast<HMODULE>(context);
    for(;;){
        bool pending=false;
        try{pending=Poll(s);pending|=PreviewDetail::Poll();pending|=PrepareOne(s);pending|=PreviewDetail::PrepareOne();}catch(...){
            std::lock_guard lock(s.lifecycle);
            for(auto& lane:s.lanes)for(auto& b:lane.batches)if(b&&b->phase!=Phase::Ready&&b->phase!=Phase::Published&&b->phase!=Phase::Recording){
                b->phase=Phase::Quarantined;Status(lane,"Diagnostic worker failed; uncertain resources retained");}
        }
        // Reset-only / pre/post-only waiting is event driven, not pixel polling.
        // The retained module and sleeping worker also prevent an unload race.
        if(s.wake)WaitForSingleObject(s.wake,pending?2:INFINITE);else break;
    }
    if(module)FreeLibraryAndExitThread(module,0);return 0;
}
void StartWorker(State& s){
    bool expected=false;if(!s.running.compare_exchange_strong(expected,true,std::memory_order_acq_rel)){Wake(s);return;}
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Worker),&module)){
        s.running.store(false,std::memory_order_release);return;}
    HANDLE thread=CreateThread(nullptr,0,Worker,module,0,nullptr);
    if(thread)CloseHandle(thread);else{FreeLibrary(module);s.running.store(false,std::memory_order_release);}Wake(s);
}
bool Matches(ID3D12CommandList* list,UINT count,ID3D12CommandList* const* lists){
    for(UINT i=0;i<count;++i)if(list==lists[i])return true;return false;
}
bool ActiveMatches(State& s,UINT count,ID3D12CommandList* const* lists){
    if(!lists)return false;for(auto& key:s.active){auto* p=key.load(std::memory_order_acquire);if(p&&Matches(p,count,lists))return true;}return false;
}
}
void Request(bool enabled,bool calibration) noexcept {
    try{auto& s=Storage();s.calibration.store(calibration,std::memory_order_release);
        s.streams.store(enabled?((1u<<static_cast<unsigned>(Stream::Count))-1u):0u,std::memory_order_release);
        s.demand.store(enabled?GuideDiagnostics::Now()+(calibration?120000:1000):0,std::memory_order_release);
        if(enabled)StartWorker(s);else Wake(s);
    }catch(...){}
}
bool Wanted() noexcept {for(unsigned n=0;n<static_cast<unsigned>(Stream::Count);++n)if(Wanted(static_cast<Stream>(n)))return true;return false;}
void Request(Stream stream,bool enabled,bool calibration) noexcept {
    try{auto& s=Storage();const auto index=static_cast<unsigned>(stream);
        unsigned mask=enabled&&index<static_cast<unsigned>(Stream::Count)?1u<<index:0u;
        if(calibration)mask|=1u<<static_cast<unsigned>(Stream::Source);
        s.calibration.store(calibration,std::memory_order_release);
        s.streams.store(mask,std::memory_order_release);
        s.demand.store(mask?GuideDiagnostics::Now()+(calibration?120000:1000):0,std::memory_order_release);
        if(mask)StartWorker(s);else Wake(s);
    }catch(...){}
}
bool Wanted(Stream stream) noexcept {
    return PreviewWanted(stream)||OfflineWanted(stream);
}
bool OfflineWanted(Stream stream) noexcept {
    const auto index=static_cast<unsigned>(stream);
    if(index>=static_cast<unsigned>(Stream::Count))return false;auto& s=Storage();auto& lane=s.lanes[index];const auto now=GuideDiagnostics::Now();
    return (stream==Stream::History&&s.historyMeasurementDeadline.load(std::memory_order_acquire)>now)||
        (lane.pairDeadline.load(std::memory_order_acquire)>now&&lane.pairCaptured.load(std::memory_order_acquire)<2)||
        (s.demand.load(std::memory_order_acquire)>now&&(s.streams.load(std::memory_order_acquire)&(1u<<index))!=0);
}
bool OfflineSampleDue(Stream stream) noexcept {
    if(!OfflineWanted(stream))return false;
    auto& s=Storage();auto& lane=s.lanes[static_cast<unsigned>(stream)];
    const auto now=GuideDiagnostics::Now();
    const bool continuous=(lane.pairDeadline.load(std::memory_order_acquire)>now&&
                           lane.pairCaptured.load(std::memory_order_acquire)<2)||
        (stream==Stream::Source&&s.calibration.load(std::memory_order_acquire));
    return continuous||now-lane.last.load(std::memory_order_acquire)>=500;
}
bool Record(Stream stream,ID3D12GraphicsCommandList* list,const Inputs& in,const GuideDiagnostics::Snapshot& stamp) noexcept {
    SampleToken token;const bool preview=TryBeginSample(stream,token)&&TryRecord(token,list,in,stamp);
    const bool offline=RecordOffline(stream,list,in,stamp);return preview||offline;
}
bool RecordOffline(Stream stream,ID3D12GraphicsCommandList* list,const Inputs& in,const GuideDiagnostics::Snapshot& stamp) noexcept {
    if(!OfflineSampleDue(stream)||!list||!in.independentRecording||
       list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
    try{
        auto& s=Storage();auto& lane=s.lanes[static_cast<unsigned>(stream)];
        auto cr=in.colourRect,dr=in.depthRect,mr=in.motionRect,ar=in.contributionRect;
        if((in.colour&&!Region(in.colour,cr))||(in.depth&&!Region(in.depth,dr))||(in.motion&&!Region(in.motion,mr))||
           (!in.colour&&!in.depth&&!in.motion)||(in.compareFreshAnswer&&(!in.colour||!in.contribution))){
            Status(lane,"Unsupported diagnostic region/view or missing comparison RGB");return false;}
        if(!in.colour)cr=in.depth?dr:mr;
        if(in.compareFreshAnswer&&!ar.width&&!ar.height)ar=cr;
        if(in.contribution&&!Region(in.contribution,ar)){Status(lane,"Invalid diagnostic answer region");return false;}
        std::array<ID3D12Resource*,4> resources{in.colour,in.depth,in.motion,in.contribution};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)if(resources[i]&&resources[i]==resources[j]){
            Status(lane,"Diagnostic inputs alias different semantics");return false;}
        ComPtr<ID3D12Device> device;if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return false;
        std::array<DXGI_FORMAT,4> formats{};
        for(unsigned i=0;i<4;++i){
            if(!resources[i]){formats[i]=i==2?DXGI_FORMAT_R32G32_FLOAT:(i==0||i==3)?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R32_FLOAT;continue;}
            ComPtr<ID3D12Device> owner;if(FAILED(resources[i]->GetDevice(IID_PPV_ARGS(&owner)))||owner.Get()!=device.Get())return false;
            formats[i]=ViewFormat(resources[i]->GetDesc().Format,i);
            if(formats[i]==DXGI_FORMAT_UNKNOWN||(in.compareFreshAnswer&&(i==0||i==3)&&!Rgb(formats[i]))){
                Status(lane,"Unsupported diagnostic texture encoding");return false;}
        }
        if(stamp.reason.size()>MaxText||stamp.colourChoice.size()>MaxText||stamp.resetReason.size()>MaxText){
            Status(lane,"Diagnostic metadata exceeds prepared text capacity");return false;}
        const float ratio=(std::min)(float(MaxWidth)/float(cr.width),float(MaxHeight)/float(cr.height));
        const unsigned w=(std::max)(1u,static_cast<unsigned>(std::round(cr.width*(std::min)(ratio,1.0f))));
        const unsigned h=(std::max)(1u,static_cast<unsigned>(std::round(cr.height*(std::min)(ratio,1.0f))));
        const auto now=GuideDiagnostics::Now();std::shared_ptr<Batch> b;ComPtr<ID3D12Device> previousRequest;
        {
            std::unique_lock lock(s.lifecycle,std::try_to_lock);if(!lock.owns_lock()){
                Status(lane,"Diagnostic lifecycle busy; this recording skipped");return false;}
            const bool pair=lane.pairDeadline.load()>now&&lane.pairCaptured.load()<2;
            const bool continuous=pair||(stream==Stream::Source&&s.calibration.load(std::memory_order_acquire));
            if(!continuous&&now-lane.last<500)return false;
            if(pair){const Key next{stamp.source,stamp.feature,stamp.configuration,stamp.owner,stamp.tick,0};
                if(!stamp.source||!stamp.epoch||!stamp.feature||!stamp.configuration||!stamp.owner)return false;
                if(lane.pairCaptured.load()&&(!SameDomain(lane.pairFirst,next)||stamp.epoch!=lane.pairEpoch||stamp.source!=lane.pairFirst.source+1)){
                    if(stamp.source!=lane.pairFirst.source){lane.pairDeadline.store(0);Status(lane,"Offline pair stopped: non-adjacent source or domain changed");}return false;}}
            for(auto& candidate:lane.batches)if(candidate&&candidate->phase==Phase::Ready&&candidate->pipeline->device.Get()==device.Get()){
                b=candidate;break;}
            if(!b){previousRequest.Swap(lane.requestedDevice);lane.requestedDevice=device;
                Status(lane,"Diagnostic slots busy or preparing asynchronously (no wait)");
            }else{
                b->phase=Phase::Recording;b->key={stamp.source,stamp.feature,stamp.configuration,stamp.owner,stamp.tick,++s.serial};
                b->pairId=pair?lane.pairId.load():0;
                b->pairIndex=pair?lane.pairCaptured.load():0;
                if(pair){if(lane.pairCaptured.load()==0){lane.pairFirst=b->key;lane.pairEpoch=stamp.epoch;}lane.pairCaptured.fetch_add(1);}
                uint64_t generation=0;
                for(auto& otherLane:s.lanes)for(auto& other:otherLane.batches)if(other&&other!=b&&
                    other->phase!=Phase::Ready&&other->phase!=Phase::Recording&&other->phase!=Phase::Discarded&&other->phase!=Phase::Cleaning&&
                    other->phase!=Phase::Quarantined&&!other->reset&&other->list.Get()==list)generation=other->generation;
                b->generation=generation?generation:++s.generation;
                b->list=list; // ready kits contain no old list reference
                for(unsigned n=0;n<Slots;++n)if(lane.batches[n]==b)
                    s.active[static_cast<unsigned>(stream)*Slots+n].store(static_cast<ID3D12CommandList*>(list),std::memory_order_release);
            }
        }
        if(!b){Wake(s);return false;}
        // Prepared GPU storage, pipeline, heap, fence and text capacity make
        // this only bounded CPU descriptor writing and command emission.
        for(unsigned i=0;i<4;++i)b->inputs[i]=resources[i];
        auto& im=b->image;im.stamp=stamp;im.valid=false;im.completedTick=0;im.stream=stream;im.width=w;im.height=h;
        im.motionToUvX=in.motionToUvX;im.motionToUvY=in.motionToUvY;im.colourPresent=in.colour!=nullptr;
        im.depthPresent=in.depth!=nullptr;im.motionPresent=in.motion!=nullptr;im.contributionPresent=in.contribution!=nullptr;
        b->compare=in.compareFreshAnswer;++b->value;
        const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto handle=b->heap->GetCPUDescriptorHandleForHeapStart();
        for(unsigned i=0;i<4;++i){D3D12_SHADER_RESOURCE_VIEW_DESC vd{};vd.Format=formats[i];vd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            vd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;vd.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(resources[i],&vd,handle);handle.ptr+=stride;}
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=w*h*2;ud.Buffer.StructureByteStride=16;
        device->CreateUnorderedAccessView(b->output.Get(),nullptr,&ud,handle);
        const std::array<D3D12_RESOURCE_STATES,4> states{in.colourState,in.depthState,in.motionState,in.contributionState};
        for(unsigned i=0;i<4;++i)Barrier(list,resources[i],states[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        b->recordStartOutputState=b->outputState;
        Barrier(list,b->output.Get(),b->outputState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Constants c{{cr.x,cr.y,cr.width,cr.height},{dr.x,dr.y,dr.width,dr.height},{mr.x,mr.y,mr.width,mr.height},{ar.x,ar.y,ar.width,ar.height},w,h,
            (in.depth?1u:0u)|(in.motion?2u:0u)|(in.contribution?4u:0u)|(in.colour?8u:0u)|(in.compareFreshAnswer?16u:0u),0};
        ID3D12DescriptorHeap* heaps[]{b->heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(b->pipeline->root.Get());
        list->SetPipelineState(b->pipeline->pso.Get());list->SetComputeRootDescriptorTable(0,b->heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRoot32BitConstants(1,20,&c,0);list->Dispatch((w+7)/8,(h+7)/8,1);
        Barrier(list,b->output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(b->readback.Get(),0,b->output.Get(),0,UINT64(w)*h*SampleBytes);
        for(unsigned i=0;i<4;++i)Barrier(list,resources[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,states[i]);
        b->outputState=D3D12_RESOURCE_STATE_COPY_SOURCE;
        {std::lock_guard lock(s.lifecycle);b->phase=Phase::Recorded;lane.last=now;}
        Status(lane,"Diagnostic recorded; awaiting exact pre-Execute receipt");Wake(s);return true;
    }catch(...){return false;}
}
ExecutionReceipt Executing(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) noexcept {
    ExecutionReceipt receipt;
    if(!queue||!lists)return receipt;try{auto& s=Storage();
        if(PreviewDetail::Executing(queue,count,lists,receipt))Wake(s);
        if(!ActiveMatches(s,count,lists))return receipt;
        {std::lock_guard lock(s.lifecycle);for(unsigned n=0;n<s.lanes.size();++n){auto& lane=s.lanes[n];
            for(unsigned j=0;j<Slots;++j){auto& b=lane.batches[j];
                if(!b||b->phase==Phase::Ready||b->phase==Phase::Recording||b->phase==Phase::Cleaning||b->reset||
                   b->phase==Phase::Quarantined||!Matches(b->list.Get(),count,lists))continue;
                if(b->pre&&!b->post&&b->queue.Get()==queue){receipt.offline[n*Slots+j]=b->generation;continue;}
                if(b->pre||b->post||b->phase!=Phase::Recorded){b->phase=Phase::Quarantined;
                    Status(lane,"Diagnostic recording replay or queue mismatch; resources retained");continue;}
                b->pre=true;b->queue=queue;b->phase=Phase::Executing;receipt.offline[n*Slots+j]=b->generation;
            }
        }}Wake(s);
    }catch(...){}return receipt;
}
void Submitted(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,const ExecutionReceipt& receipt) noexcept {
    if(!queue||!lists)return;try{auto& s=Storage();
        if(PreviewDetail::Submitted(queue,count,lists,receipt))Wake(s);
        if(!ActiveMatches(s,count,lists))return;
        {std::lock_guard lock(s.lifecycle);for(unsigned n=0;n<s.lanes.size();++n){auto& lane=s.lanes[n];
            for(unsigned j=0;j<Slots;++j){const auto generation=receipt.offline[n*Slots+j];auto& b=lane.batches[j];
                if(!generation||!b||b->generation!=generation||!Matches(b->list.Get(),count,lists)||
                   b->phase==Phase::Quarantined||b->phase==Phase::Ready||b->phase==Phase::Cleaning)continue;
                if(!b->pre||b->queue.Get()!=queue){b->phase=Phase::Quarantined;
                    Status(lane,"Diagnostic receipt queue mismatch; resources retained");continue;}
                if(b->post)continue; // nested hook acknowledgement of the same Execute
                b->post=true;b->phase=Phase::NeedsSignal;Status(lane,"Diagnostic actually submitted; awaiting exact queue fence");
            }
        }}Wake(s);
    }catch(...){}
}
void Reset(ID3D12GraphicsCommandList* list) noexcept {
    if(!list)return;try{auto& s=Storage();
        if(PreviewDetail::Reset(list))Wake(s);ID3D12CommandList* base=list;
        if(!ActiveMatches(s,1,&base))return;
        {std::lock_guard lock(s.lifecycle);for(auto& lane:s.lanes)for(auto& b:lane.batches){
            if(!b||b->phase==Phase::Ready||b->phase==Phase::Recording||b->phase==Phase::Cleaning||
               b->phase==Phase::Quarantined||b->reset||b->list.Get()!=list)continue;
            b->reset=true;
            if(!b->pre){b->phase=Phase::Discarded;
                Status(lane,"Successful Reset discarded an unsubmitted diagnostic recording");}
        }}Wake(s);
    }catch(...){}
}
bool Read(Stream stream,Image& image,std::string& status) noexcept {
    if(static_cast<unsigned>(stream)>=static_cast<unsigned>(Stream::Count))return false;
    try{auto& s=Storage();auto& lane=s.lanes[static_cast<unsigned>(stream)];std::shared_ptr<const Image> latest;
        {std::unique_lock lock(s.publication,std::try_to_lock);if(!lock.owns_lock()){status="Diagnostic image publication busy";return false;}latest=lane.latest;}
        status=lane.status.load(std::memory_order_acquire);if(!latest)return false;image=*latest;return image.valid;
    }catch(...){return false;}
}
void RequestHistoryMeasurement(bool enabled) noexcept {
    try {
        auto& s=Storage();
        if(!enabled){s.historyMeasurementDeadline.store(0,std::memory_order_release);return;}
        const auto now=GuideDiagnostics::Now();
        // Renew/wake at most twice a second while the panel is drawn. No menu
        // frame allocates pixels or wakes an otherwise idle worker repeatedly.
        if(s.historyMeasurementDeadline.load(std::memory_order_acquire)>now+500)return;
        s.historyMeasurementDeadline.store(now+1000,std::memory_order_release);
        StartWorker(s);
    } catch(...) {}
}
bool ReadHistoryMeasurement(ContributionSample& out) noexcept {
    try {
        auto& s=Storage();std::unique_lock lock(s.publication,std::try_to_lock);
        if(!lock.owns_lock())return false;
        const auto& image=s.lanes[static_cast<unsigned>(Stream::History)].latest;
        if(!image||!image->valid)return false;
        const auto& stamp=image->stamp;
        out={stamp.source,stamp.owner,stamp.feature,stamp.configuration,stamp.tick,image->completedTick,
             stamp.contribution,stamp.coverage,stamp.contributionKnown};
        return true;
    } catch(...) {return false;}
}
bool RequestPair(Stream stream) noexcept {
    const auto n=static_cast<unsigned>(stream);if(n>=static_cast<unsigned>(Stream::Count))return false;
    try{auto& s=Storage();std::unique_lock life(s.lifecycle,std::try_to_lock);if(!life.owns_lock())return false;
        std::unique_lock pub(s.publication,std::try_to_lock);if(!pub.owns_lock())return false;auto& lane=s.lanes[n];
        if(lane.pairDeadline.load()>GuideDiagnostics::Now()&&lane.pairCaptured.load()<2)return false;
        ++lane.pairId;lane.pairCaptured.store(0);lane.pairFirst={};lane.pairEpoch=0;lane.pairImages={};
        lane.pairDeadline.store(GuideDiagnostics::Now()+15000,std::memory_order_release);
        Status(lane,"Explicit offline pair requested; at most two adjacent 160x100 proxy images");
        pub.unlock();life.unlock();StartWorker(s);return true;
    }catch(...){return false;}
}
bool ReadPair(Stream stream,std::array<Image,2>& out,std::string& status) noexcept {
    const auto n=static_cast<unsigned>(stream);if(n>=static_cast<unsigned>(Stream::Count))return false;
    try{auto& s=Storage();auto& lane=s.lanes[n];std::array<std::shared_ptr<const Image>,2> images;
        {std::unique_lock lock(s.publication,std::try_to_lock);if(!lock.owns_lock()){status="Offline pair publication busy";return false;}images=lane.pairImages;}
        status=lane.status.load(std::memory_order_acquire);if(!images[0]||!images[1]){
            if(lane.pairDeadline.load()<=GuideDiagnostics::Now())status="Offline pair stopped/expired; incomplete observations cannot be joined";return false;}
        const auto& a=images[0]->stamp;const auto& b=images[1]->stamp;
        if(!images[0]->valid||!images[1]->valid||a.epoch!=b.epoch||a.feature!=b.feature||a.configuration!=b.configuration||a.owner!=b.owner||
            a.source==UINT64_MAX||b.source!=a.source+1||images[0]->width!=images[1]->width||images[0]->height!=images[1]->height)return false;
        out={*images[0],*images[1]};status="Two immutable completed offline proxy images (not fullscreen display)";return true;
    }catch(...){return false;}
}
std::vector<Image> DrainPairs(Stream stream) noexcept {
    std::vector<Image> result;const auto n=static_cast<unsigned>(stream);if(n>=static_cast<unsigned>(Stream::Count))return result;
    try{auto& s=Storage();auto& lane=s.lanes[n];std::array<std::shared_ptr<const Image>,2> images;
        {std::unique_lock lock(s.publication,std::try_to_lock);if(!lock.owns_lock())return result;
            if(!lane.pairImages[0]||!lane.pairImages[1])return result;images=std::move(lane.pairImages);lane.pairImages={};lane.pairDeadline.store(0);}
        result.reserve(2);for(auto& image:images)result.push_back(*image);
    }catch(...){}return result;
}
std::vector<Image> DrainSourcePairs() noexcept {
    std::vector<Image> result;try{auto& s=Storage();std::array<std::shared_ptr<const Image>,8> images{};size_t count=0;
        {std::unique_lock lock(s.publication,std::try_to_lock);if(!lock.owns_lock())return result;
            while(!s.sources.empty()){images[count++]=std::move(s.sources.front());s.sources.pop_front();}}
        result.reserve(count);for(size_t i=0;i<count;++i)result.push_back(*images[i]);
    }catch(...){}return result;
}
} // namespace GuideCapture

#include "GuidePreview.inl"
