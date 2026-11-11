// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinBridgeConsumerContract.h"
#include "GenshinFsrSubstageProgress.h"
#include "GenshinProducerTransport.h"
#include <dlssnr/DlssNr_NgxResourceParameter.h>
#include <nvsdk_ngx.h>
#include "GenshinNrProfile.h"

namespace GenshinCaller {
// Bounded to the already established four-resource synchronous bridge seam.
// No config, semantic defaults, global State, FG capture, optical flow or new NR backend.
class BridgeAfterUpscale final {
    std::shared_ptr<GenshinTransport::ProducerFrame> producer_;
    std::shared_ptr<GenshinTransport::Lease> lease_;
    std::shared_ptr<const GenshinGuides::Receipt> guides_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    NVSDK_NGX_Parameter* parameters_=nullptr; // synchronous scope owns this block
    BindingSnapshot binding_;
    GenshinGuides::Call call_;
    ProviderInvocation invocation_;
    GenshinGuides::Reason prepared_=GenshinGuides::Reason::MissingReceipt;
    BridgeAfterUpscale()=default;
    static uint64_t Identity(IUnknown* object)noexcept {
        Microsoft::WRL::ComPtr<IUnknown> identity;
        return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)))
            ?reinterpret_cast<uint64_t>(identity.Get()):0;
    }
    bool ReadCurrent(BindingSnapshot& next)noexcept {
        auto* scope=GenshinTransport::Scope::Current();
        if(!scope || !producer_ || !producer_->Live() || !lease_ || scope->lease!=lease_ ||
           GenshinTransport::ProducerInvocation::Current()!=producer_ || scope->parameters!=parameters_ ||
           scope->owner.thread!=GetCurrentThreadId() || !GenshinGuides::SameOwner(scope->owner,producer_->Owner()))return false;
        auto observed=lease_->Snapshot();
        if(observed.revoked || observed.stage!=GenshinTransport::Stage::InputsOrdered ||
           !GenshinGuides::SameOwner(scope->owner,observed.owner) ||
           observed.parameters!=reinterpret_cast<uint64_t>(parameters_) ||
           observed.queue!=reinterpret_cast<uint64_t>(queue_.Get()))return false;
        Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
        context_->GetDevice(&sourceDevice);
        if(scope->owner.device!=reinterpret_cast<uint64_t>(sourceDevice.Get()) ||
           scope->owner.context!=reinterpret_cast<uint64_t>(context_.Get()))return false;
        Microsoft::WRL::ComPtr<ID3D12Device> listDevice,queueDevice;
        if(FAILED(list_->GetDevice(IID_PPV_ARGS(&listDevice))) ||
           FAILED(queue_->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
           Identity(listDevice.Get())!=Identity(queueDevice.Get()) ||
           FAILED(listDevice->GetDeviceRemovedReason()) || FAILED(sourceDevice->GetDeviceRemovedReason()) ||
           list_->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT || queue_->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        next.owner=scope->owner;next.parameters=reinterpret_cast<uint64_t>(parameters_);
        next.queue=reinterpret_cast<uint64_t>(queue_.Get());next.commandList=reinterpret_cast<uint64_t>(list_.Get());
        next.consumerDevice=Identity(listDevice.Get());
        const char* names[]={NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,
                            NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Output};
        for(size_t i=0;i<4;++i) {
            auto* imported=lease_->Resource(scope->owner,i);
            auto* bound=DlssNr::NgxParameter::ReadResource<ID3D12Resource>(parameters_,names[i]);
            if(!imported || !bound || Identity(imported)!=Identity(bound) ||
               observed.source[i]!=reinterpret_cast<uint64_t>(scope->resources[i]) ||
               observed.imported[i]!=reinterpret_cast<uint64_t>(imported))return false;
            // Check actual imported descriptors, not only caller-owned metadata.
            const auto d=bound->GetDesc();
            const auto& facts=producer_->Guides()->Get();
            Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
            if(FAILED(bound->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
               Identity(resourceDevice.Get())!=next.consumerDevice || d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
               d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality ||
               d.Width!=facts.textures[i].width.value || d.Height!=facts.textures[i].height.value ||
               d.Format!=facts.textures[i].format.value)return false;
            next.source[i]=Identity(scope->resources[i]);next.imported[i]=Identity(imported);next.bound[i]=Identity(bound);
        }
        return ValidateBinding(next)==BindingReason::None;
    }
    bool Current()noexcept {
        BindingSnapshot next;
        return ReadCurrent(next) && SameBinding(binding_,next)==BindingReason::None;
    }
    bool ReadCall()noexcept {
        auto* scope=GenshinTransport::Scope::Current();
        if(!scope)return false;
        call_.owner=scope->owner;call_.resources=scope->resources;
        call_.outputConsumer=AfterUpscaleNrRequest(binding_);
        unsigned width=0,height=0;
        const auto rw=parameters_->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&width);
        const auto rh=parameters_->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&height);
        call_.renderWidth=width;call_.renderHeight=height;
        unsigned outWidth=0,outHeight=0;
        const auto& f=producer_->Guides()->Get();
        if(parameters_->Get(NVSDK_NGX_Parameter_OutWidth,&outWidth)!=NVSDK_NGX_Result_Success ||
           parameters_->Get(NVSDK_NGX_Parameter_OutHeight,&outHeight)!=NVSDK_NGX_Result_Success ||
           outWidth!=f.outputWidth.value || outHeight!=f.outputHeight.value)return false;
        const char* origins[]={NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
            NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y};
        // Missing optional origins retain NGX's origin-zero default. An explicitly
        // nonzero value cannot describe these full private resources and is refused.
        for(const char* name:origins){unsigned origin=0;parameters_->Get(name,&origin);if(origin)return false;}
        const auto jx=parameters_->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&call_.jitterX);
        const auto jy=parameters_->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&call_.jitterY);
        const auto mx=parameters_->Get(NVSDK_NGX_Parameter_MV_Scale_X,&call_.motionScaleX);
        const auto my=parameters_->Get(NVSDK_NGX_Parameter_MV_Scale_Y,&call_.motionScaleY);
        call_.temporalParametersRead=rw==NVSDK_NGX_Result_Success && rh==NVSDK_NGX_Result_Success &&
            jx==NVSDK_NGX_Result_Success && jy==NVSDK_NGX_Result_Success &&
            mx==NVSDK_NGX_Result_Success && my==NVSDK_NGX_Result_Success;
        return true;
    }
