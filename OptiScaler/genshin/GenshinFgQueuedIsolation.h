// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFgQueryIsolation.h"
#include "GenshinDx12Transport.h"
#include "GenshinOwnerIdentity.h"

namespace GenshinFgQueryIsolation {
// CPU-only adapter for genuine SR packets. It cannot construct a packet,
// certify guide semantics, obtain query authority, or issue a game GPU command.
// The caller must retain ALL existing native/reference/camera/picture checks.
inline bool PacketInputs(ID3D11DeviceContext* game,
    const std::shared_ptr<const GenshinTransport::QueuedTransfer>& current,
    const std::shared_ptr<const GenshinTransport::QueuedTransfer>& previous,
    const GenshinGuides::Owner& expectedCurrent,const GenshinGuides::Owner& expectedPrevious,
    Inputs& out) noexcept {
    out={};
    if(!game||!current||!previous||current==previous)return false;
    const auto& c=current->Get();const auto& p=previous->Get();
    GenshinGuides::Owner canonicalCurrent{},canonicalPrevious{};
    const GenshinGuides::OwnerIdentity::Context identity(game);
    if(!identity.ToCanonical(c.owner,canonicalCurrent)||!identity.ToCanonical(p.owner,canonicalPrevious)||
       !GenshinGuides::SameOwner(canonicalCurrent,expectedCurrent)||
       !GenshinGuides::SameOwner(canonicalPrevious,expectedPrevious)||
       !c.owner.deviceEpoch||!c.owner.producerEpoch||!c.owner.configEpoch||!c.owner.historyEpoch||
       !c.owner.instance||!c.owner.generation||p.owner.generation==UINT64_MAX||
       c.owner.generation!=p.owner.generation+1||c.owner.sourceFrame<=p.owner.sourceFrame||
       c.owner.deviceEpoch!=p.owner.deviceEpoch||c.owner.producerEpoch!=p.owner.producerEpoch||
       c.owner.configEpoch!=p.owner.configEpoch||c.owner.historyEpoch!=p.owner.historyEpoch||
       c.owner.instance!=p.owner.instance||!c.surfaceLifetime||!p.surfaceLifetime||
       !c.queue||!p.queue||!c.list||!p.list||!c.allocator||!p.allocator||!Same(c.queue.Get(),p.queue.Get()))return false;
    // QueuedTransfer binds Prepared[i] to imported[i] in its private factory.
    // Check distinct immutable history and shapes again; never select scratch.
    const std::array<ID3D11Texture2D*,3> prepared{
        current->Prepared(2),current->Prepared(1),previous->Prepared(1)};
    for(size_t i=0;i<3;++i){
        if(!prepared[i])return false;
        D3D11_TEXTURE2D_DESC d{};prepared[i]->GetDesc(&d);
        const auto& source=i==0?c.descriptors[2]:(i==1?c.descriptors[1]:p.descriptors[1]);
        if(d.Width!=source.Width||d.Height!=source.Height||d.Format!=source.Format||
           d.Format!=(i?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16_FLOAT)||
           d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||
           !(d.MiscFlags&D3D11_RESOURCE_MISC_SHARED_NTHANDLE))return false;
        for(size_t j=0;j<i;++j)if(Same(prepared[i],prepared[j]))return false;
    }
    Inputs result;result.resources={c.imported[2],c.imported[1],p.imported[1]};
    result.submitted={c.submitted,p.submitted};result.values={c.submittedValue,p.submittedValue};
    result.packets={current,previous}; // retain the real typed packet, not only its textures
    for(const auto& r:result.resources)if(!r)return false;
    for(size_t i=0;i<2;++i)if(!result.submitted[i]||!result.values[i]||result.values[i]==UINT64_MAX)return false;
    out=std::move(result);return true;
}
// Only this factory may seal execution authority from genuine transport
// packets. It is not a query-history, camera, or semantic certificate.
class PacketPair final {
    friend class PacketLane;
    DWORD thread_=GetCurrentThreadId();
    Inputs inputs_;
    ComPtr<ID3D11Device> game_;
    ComPtr<IUnknown> context_;
    ComPtr<ID3D12Device> producer_;
    ComPtr<ID3D12CommandQueue> queue_;
    PacketPair()=default;
public:
    PacketPair(const PacketPair&)=delete;PacketPair& operator=(const PacketPair&)=delete;
    static std::unique_ptr<const PacketPair> Open(ID3D11DeviceContext* game,
        const std::shared_ptr<const GenshinTransport::QueuedTransfer>& current,
        const std::shared_ptr<const GenshinTransport::QueuedTransfer>& previous,
        const GenshinGuides::Owner& expectedCurrent,const GenshinGuides::Owner& expectedPrevious) {
        auto result=std::unique_ptr<PacketPair>(new PacketPair);
        if(!PacketInputs(game,current,previous,expectedCurrent,expectedPrevious,result->inputs_))return {};
        game->GetDevice(&result->game_);
        if(!result->game_||FAILED(game->QueryInterface(IID_PPV_ARGS(&result->context_))))return {};
        result->queue_=current->Get().queue;
        if(result->queue_->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||
           FAILED(result->queue_->GetDevice(IID_PPV_ARGS(&result->producer_))))return {};
        for(const auto* view:{&current->Get(),&previous->Get()}){
            ComPtr<ID3D12Device> listDevice,allocatorDevice,fenceDevice;
            if(view->list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||
               FAILED(view->list->GetDevice(IID_PPV_ARGS(&listDevice)))||
               FAILED(view->allocator->GetDevice(IID_PPV_ARGS(&allocatorDevice)))||
               FAILED(view->submitted->GetDevice(IID_PPV_ARGS(&fenceDevice)))||
               !Same(listDevice.Get(),result->producer_.Get())||
               !Same(allocatorDevice.Get(),result->producer_.Get())||
               !Same(fenceDevice.Get(),result->producer_.Get()))return {};
        }
        return result;
    }
};
class PacketLane final {
    DWORD thread_=GetCurrentThreadId();
    ComPtr<ID3D11Device> game_;
    ComPtr<IUnknown> context_;
    ComPtr<ID3D12Device> producer_;
    ComPtr<ID3D12CommandQueue> queue_;
    Lane::Ptr lane_;
    PacketLane()=default;
public:
    static std::unique_ptr<PacketLane> Create(const PacketPair& pair,const void* shader,SIZE_T size) {
        if(pair.thread_!=GetCurrentThreadId())return {};
        auto result=std::unique_ptr<PacketLane>(new PacketLane);
        result->lane_=Lane::Create(pair.game_.Get(),pair.producer_.Get(),shader,size);
        if(!result->lane_)return {};
        result->game_=pair.game_;result->context_=pair.context_;result->producer_=pair.producer_;result->queue_=pair.queue_;
        return result;
    }
    bool Matches(const PacketPair& pair)const noexcept {
        return thread_==GetCurrentThreadId()&&pair.thread_==thread_&&
            Same(game_.Get(),pair.game_.Get())&&Same(context_.Get(),pair.context_.Get())&&
            Same(producer_.Get(),pair.producer_.Get())&&Same(queue_.Get(),pair.queue_.Get());
    }
    std::shared_ptr<Job> Submit(const PacketPair& pair,const Control& control) {
        return Matches(pair)?lane_->Submit(pair.inputs_,control):std::shared_ptr<Job>{};
    }
    Failure LastFailure()const noexcept {return lane_->LastFailure();}
    void Collect()noexcept {if(thread_==GetCurrentThreadId())lane_->Collect();}
};
} // namespace GenshinFgQueryIsolation
