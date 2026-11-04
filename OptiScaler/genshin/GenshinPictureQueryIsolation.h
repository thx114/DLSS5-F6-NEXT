// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFgQueryIsolation.h"
#include "GenshinDx12Transport.h"
#include "GenshinOwnerIdentity.h"
#include "GenshinDepthPortableResources.h"
#include <functional>

namespace GenshinDepthMapping {class Capture;}
namespace GenshinFgQueryIsolation {
// A one-packet extension of the existing query-isolation transport domain.
// Only Capture may submit the existing picture mapper to its closed context.
// No previous packet, FG guide/donor, history UV or game-query certificate.
class PictureLane;
class PictureJob final {
    friend class PictureLane;
    DWORD thread_=GetCurrentThreadId();
    std::shared_ptr<const GenshinTransport::QueuedTransfer> packet_;
    std::shared_ptr<void> rasterRoots_;
    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11Device> game_;
    ComPtr<IUnknown> gameContext_;
    ComPtr<ID3D12Device> producer_;
    ComPtr<ID3D11Texture2D> input_,output_,gameOutput_;
    ComPtr<ID3D11ShaderResourceView> inputView_;
    ComPtr<ID3D11Fence> inputFence_,ready_,gameReady_;
    ComPtr<ID3D12Resource> boundInput_;
    ComPtr<ID3D12Fence> boundFence_;
    UINT64 readyValue_=0;
    UINT width_=0,height_=0;bool retired_=false;
    bool issued_=false,signalled_=false,poisoned_=false,ordered_=false;
    UINT64 bytes_=0;
    PictureJob()=default;
public:
    PictureJob(const PictureJob&)=delete;PictureJob& operator=(const PictureJob&)=delete;
    ~PictureJob(){if(bytes_)Detail::bytes.fetch_sub(bytes_);}
    bool Complete()const noexcept {
        if(thread_!=GetCurrentThreadId()||poisoned_||!device_||!producer_||!game_||
           FAILED(device_->GetDeviceRemovedReason())||FAILED(producer_->GetDeviceRemovedReason())||
           FAILED(game_->GetDeviceRemovedReason()))return false;
        return !issued_||(signalled_&&ready_&&Passed(ready_->GetCompletedValue(),readyValue_));
    }
    ID3D11Texture2D* Texture()const noexcept {
        return thread_==GetCurrentThreadId()&&signalled_&&!retired_&&!poisoned_&&device_&&game_&&producer_&&
            SUCCEEDED(device_->GetDeviceRemovedReason())&&SUCCEEDED(game_->GetDeviceRemovedReason())&&
            SUCCEEDED(producer_->GetDeviceRemovedReason())?gameOutput_.Get():nullptr;
    }
    // The actual NR copy remains on its existing context/reader-debt path.
    // This adds only the physical dependency for that copy, never a capture
    // copy/readback or a private Draw/Dispatch in an UnknownPast query domain.
    bool OrderRead(ID3D11DeviceContext* context)noexcept {
        if(thread_!=GetCurrentThreadId()||!context||!signalled_||retired_||poisoned_||!gameReady_||
           !Same(context,gameContext_.Get()))return false;
        if(ordered_)return true;
        ComPtr<ID3D11DeviceContext4> c;ComPtr<ID3D11Device> d;context->GetDevice(&d);
        if(!Same(d.Get(),game_.Get())||FAILED(context->QueryInterface(IID_PPV_ARGS(&c))))return false;
        if(FAILED(c->Wait(gameReady_.Get(),readyValue_))){poisoned_=true;return false;}
        ordered_=true;return true;
    }
};

class PictureLane final {
    friend class GenshinDepthMapping::Capture;
    DWORD thread_=GetCurrentThreadId();
    ComPtr<ID3D11Device> game_;
    ComPtr<IUnknown> gameContext_;
    ComPtr<ID3D12Device> producer_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11DeviceContext4> context_;
    std::array<std::shared_ptr<PictureJob>,3> jobs_;
    bool poisoned_=false,counted_=false;
    PictureLane* next_=nullptr;
    // Same owner-thread lifetime as the existing retired-lane reaper. Keep
    // published Node/reader roots and all uncertain GPU debt under pressure.
    Detail::IdleCache idleCache_{this,[](void* owner) noexcept {
        auto& lane=*static_cast<PictureLane*>(owner);
        if(!lane.poisoned_)lane.Collect(true);
    }};
    PictureLane()=default;
    ~PictureLane(){if(counted_)Detail::lanes.fetch_sub(1);}
    struct Retired {std::mutex mutex;PictureLane* first=nullptr;};
    static Retired& Quarantine(){static auto* r=new Retired;return *r;}
    void Collect(bool release=false)noexcept {
        if(thread_!=GetCurrentThreadId())return;
        // A published Node/PreparedDepthLease retains the job until its OWN
        // copy and model reader debts retire. Producer completion alone cannot
        // evict a published job. Failed/unknown completion stays quarantined.
        for(auto& j:jobs_)if(j&&j.use_count()==1&&j->Complete()){
            if(release){j.reset();continue;}
            j->packet_.reset();j->rasterRoots_.reset();j->retired_=true;
        }
    }
    bool Empty()const noexcept {for(const auto& j:jobs_)if(j)return false;return true;}
    bool Pending()noexcept {
        if(thread_!=GetCurrentThreadId())return true;Collect();
        for(const auto& j:jobs_)if(j&&(!j->Complete()||j.use_count()>1))return true;
        return false;
    }
    static void Reap()noexcept {
        auto& r=Quarantine();std::lock_guard lock(r.mutex);auto** at=&r.first;
        while(*at){auto* lane=*at;lane->Collect(true);
            if(lane->thread_==GetCurrentThreadId()&&lane->Empty()){*at=lane->next_;delete lane;}
            else at=&lane->next_;
        }
    }
    static bool Packet(ID3D11DeviceContext* game,const GenshinGuides::Owner& owner,
        ID3D11Resource* source,const std::shared_ptr<const GenshinTransport::QueuedTransfer>& packet,
        ComPtr<ID3D12Device>& producer)noexcept {
        if(!game||!source||!packet)return false;
        const auto& p=packet->Get();GenshinGuides::Owner canonical{};
        if(!GenshinGuides::OwnerIdentity::Context(game).ToCanonical(p.owner,canonical)||
           !GenshinGuides::SameOwner(owner,canonical)||!owner.generation||!owner.sourceFrame||
           !owner.deviceEpoch||!owner.producerEpoch||!owner.configEpoch||!owner.historyEpoch||!owner.instance||
           !p.surfaceLifetime||!p.sources[1]||!Same(p.sources[1].Get(),source)||!p.imported[1]||
           !packet->Prepared(1)||!p.queue||!p.list||!p.allocator||!p.submitted||
           !p.submittedValue||p.submittedValue==UINT64_MAX||p.submitted->GetCompletedValue()==UINT64_MAX||
           p.queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||p.list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||
           FAILED(p.queue->GetDevice(IID_PPV_ARGS(&producer))))return false;
        ComPtr<ID3D12Device> rd,ld,ad,fd;
        if(FAILED(p.imported[1]->GetDevice(IID_PPV_ARGS(&rd)))||FAILED(p.list->GetDevice(IID_PPV_ARGS(&ld)))||
           FAILED(p.allocator->GetDevice(IID_PPV_ARGS(&ad)))||FAILED(p.submitted->GetDevice(IID_PPV_ARGS(&fd)))||
           !Same(rd.Get(),producer.Get())||!Same(ld.Get(),producer.Get())||!Same(ad.Get(),producer.Get())||
           !Same(fd.Get(),producer.Get())||FAILED(producer->GetDeviceRemovedReason()))return false;
        const auto d=p.imported[1]->GetDesc();const auto& original=p.descriptors[1];
        D3D11_TEXTURE2D_DESC prepared{};packet->Prepared(1)->GetDesc(&prepared);
        return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&TextureExtent(original.Width,original.Height)&&
            d.Width==original.Width&&d.Height==original.Height&&d.DepthOrArraySize==1&&d.MipLevels==1&&
            d.SampleDesc.Count==1&&!d.SampleDesc.Quality&&d.Format==DXGI_FORMAT_R32_FLOAT&&
            !(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)&&
            (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)&&
            prepared.Width==d.Width&&prepared.Height==d.Height&&prepared.Format==d.Format&&
            prepared.ArraySize==1&&prepared.MipLevels==1&&prepared.SampleDesc.Count==1&&!prepared.SampleDesc.Quality&&
            (prepared.MiscFlags&D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    }
    struct Deleter {void operator()(PictureLane* lane)const noexcept {
        if(!lane)return;auto& r=Quarantine();std::lock_guard lock(r.mutex);lane->next_=r.first;r.first=lane;
    }};
    using Ptr=std::unique_ptr<PictureLane,Deleter>;
    static Ptr Create(ID3D11DeviceContext* game,const GenshinGuides::Owner& owner,ID3D11Resource* source,
                      const std::shared_ptr<const GenshinTransport::QueuedTransfer>& packet) {
        Reap();ComPtr<ID3D12Device> producer;if(!Packet(game,owner,source,packet,producer))return {};
        auto n=Detail::lanes.load();do{if(n>=3)return {};}while(!Detail::lanes.compare_exchange_weak(n,n+1));
        PictureLane* raw=nullptr;try{raw=new PictureLane;}catch(...){Detail::lanes.fetch_sub(1);throw;}
        Ptr lane(raw);lane->counted_=true;game->GetDevice(&lane->game_);
        if(!lane->game_||FAILED(game->QueryInterface(IID_PPV_ARGS(&lane->gameContext_))))return {};
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
        if(FAILED(lane->game_.As(&dxgi))||FAILED(dxgi->GetAdapter(&adapter))||FAILED(adapter->GetDesc(&desc))||
           !SameLuid(desc.AdapterLuid,producer->GetAdapterLuid()))return {};
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL actual{};
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        GenshinDepthPortable::PrivateCreation privateCreation;
        if(FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,2,D3D11_SDK_VERSION,&device,&actual,&context))||actual<D3D_FEATURE_LEVEL_11_0||
           Same(device.Get(),lane->game_.Get())||FAILED(device.As(&lane->device_))||FAILED(context.As(&lane->context_)))return {};
        ComPtr<IDXGIDevice> pd;ComPtr<IDXGIAdapter> pa;DXGI_ADAPTER_DESC pdesc{};
        if(FAILED(device.As(&pd))||FAILED(pd->GetAdapter(&pa))||FAILED(pa->GetDesc(&pdesc))||
           !SameLuid(desc.AdapterLuid,pdesc.AdapterLuid)||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return {};
        lane->producer_=producer;lane->queue_=packet->Get().queue;return lane;
    }
    bool Matches(ID3D11DeviceContext* game,ID3D12CommandQueue* queue)const noexcept {
        if(thread_!=GetCurrentThreadId()||poisoned_)return false;
        for(const auto& j:jobs_)if(j&&j->poisoned_)return false;
        return Same(game,gameContext_.Get())&&Same(queue,queue_.Get());
    }
    using Mapper=std::function<ComPtr<ID3D11Texture2D>(ID3D11Device5*,ID3D11DeviceContext4*,ID3D11Texture2D*)>;
    std::shared_ptr<PictureJob> Submit(ID3D11DeviceContext* game,const GenshinGuides::Owner& owner,
        ID3D11Resource* source,const std::shared_ptr<const GenshinTransport::QueuedTransfer>& packet,
        UINT width,UINT height,std::shared_ptr<void> roots,const Mapper& mapper) {
        if(!Matches(game,packet?packet->Get().queue.Get():nullptr)||!TextureExtent(width,height)||!roots||!mapper)return {};
        ComPtr<ID3D12Device> producer;
        if(!Packet(game,owner,source,packet,producer)||!Same(producer.Get(),producer_.Get()))return {};
        Collect();std::shared_ptr<PictureJob>* slot=nullptr,*vacant=nullptr,*discardable=nullptr;
        for(auto& j:jobs_){
            if(!j){if(!vacant)vacant=&j;continue;}
            if(j.use_count()!=1||!j->retired_||!j->Complete())continue;
            if(!discardable)discardable=&j;
            if(j->width_==width&&j->height_==height&&j->readyValue_<UINT64_MAX-1){slot=&j;break;}
        }
        std::shared_ptr<PictureJob> j;
        if(slot)j=*slot;
        else {
            slot=vacant?vacant:discardable;if(!slot)return {};slot->reset();
            j=std::shared_ptr<PictureJob>(new PictureJob);
            // Additional shared output uses the existing isolation reservation;
            // mapper UV/depth and buffer roots keep their existing budgets.
            const UINT64 bytes=UINT64(width)*height*16;
            bool reserved=Detail::Reserve(bytes);
            if(!reserved)for(auto& old:jobs_)if(old&&old.use_count()==1&&old->retired_&&old->Complete()){
                old.reset();if((reserved=Detail::Reserve(bytes)))break;
            }
            if(!reserved)return {};j->bytes_=bytes;j->width_=width;j->height_=height;
            j->device_=device_;j->game_=game_;j->producer_=producer_;j->gameContext_=gameContext_;
            D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.Usage=D3D11_USAGE_DEFAULT;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
            ComPtr<IDXGIResource1> shared;ComPtr<ID3D11Device1> game1;ComPtr<ID3D11Device5> game5;
            Detail::Handle oh,rh;
            if(FAILED(device_->CreateTexture2D(&d,nullptr,&j->output_))||FAILED(j->output_.As(&shared))||
               FAILED(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&oh.value))||
               FAILED(game_.As(&game1))||FAILED(game_.As(&game5))||
               FAILED(game1->OpenSharedResource1(oh.value,IID_PPV_ARGS(&j->gameOutput_)))||
               FAILED(device_->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&j->ready_)))||
               FAILED(j->ready_->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&rh.value))||
               FAILED(game5->OpenSharedFence(rh.value,IID_PPV_ARGS(&j->gameReady_))))return {};
        }
        // Same-object imports may remain cached, but the new genuine packet
        // (and its immutable value/owner) is mandatory on EVERY submission.
        auto input=j->input_;auto fence=j->inputFence_;
        if(!input||!Same(j->boundInput_.Get(),packet->Get().imported[1].Get())){
            Detail::Handle h;input.Reset();
            if(FAILED(producer_->CreateSharedHandle(packet->Get().imported[1].Get(),nullptr,GENERIC_ALL,nullptr,&h.value))||
               FAILED(device_->OpenSharedResource1(h.value,IID_PPV_ARGS(&input))))return {};
        }
        if(!fence||!Same(j->boundFence_.Get(),packet->Get().submitted.Get())){
            Detail::Handle h;fence.Reset();
            if(FAILED(producer_->CreateSharedHandle(packet->Get().submitted.Get(),nullptr,GENERIC_ALL,nullptr,&h.value))||
               FAILED(device_->OpenSharedFence(h.value,IID_PPV_ARGS(&fence))))return {};
        }
        j->input_=std::move(input);j->inputFence_=std::move(fence);
        j->boundInput_=packet->Get().imported[1];j->boundFence_=packet->Get().submitted;
        j->packet_=packet;j->rasterRoots_=std::move(roots);++j->readyValue_;
        j->ordered_=j->signalled_=j->retired_=false;
        // Install ALL roots before the first GPU Wait. Exceptions and partial
        // work use the same physical Signal/Flush. Failed Signal/Wait retains
        // and poisons the job; CPU return cannot free/reuse its resources.
        *slot=j;j->issued_=true;bool mapped=false;
        try{
            if(SUCCEEDED(context_->Wait(j->inputFence_.Get(),packet->Get().submittedValue))){
                auto result=mapper(device_.Get(),context_.Get(),j->input_.Get());
                if(result){D3D11_TEXTURE2D_DESC rd{};result->GetDesc(&rd);
                    if(rd.Width==width&&rd.Height==height&&rd.Format==DXGI_FORMAT_R32G32B32A32_FLOAT&&
                       rd.ArraySize==1&&rd.MipLevels==1&&rd.SampleDesc.Count==1&&!rd.SampleDesc.Quality){
                        context_->CopyResource(j->output_.Get(),result.Get());mapped=true;}
                }
            }else j->poisoned_=poisoned_=true;
        }catch(...){mapped=false;}
        j->signalled_=SUCCEEDED(context_->Signal(j->ready_.Get(),j->readyValue_));context_->Flush();
        if(!j->signalled_)j->poisoned_=poisoned_=true;
        if(!mapped||!j->signalled_||j->poisoned_)return {};
        return j;
    }

};
} // namespace GenshinFgQueryIsolation