public:
    BridgeAfterUpscale(const BridgeAfterUpscale&)=delete;
    BridgeAfterUpscale& operator=(const BridgeAfterUpscale&)=delete;
    // Called after four imported resources were actually bound on this command list's
    // provider parameter block, before Evaluate. No request-only correspondence constructor.
    static std::unique_ptr<BridgeAfterUpscale> Observe(ID3D11DeviceContext* context,
        NVSDK_NGX_Parameter* params,ID3D12CommandQueue* queue,ID3D12GraphicsCommandList* list) {
        try {
        auto* scope=GenshinTransport::Scope::Current();
        auto producer=GenshinTransport::ProducerInvocation::Current();
        if(!context || !params || !queue || !list || !scope || !scope->lease || !producer || !producer->Guides())return {};
        auto next=std::unique_ptr<BridgeAfterUpscale>(new BridgeAfterUpscale);
        next->producer_=std::move(producer);next->lease_=scope->lease;
        next->context_=context;next->queue_=queue;next->list_=list;next->parameters_=params;
        if(!next->ReadCurrent(next->binding_) || !next->ReadCall())return {};
        GenshinGuides::OutputCorrespondence observed;
        observed.source=GenshinGuides::CorrespondenceSource::ActualConsumerObservation;
        observed.owner=next->binding_.owner;observed.sourceOutput=next->binding_.source[3];
        observed.observed=next->call_.outputConsumer;
        next->guides_=GenshinGuides::Receipt::WithOutputConsumer(next->producer_->Guides(),observed);
        if(!next->guides_)return {};
        GenshinSubstage::Event(GenshinSubstage::Stage::NrGuides,GenshinSubstage::State::Running);
        next->prepared_=next->guides_->Prepare(next->call_,GenshinGuides::Consumer::NoiseReduction);
        if(next->prepared_!=GenshinGuides::Reason::None)GenshinSubstage::Event(GenshinSubstage::Stage::NrGuides,
            GenshinSubstage::State::Failed,GenshinSubstage::Resource::Nr,GenshinSubstage::Domain::Nr,static_cast<int64_t>(next->prepared_));
        return next;
        }catch(...){return {};}
    }
    GenshinGuides::Reason PreparedReason()const noexcept{return prepared_;}
    const GenshinGuides::OutputRequest& Request()const noexcept{return call_.outputConsumer;}
    const GenshinGuides::Facts& Facts()const noexcept{return guides_->Get();}
    // The caller must put exactly its existing provider Evaluate in this callback.
    // Admission cannot be manufactured by passing a public "providerSucceeded=true".
    template<class Evaluate> bool EvaluateProvider(Evaluate&& evaluate) {
        // Observation failure only abandons NR. The authorized SR provider still
        // executes once and its actual return controls the original bridge path.
        if(!Current())invocation_.AbandonConsumer();
        return invocation_.Run(std::forward<Evaluate>(evaluate));
    }
    // The existing NR lifecycle owns its model/resources. The callback receives the
    // source render frame, never this bridge's independent _frameCount/submission ring.
    // true means the NR function was offered this frame, not that its void API succeeded.
    template<class Nr> bool OfferNr(Nr&& nr) {
        using namespace GenshinGuides;
        if(!Current() || !ReadCall())return false;
        const auto reason=guides_->Admit(call_,Consumer::NoiseReduction,invocation_.Succeeded());
        GenshinSubstage::Event(GenshinSubstage::Stage::NrGuides,
            reason==Reason::None?GenshinSubstage::State::Succeeded:GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Nr,GenshinSubstage::Domain::Nr,static_cast<int64_t>(reason));
        if(reason!=Reason::None){PublishRejected(guides_.get(),Consumer::NoiseReduction,reason);return false;}
        if(prepared_!=Reason::None || !invocation_.BeginConsumer())return false;
        const auto& f=guides_->Get();
        if(!f.nrProfile || f.colourDomain.value!=ColourDomain::NativeBounded)return false;
        GenshinNr::Inputs inputs;inputs.profile=f.nrProfile;inputs.owner=binding_.owner;
        inputs.colourProcessing=GenshinNr::ColourProcessing::NativeBoundedIdentity;
        for(size_t i=0;i<4;++i)inputs.resources[i]=lease_->Resource(binding_.owner,i);
        inputs.renderWidth=f.renderWidth.value;inputs.renderHeight=f.renderHeight.value;
        inputs.outputWidth=f.outputWidth.value;inputs.outputHeight=f.outputHeight.value;
        inputs.motionScaleX=f.motionScaleX.value;inputs.motionScaleY=f.motionScaleY.value;
        GenshinNr::Scope nrScope(std::move(inputs),list_.Get(),parameters_,queue_.Get());
        std::forward<Nr>(nr)(static_cast<unsigned long long>(binding_.owner.sourceFrame));
        // Refuse submission/copyback if a consumer replaced an unleased parameter.
        if(!Current()){lease_->Revoke();return false;}
        return true;
    }
    // producer_ and lease_ remain pinned through this local caller's lifetime.
    // The original bridge calls Restored/Submitted/Copyback on that exact Scope::lease
    // with its actual current owner, queue, list and allocator. No NR qualification
    // is allowed to replace those existing transport checks or GPU retirement.
};
}
