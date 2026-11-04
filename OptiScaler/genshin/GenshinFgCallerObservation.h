// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFgObservationContract.h"
#include "GenshinSrReceipt.h"
#include <framegen/IFGFeature_Dx12.h>

class DLSSG_Dx12;
class FSRFG_Dx12;
class XeFG_Dx12;
namespace GenshinCaller {
// Lives on the real NGX DX11 evaluation stack. It neither calls a backend nor
// changes its input. Accepted is reachable only from the concrete SetResource
// success paths, while their existing resource/execution guards remain held.
class FgCallerObservation final {
    friend class ::DLSSG_Dx12;
    friend class ::FSRFG_Dx12;
    friend class ::XeFG_Dx12;
    inline static thread_local FgCallerObservation* active_=nullptr;
    inline static thread_local FgObservationBudget budget_;
    FgCallerObservation* previous_=nullptr;
    std::shared_ptr<GenshinTransport::ProducerFrame> producer_;
    GenshinSrReceipt::Scope* sr_=nullptr;
    GenshinSrReceipt::Record initial_{};
    IFGFeature_Dx12* provider_=nullptr;
    GenshinGuides::Owner owner_{};
    std::array<FgBindingRecord,3> bindings_{}; // depth, velocity, HUD-less picture
    std::array<Microsoft::WRL::ComPtr<IUnknown>,12> pins_;
    unsigned accepted_=0, matched_=0;
    bool sampled_=false, finished_=false;
    static uint64_t Identity(IUnknown* value) noexcept {
        Microsoft::WRL::ComPtr<IUnknown> identity;
        return value && SUCCEEDED(value->QueryInterface(IID_PPV_ARGS(&identity)))
            ? reinterpret_cast<uint64_t>(identity.Get()):0;
    }
    static void Accepted(IFGFeature_Dx12& provider,const Dx12Resource& input,
        Dx12Resource& bound,ID3D12CommandQueue* queue,uint64_t frame,int index) noexcept {
        auto* self=active_;
        if(!self || !self->sampled_ || self->finished_ || self->provider_!=&provider ||
           !self->producer_ || !self->producer_->Live() || GenshinSrReceipt::active!=self->sr_ ||
           GenshinTransport::ProducerInvocation::Current()!=self->producer_ ||
           self->owner_.thread!=GetCurrentThreadId())return;
        const auto type=input.type;
        const size_t slot=type==FG_ResourceType::Depth?0:type==FG_ResourceType::Velocity?1:
            type==FG_ResourceType::HudlessColor?2:3;
        if(slot>=3)return; // UIColor is never relabelled as a full or HUD-less picture.
        ++self->accepted_;
        auto* scope=GenshinTransport::Scope::Current();
        if(!scope || !scope->lease || !GenshinGuides::SameOwner(scope->owner,self->owner_) ||
           index!=provider.GetIndex() || frame!=provider.FrameCount() ||
           !input.cmdList || reinterpret_cast<uintptr_t>(input.cmdList)<=1 ||
           input.cmdList!=bound.cmdList || !queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
        const auto observation=scope->lease->Snapshot();
        if(observation.revoked || observation.queue!=reinterpret_cast<uint64_t>(queue))return;
        const size_t sourceIndex=slot+1;
        auto* imported=scope->lease->Resource(scope->owner,sourceIndex);
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice,listDevice,resourceDevice;
        auto* destination=bound.GetResource();
        if(!destination || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
           FAILED(input.cmdList->GetDevice(IID_PPV_ARGS(&listDevice))) ||
           FAILED(destination->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
           Identity(queueDevice.Get())!=Identity(listDevice.Get()) ||
           Identity(queueDevice.Get())!=Identity(resourceDevice.Get()))return;
        FgBindingRecord record{self->owner_,reinterpret_cast<uint64_t>(&provider),frame,
            Identity(queue),Identity(input.cmdList),Identity(queueDevice.Get()),
            Identity(imported),Identity(input.resource),Identity(destination),static_cast<uint32_t>(index)};
        if(CompareFgBinding(record,self->owner_,Identity(imported))!=FgObservationReason::None)return;
        // Preserve the actual accepted source and destination. A copy/format
        // conversion is an observation, never a certified source-output relation.
        self->bindings_[slot]=record;
        self->pins_[slot*4]=input.resource;self->pins_[slot*4+1]=destination;
        self->pins_[slot*4+2]=input.cmdList;self->pins_[slot*4+3]=queue;
        self->matched_|=1u<<slot;
    }
public:
    explicit FgCallerObservation(IFGFeature_Dx12* provider) noexcept {
        if(!GenshinSrReceipt::active)return; // Ordinary game/API path is a pointer check.
        producer_=GenshinTransport::ProducerInvocation::Current();
        if(!producer_ || !producer_->Live())return;
        owner_=producer_->Owner();sr_=GenshinSrReceipt::active;initial_=sr_->Snapshot();provider_=provider;
        sampled_=budget_.Take(owner_);
        previous_=active_;active_=this;
    }
    ~FgCallerObservation(){if(active_==this)active_=previous_;}
    FgCallerObservation(const FgCallerObservation&)=delete;
    FgCallerObservation& operator=(const FgCallerObservation&)=delete;
    void Finish(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* params,uint64_t epoch) noexcept {
        if(!sampled_ || finished_)return;
        finished_=true;
        if(!producer_ || !producer_->Live() || GenshinSrReceipt::active!=sr_ ||
           owner_.thread!=GetCurrentThreadId() || reinterpret_cast<uint64_t>(params)!=initial_.identity.parameters)return;
        const auto now=sr_->Snapshot();
        const bool evaluated=sr_->Accept(initial_.identity,now.handle,initial_.attempt);
        // Preserve the full existing semantic/output refusal. Actual accepted
        // bindings are diagnostic evidence only; no ActualConsumerObservation,
        // ready flag, request default, receipt derivative or global gate is created.
        const auto reason=sr_->GuideAdmission(context,params,epoch,GenshinGuides::Consumer::FrameGeneration,false);
        const auto& picture=bindings_[2];
        LOG_INFO("Genshin FG caller observation: source frame {}; instance {}; generation {}; config epoch {}; producer epoch {}; SR witness {}; accepted bindings {}; matched mask {}; provider {:X}; provider frame {}; slot {}; queue {:X}; context {:X}; source {:X}; destination {:X}; typed reason {}; admission unchanged",
            owner_.sourceFrame,owner_.instance,owner_.generation,owner_.configEpoch,owner_.producerEpoch,evaluated,
            accepted_,matched_,reinterpret_cast<uint64_t>(provider_),picture.providerFrame,picture.providerIndex,
            picture.queue,picture.commandList,picture.acceptedInput,picture.acceptedDestination,
            GenshinGuides::ReasonText(reason));
        for(size_t i=0;i<bindings_.size();++i) {
            if(!(matched_&(1u<<i)))continue;
            const auto& binding=bindings_[i];
            LOG_INFO("Genshin FG accepted binding observation: source frame {}; slot kind {}; provider frame {}; provider index {}; queue {:X}; context {:X}; device {:X}; leased {:X}; accepted source {:X}; accepted destination {:X}; transport observation only",
                binding.owner.sourceFrame,i,binding.providerFrame,binding.providerIndex,binding.queue,
                binding.commandList,binding.device,binding.leasedInput,binding.acceptedInput,binding.acceptedDestination);
        }
    }
};
}
