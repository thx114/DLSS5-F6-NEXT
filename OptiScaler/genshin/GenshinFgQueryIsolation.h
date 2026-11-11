// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// FG-only isolated compute transport; guide admission remains with its caller.
// A closed, newly created D3D11 device owns every Update/Wait/Dispatch/Signal.
// No game immediate/deferred context is obtained, accepted, or submitted here.
// This is NOT a query-history/native/semantic certificate. In particular GPU
// elapsed time and device-dependent/global counters are NOT proved neutral.
// The typed packet adapter retains owner/device/queue/input authority; the
// caller retains camera/reference/picture gates. Opaque vendor-wide profiling
// and elapsed time are explicitly outside the pipeline-count isolation claim.
#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace GenshinFgQueryIsolation {
using Microsoft::WRL::ComPtr;
inline bool Same(IUnknown* a,IUnknown* b) noexcept {
    ComPtr<IUnknown> x,y;
    return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x.Get()==y.Get();
}
inline bool SameLuid(LUID a,LUID b) noexcept {return a.HighPart==b.HighPart&&a.LowPart==b.LowPart;}
// Texture extents follow the FL11 transport limit; retained bytes are bounded
// independently by Detail::Reserve. Ultrawide pictures may exceed 4096 pixels.
inline constexpr bool TextureExtent(uint32_t width,uint32_t height) noexcept {
    return width&&height&&width<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION&&
        height<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
inline bool Passed(UINT64 completed,UINT64 expected) noexcept {
    return expected&&expected!=UINT64_MAX&&completed!=UINT64_MAX&&completed>=expected;
}
enum class Failure { None, WrongThread, Device, Shader, Input, Resource, Capacity, Submission, Reader, ByteCapacity };

// These are transport references, never guide authority. Supply the genuine
// current/previous QueuedTransfer packets as lifetime roots (not scratch).
// All three resources must already be imported by the SAME producer device.
struct Inputs {
    std::array<ComPtr<ID3D12Resource>,3> resources; // current MV, current Z, previous Z
    std::array<ComPtr<ID3D12Fence>,2> submitted;
    std::array<UINT64,2> values{};
    std::array<std::shared_ptr<const void>,2> packets;
};
struct Control {
    uint32_t pictureWidth=0,pictureHeight=0,producerWidth=0,producerHeight=0;
    float currentX=0,currentY=0,previousX=0,previousY=0,clear=1;
    uint32_t reversed=0,pad0=0,pad1=0;
    float inverseWidth=0,inverseHeight=0,pad2=0,pad3=0;
    std::array<float,16> clipToWorld{},previousWorldToClip{};
};
static_assert(sizeof(Control)==192);

namespace Detail {
inline std::atomic<unsigned> lanes{0};
inline std::atomic<UINT64> bytes{0};
inline constexpr UINT64 ByteLimit=192ull*1024*1024;
// Both existing consumers reserve this same physical budget. Under pressure,
// each owner may release only its exclusive, positively completed idle jobs.
// Thread-local registration never visits a foreign immediate context. The
// cursor is updated by unlink, including destruction during a trim callback.
class IdleCache final {
    void* owner_;
    void (*trim_)(void*) noexcept;
    IdleCache *previous_=nullptr,*next_=nullptr;
    inline static thread_local IdleCache *first_=nullptr,*cursor_=nullptr;
    inline static thread_local bool trimming_=false;
public:
    IdleCache(void* owner,void (*trim)(void*) noexcept) noexcept:owner_(owner),trim_(trim) {
        next_=first_;if(next_)next_->previous_=this;first_=this;
    }
    IdleCache(const IdleCache&)=delete;IdleCache& operator=(const IdleCache&)=delete;
    ~IdleCache() {
        if(cursor_==this)cursor_=next_;
        if(previous_)previous_->next_=next_;else first_=next_;
        if(next_)next_->previous_=previous_;
    }
    static void Trim() noexcept {
        if(trimming_)return;
        trimming_=true;cursor_=first_;
        while(cursor_){auto* cache=cursor_;cursor_=cache->next_;cache->trim_(cache->owner_);}
        trimming_=false;
    }
};
inline bool Reserve(UINT64 amount) noexcept {
    if(amount>ByteLimit)return false;
    for(unsigned attempt=0;attempt<2;++attempt){
        auto used=bytes.load();
        while(used<=ByteLimit-amount){
            if(bytes.compare_exchange_weak(used,used+amount))return true;
        }
        if(!attempt)IdleCache::Trim();
    }
    return false; // Live/unfinished/quarantined owners still consume the bound.
}
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){if(value)CloseHandle(value);}
};
inline void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=resource;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=before;b.Transition.StateAfter=after;list->ResourceBarrier(1,&b);
}
}
class Lane;
class Job final {
    friend class Lane;
    DWORD thread_=GetCurrentThreadId();
    UINT64 bytes_=0;
    Inputs inputs_;
    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D12Device> producer_,reader_;
    std::array<ComPtr<ID3D11Texture2D>,3> imported_;
    std::array<ComPtr<ID3D11ShaderResourceView>,3> srvs_;
    std::array<ComPtr<ID3D11Fence>,2> waits_;
    std::array<ComPtr<ID3D11Texture2D>,2> outputs_;
    std::array<ComPtr<ID3D11UnorderedAccessView>,2> uavs_;
    std::array<Detail::Handle,2> outputHandles_;
    Detail::Handle fenceHandle_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Fence> ready_;
    std::array<ComPtr<ID3D12Resource>,2> readOutputs_;
    ComPtr<ID3D12Fence> readReady_,readDone_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    UINT64 readyValue_=0,readValue_=0;
    uint32_t pictureWidth_=0,pictureHeight_=0,producerWidth_=0,producerHeight_=0;
    bool issued_=false,signalled_=false,poisoned_=false,readerStarted_=false,readerEnded_=false,retired_=false;
    Job()=default;
    bool Alive()const noexcept {
        return device_&&producer_&&SUCCEEDED(device_->GetDeviceRemovedReason())&&
            SUCCEEDED(producer_->GetDeviceRemovedReason())&&(!reader_||SUCCEEDED(reader_->GetDeviceRemovedReason()));
    }
public:
    Job(const Job&)=delete;Job& operator=(const Job&)=delete;
    ~Job(){if(bytes_)Detail::bytes.fetch_sub(bytes_);}
    bool Complete()const noexcept {
        if(thread_!=GetCurrentThreadId()||!Alive()||poisoned_)return false;
        if(!issued_)return true;
        if(!signalled_||!ready_||!Passed(ready_->GetCompletedValue(),readyValue_))return false;
        return !readerStarted_||(readerEnded_&&readDone_&&Passed(readDone_->GetCompletedValue(),readValue_));
    }
    bool ProducerComplete()const noexcept {
        return thread_==GetCurrentThreadId()&&Alive()&&!poisoned_&&signalled_&&ready_&&Passed(ready_->GetCompletedValue(),readyValue_);
    }
    // CPU-only import. No queue operation occurs until BeginRead installs the
    // reader debt; an import abandoned before BeginRead has no GPU lifetime debt.
    bool ImportReady(ID3D12Device* d,ID3D12CommandQueue* queue) noexcept {
        if(GetCurrentThreadId()!=thread_||!Alive()||poisoned_||!signalled_||!d||!queue||
           queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        ComPtr<ID3D12Device> q;
        if(FAILED(d->GetDeviceRemovedReason())||FAILED(queue->GetDevice(IID_PPV_ARGS(&q)))||
           !Same(d,q.Get())||!SameLuid(d->GetAdapterLuid(),producer_->GetAdapterLuid()))return false;
        if(reader_&&Same(reader_.Get(),d)){
            if(queue_&&!Same(queue_.Get(),queue))return false;
            queue_=queue;return true;
        }
        // A retired generation may keep its CPU imports. Rebind a different
        // reader device only before this generation has acquired a queue/debt.
        if(reader_&&(queue_||readerStarted_))return false;
        std::array<ComPtr<ID3D12Resource>,2> opened;ComPtr<ID3D12Fence> fence;
        for(size_t i=0;i<2;++i)
            if(FAILED(d->OpenSharedHandle(outputHandles_[i].value,IID_PPV_ARGS(&opened[i]))))return false;
        if(FAILED(d->OpenSharedHandle(fenceHandle_.value,IID_PPV_ARGS(&fence))))return false;
        reader_=d;queue_=queue;readOutputs_=std::move(opened);readReady_=fence;return true;
    }
    // Before the first reader command, record a real reader dependency and
    // lifetime debt. No game D3D11 Wait or context command is ever issued here.
    bool BeginRead(ID3D12GraphicsCommandList* list,ID3D12CommandQueue* queue) noexcept {
        if(GetCurrentThreadId()!=thread_||readerStarted_||!list||
           list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        ComPtr<ID3D12Device> d;
        if(FAILED(list->GetDevice(IID_PPV_ARGS(&d)))||!ImportReady(d.Get(),queue))return false;
        list_=list;readerStarted_=true; // Failed/abandoned readers cannot retire.
        if(FAILED(queue_->Wait(readReady_.Get(),readyValue_))){poisoned_=true;return false;}
        for(auto& r:readOutputs_)Detail::Barrier(list,r.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }
    ID3D12Resource* Motion()const noexcept {return thread_==GetCurrentThreadId()&&reader_&&queue_&&!poisoned_&&!retired_?readOutputs_[0].Get():nullptr;}
    ID3D12Resource* Depth()const noexcept {return thread_==GetCurrentThreadId()&&reader_&&queue_&&!poisoned_&&!retired_?readOutputs_[1].Get():nullptr;}
    bool EndRead(ID3D12GraphicsCommandList* list) noexcept {
        if(GetCurrentThreadId()!=thread_||poisoned_||!readerStarted_||readerEnded_||!Same(list,list_.Get()))return false;
        for(auto& r:readOutputs_)Detail::Barrier(list,r.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        readerEnded_=true;return true;
    }
    // Invoke only after actual reader Execute + queue Signal. Completion is
    // still observed from the physical fence, never from this call's boolean.
    bool BindCompletion(ID3D12Fence* fence,UINT64 value) noexcept {
        if(GetCurrentThreadId()!=thread_||poisoned_||!readerEnded_||readDone_||!fence||!value||value==UINT64_MAX)return false;
        ComPtr<ID3D12Device> d;
        if(FAILED(fence->GetDevice(IID_PPV_ARGS(&d)))||!Same(d.Get(),reader_.Get()))return false;
        readDone_=fence;readValue_=value;return true;
    }
};

class Lane final {
    DWORD thread_=GetCurrentThreadId();
    ComPtr<ID3D11Device> game_; // Identity only: never GetImmediateContext.
    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11DeviceContext4> context_; // Never escapes this class.
    ComPtr<ID3D12Device> producer_;
    ComPtr<ID3D11ComputeShader> shader_;
    // Active, quarantined and idle retired resources share the SAME three
    // slots/physical byte reservation; caching cannot grow a second live ring.
    std::array<std::shared_ptr<Job>,3> jobs_;
    Failure failure_=Failure::None;
    bool poisoned_=false,counted_=false;
    Lane* retiredNext_=nullptr;
    // Lane destruction is already owner-thread-only in ReapRetired.
    Detail::IdleCache idleCache_{this,[](void* owner) noexcept {
        auto& lane=*static_cast<Lane*>(owner);
        if(!lane.poisoned_)lane.CollectCompleted(true);
    }};
    Lane()=default;
    struct Retired {std::mutex mutex;Lane* head=nullptr;};
    static Retired& Quarantine(){static auto* r=new Retired;return *r;}
    bool Empty()const noexcept {for(const auto& j:jobs_)if(j)return false;return true;}
    ~Lane(){if(counted_)Detail::lanes.fetch_sub(1);}
    // A vendor Frame/preview retains its Lease, which in turn retains Job.
    // Exclusivity therefore also excludes those consumers; it is NEVER a
    // replacement for the producer and actual reader completion receipts.
    void CollectCompleted(bool release) noexcept {
        if(thread_!=GetCurrentThreadId())return;
        for(auto& j:jobs_)if(j&&j.use_count()==1&&j->Complete()){
            if(release){j.reset();continue;}
            // Drop genuine upstream lifetime roots only after all holders and
            // GPU debt have retired. COM identities/views stay as idle cache;
            // they cannot authorize reading a subsequent packet's contents.
            j->inputs_.packets={};j->list_.Reset();j->retired_=true;
        }
    }
    bool Reusable(const Job& j,const Control& c)const noexcept {
        return j.retired_&&j.Complete()&&j.readyValue_<UINT64_MAX-1&&
            Same(j.device_.Get(),device_.Get())&&Same(j.producer_.Get(),producer_.Get())&&
            j.pictureWidth_==c.pictureWidth&&j.pictureHeight_==c.pictureHeight&&
            j.producerWidth_==c.producerWidth&&j.producerHeight_==c.producerHeight;
    }
    static bool SameInputs(const Job& j,const Inputs& in) noexcept {
        for(size_t i=0;i<3;++i)if(!Same(j.inputs_.resources[i].Get(),in.resources[i].Get()))return false;
        for(size_t i=0;i<2;++i)if(!Same(j.inputs_.submitted[i].Get(),in.submitted[i].Get()))return false;
        return true;
    }
    bool Validate(const Inputs& in,const Control& c)const noexcept {
        if(!TextureExtent(c.pictureWidth,c.pictureHeight)||
           !TextureExtent(c.producerWidth,c.producerHeight)||
           c.reversed>1||c.clear!=(c.reversed?0.f:1.f)||
           c.inverseWidth!=1.f/c.pictureWidth||c.inverseHeight!=1.f/c.pictureHeight||
           !in.packets[0]||!in.packets[1])return false;
        for(float f:{c.currentX,c.currentY,c.previousX,c.previousY}){
            uint32_t bits=0;std::memcpy(&bits,&f,4);if((bits&0x7f800000u)==0x7f800000u)return false;
        }
        for(const auto* matrix:{&c.clipToWorld,&c.previousWorldToClip})for(float f:*matrix){
            uint32_t bits=0;std::memcpy(&bits,&f,4);if((bits&0x7f800000u)==0x7f800000u)return false;
        }
        for(size_t i=0;i<3;++i){
            if(!in.resources[i])return false;
            ComPtr<ID3D12Device> d;
            if(FAILED(in.resources[i]->GetDevice(IID_PPV_ARGS(&d)))||!Same(d.Get(),producer_.Get()))return false;
            const auto desc=in.resources[i]->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=c.producerWidth||
               desc.Height!=c.producerHeight||desc.DepthOrArraySize!=1||desc.MipLevels!=1||
               desc.SampleDesc.Count!=1||desc.SampleDesc.Quality||
               desc.Format!=(i?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16_FLOAT)||
               (desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||
               !(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))return false;
            for(size_t j=0;j<i;++j)if(Same(in.resources[i].Get(),in.resources[j].Get()))return false;
        }
        for(size_t i=0;i<2;++i){
            ComPtr<ID3D12Device> d;
            if(!in.submitted[i]||!in.values[i]||in.values[i]==UINT64_MAX||
               FAILED(in.submitted[i]->GetDevice(IID_PPV_ARGS(&d)))||!Same(d.Get(),producer_.Get())||
               in.submitted[i]->GetCompletedValue()==UINT64_MAX)return false;
        }
        return true;
    }
    // Bind transactionally while the slot is exclusively retired (or new).
    // Reuse each import/SRV only for the SAME live COM object. Changed input
    // resources/fences get new imports, never a cached pointer relabelled ready.
    bool BindInputs(Job& j,const Inputs& in) noexcept {
        auto imported=j.imported_;auto srvs=j.srvs_;auto waits=j.waits_;
        for(size_t i=0;i<3;++i){
            if(imported[i]&&srvs[i]&&Same(j.inputs_.resources[i].Get(),in.resources[i].Get()))continue;
            Detail::Handle h;
            ComPtr<ID3D11Texture2D> resource;ComPtr<ID3D11ShaderResourceView> view;
            if(FAILED(producer_->CreateSharedHandle(in.resources[i].Get(),nullptr,GENERIC_ALL,nullptr,&h.value))||
               FAILED(device_->OpenSharedResource1(h.value,IID_PPV_ARGS(&resource)))||
               FAILED(device_->CreateShaderResourceView(resource.Get(),nullptr,&view)))return false;
            imported[i]=std::move(resource);srvs[i]=std::move(view);
        }
        for(size_t i=0;i<2;++i){
            if(waits[i]&&Same(j.inputs_.submitted[i].Get(),in.submitted[i].Get()))continue;
            Detail::Handle h;
            ComPtr<ID3D11Fence> fence;
            if(FAILED(producer_->CreateSharedHandle(in.submitted[i].Get(),nullptr,GENERIC_ALL,nullptr,&h.value))||
               FAILED(device_->OpenSharedFence(h.value,IID_PPV_ARGS(&fence))))return false;
            waits[i]=std::move(fence);
        }
        j.imported_=std::move(imported);j.srvs_=std::move(srvs);j.waits_=std::move(waits);return true;
    }
    bool Resources(Job& j,const Control& c) noexcept {
        j.device_=device_;j.producer_=producer_;
        j.pictureWidth_=c.pictureWidth;j.pictureHeight_=c.pictureHeight;
        j.producerWidth_=c.producerWidth;j.producerHeight_=c.producerHeight;
        for(size_t i=0;i<2;++i){
            D3D11_TEXTURE2D_DESC d{};d.Width=c.pictureWidth;d.Height=c.pictureHeight;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
            d.Format=i?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
            if(FAILED(device_->CreateTexture2D(&d,nullptr,&j.outputs_[i]))||
               FAILED(device_->CreateUnorderedAccessView(j.outputs_[i].Get(),nullptr,&j.uavs_[i])))return false;
            ComPtr<IDXGIResource1> r;
            if(FAILED(j.outputs_[i].As(&r))||FAILED(r->CreateSharedHandle(nullptr,
                DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&j.outputHandles_[i].value)))return false;
        }
        D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Control);b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(device_->CreateBuffer(&b,nullptr,&j.constants_))&&
            SUCCEEDED(device_->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&j.ready_)))&&
            SUCCEEDED(j.ready_->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&j.fenceHandle_.value));
    }
public:
    struct Deleter {void operator()(Lane* lane)const noexcept {
        if(!lane)return;
        // Can be destroyed on a foreign thread: do not touch mutable lane/job
        // state there. ReapRetired is owner-thread-only for each retained lane.
        auto& r=Quarantine();std::lock_guard lock(r.mutex);
        lane->retiredNext_=r.head;r.head=lane;
    }};
    using Ptr=std::unique_ptr<Lane,Deleter>;
    // At most three live OR quarantined lanes globally, not three per retry.
    static Ptr Create(ID3D11Device* game,ID3D12Device* producer,const void* code,SIZE_T length) {
        ReapRetired();
        auto n=Detail::lanes.load();
        do {if(n>=3)return {}; }while(!Detail::lanes.compare_exchange_weak(n,n+1));
        Lane* raw=nullptr;
        try {raw=new Lane;}catch(...){Detail::lanes.fetch_sub(1);throw;}
        Ptr lane(raw);lane->counted_=true;
        if(!game||!producer||!code||!length||FAILED(game->GetDeviceRemovedReason())||FAILED(producer->GetDeviceRemovedReason()))return {};
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
        if(FAILED(game->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter))||
           FAILED(adapter->GetDesc(&desc))||!SameLuid(desc.AdapterLuid,producer->GetAdapterLuid()))return {};
        ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;D3D_FEATURE_LEVEL actual;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,2,D3D11_SDK_VERSION,&d,&actual,&c))||actual<D3D_FEATURE_LEVEL_11_0||Same(d.Get(),game)||
           FAILED(d.As(&lane->device_))||FAILED(c.As(&lane->context_)))return {};
        // The creation result must itself still identify the selected adapter.
        ComPtr<IDXGIDevice> privateDxgi;ComPtr<IDXGIAdapter> privateAdapter;DXGI_ADAPTER_DESC privateDesc{};
        if(FAILED(d.As(&privateDxgi))||FAILED(privateDxgi->GetAdapter(&privateAdapter))||
           FAILED(privateAdapter->GetDesc(&privateDesc))||!SameLuid(desc.AdapterLuid,privateDesc.AdapterLuid)||
           c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||
           FAILED(d->CreateComputeShader(code,length,nullptr,&lane->shader_)))return {};
        lane->game_=game;lane->producer_=producer;return lane;
    }
    Failure LastFailure()const noexcept {return failure_;}
    size_t Retained()const noexcept {size_t n=0;for(const auto& j:jobs_)if(j)++n;return n;}
    void Collect() noexcept {
        CollectCompleted(false);
    }
    static void ReapRetired() noexcept {
        auto& r=Quarantine();std::lock_guard lock(r.mutex);auto** at=&r.head;
        while(*at){auto* lane=*at;
            // Retiring a lane must drain its idle cache as well as live jobs;
            // otherwise cached slots would pin the global lane/byte budgets.
            if(lane->thread_==GetCurrentThreadId())lane->CollectCompleted(true);
            if(lane->thread_==GetCurrentThreadId()&&lane->Empty()){
                *at=lane->retiredNext_;delete lane;
            }else at=&lane->retiredNext_;
        }
    }
    std::shared_ptr<Job> Submit(Inputs in,const Control& c) {
        // Reject foreign-thread entry before touching any mutable lane state.
        if(thread_!=GetCurrentThreadId())return {};
        failure_=Failure::None;
        const auto fail=[&](Failure f)->std::shared_ptr<Job>{failure_=f;return {};};
        if(poisoned_||!device_||FAILED(device_->GetDeviceRemovedReason())||FAILED(game_->GetDeviceRemovedReason())||
           FAILED(producer_->GetDeviceRemovedReason()))return fail(Failure::Device);
        if(!Validate(in,c))return fail(Failure::Input);
        Collect();std::shared_ptr<Job>* slot=nullptr;std::shared_ptr<Job>* vacant=nullptr;
        std::shared_ptr<Job>* discardable=nullptr;
        for(auto& j:jobs_){
            if(!j){if(!vacant)vacant=&j;continue;}
            if(j.use_count()!=1||!j->retired_||!j->Complete())continue;
            if(!discardable)discardable=&j;
            if(Reusable(*j,c)&&(!slot||(!SameInputs(**slot,in)&&SameInputs(*j,in))))slot=&j;
        }
        std::shared_ptr<Job> job;
        if(slot)job=*slot;
        else {
            slot=vacant?vacant:discardable;
            if(!slot)return fail(Failure::Capacity);
            slot->reset(); // Only an empty or exclusively, positively retired slot.
            job=std::shared_ptr<Job>(new Job);
            const UINT64 bytes=UINT64(c.pictureWidth)*c.pictureHeight*12;
            bool reserved=Detail::Reserve(bytes);
            // Shape changes may leave idle incompatible slots consuming the
            // physical budget. Evict only completed sole-owned cache as needed.
            if(!reserved)for(auto& j:jobs_)if(j&&j.use_count()==1&&j->retired_&&j->Complete()){
                j.reset();if((reserved=Detail::Reserve(bytes)))break;
            }
            if(!reserved)return fail(Failure::ByteCapacity);
            job->bytes_=bytes;
            if(!Resources(*job,c))return fail(Failure::Resource);
        }
        if(!BindInputs(*job,in))return fail(Failure::Resource);
        // No old Lease/Frame/preview can observe this rearm: selection required
        // sole lane ownership AND positive completion, before taking job here.
        // Fence values never reset/repeat and UINT64_MAX is never issued.
        job->inputs_=std::move(in);++job->readyValue_;
        job->readDone_.Reset();job->readValue_=0;job->list_.Reset();job->queue_.Reset();
        job->readerStarted_=job->readerEnded_=job->signalled_=job->retired_=false;
        *slot=job;job->issued_=true; // Retention installed BEFORE the first GPU command.
        for(size_t i=0;i<2;++i)if(FAILED(context_->Wait(job->waits_[i].Get(),job->inputs_.values[i]))){
            job->poisoned_=poisoned_=true;context_->Flush();return fail(Failure::Submission);
        }
        context_->UpdateSubresource(job->constants_.Get(),0,nullptr,&c,0,0);
        ID3D11ShaderResourceView* srvs[]{job->srvs_[0].Get(),job->srvs_[1].Get(),job->srvs_[2].Get()};
        ID3D11UnorderedAccessView* uavs[]{job->uavs_[0].Get(),job->uavs_[1].Get()};auto* cb=job->constants_.Get();
        context_->CSSetShaderResources(0,3,srvs);context_->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        context_->CSSetConstantBuffers(0,1,&cb);context_->CSSetShader(shader_.Get(),nullptr,0);
        context_->Dispatch((c.pictureWidth+7)/8,(c.pictureHeight+7)/8,1);
        // Unbind only our closed context. Game predication/state is untouched.
        ID3D11ShaderResourceView* emptySrv[3]{};ID3D11UnorderedAccessView* emptyUav[2]{};cb=nullptr;
        context_->CSSetShaderResources(0,3,emptySrv);context_->CSSetUnorderedAccessViews(0,2,emptyUav,nullptr);
        context_->CSSetConstantBuffers(0,1,&cb);context_->CSSetShader(nullptr,nullptr,0);
        job->signalled_=SUCCEEDED(context_->Signal(job->ready_.Get(),job->readyValue_));context_->Flush();
        if(!job->signalled_){job->poisoned_=poisoned_=true;return fail(Failure::Submission);}
        return job;
    }
};
} // namespace GenshinFgQueryIsolation
