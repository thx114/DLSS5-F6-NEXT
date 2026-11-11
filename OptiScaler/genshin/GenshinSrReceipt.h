// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include "GenshinTypedGuides.h"
#include "GenshinProducerTransport.h"
#include <nvsdk_ngx.h>
#include <dlssnr/DlssNr_NgxResourceParameter.h>
#include <cstdint>
#include <type_traits>

namespace GenshinSrReceipt
{
enum class Outcome : uint32_t
{
    AwaitingEvaluate, CreateFailed, CreateRebuild, NoHandle, InvalidContext, SkipFirstFrames,
    BackendRebuild, NoFeature, EvaluateFailed, ActualEvaluateSucceeded, IdentityMismatch,
    NestedAttempt, PublicFailure, UnsupportedFeature
};
enum class GuideStage : unsigned char { BeforeEvaluate, AfterEvaluate };
struct Identity
{
    uint64_t instance=0,epoch=0,generation=0;
    uint32_t sourceFrame=0,reserved=0;
    uint64_t device=0,context=0,parameters=0,output=0;
};
struct Observation
{
    uint64_t device=0,context=0,parameters=0,output=0,handle=0;
};
struct Record
{
    uint32_t version=1,bytes=sizeof(Record);
    uint64_t attempt=0;
    Identity identity{};
    uint64_t handle=0;
    Outcome outcome=Outcome::AwaitingEvaluate;
    uint32_t evaluateCalls=0;
};
static_assert(std::is_standard_layout_v<Record> && std::is_trivially_copyable_v<Record>);
static_assert(sizeof(Identity)==64 && sizeof(Observation)==40 && sizeof(Record)==96);

inline bool SameIdentity(const Identity& a,const Identity& b) noexcept
{
    return a.instance==b.instance && a.epoch==b.epoch && a.generation==b.generation && a.sourceFrame==b.sourceFrame &&
        a.device==b.device && a.context==b.context && a.parameters==b.parameters && a.output==b.output;
}
class Scope;
inline thread_local Scope* active=nullptr;
// The legacy SR scope has no independently established NR/FG guide contract.
// Normal game/API inputs retain their existing admission; internal legacy inputs fail closed.
inline bool AllowsDownstreamGuides() noexcept { return active == nullptr; }
inline thread_local unsigned char threadIdentity;
inline std::atomic<uint64_t> attempts{1};

// Only the internal Genshin dispatch owns this stack-local attempt. No shared State or public ABI.
class Scope
{
    Record record_{};
    const void* thread_=&threadIdentity;
    bool owns_=false,rejected_=false;
    std::shared_ptr<const GenshinGuides::Receipt> guides_;
    bool nrReported_=false,fgReported_=false;
    ID3D12GraphicsCommandList* outputList_=nullptr;
    NVSDK_NGX_Parameter* outputParams_=nullptr;
    ID3D12Resource* vendorOutput_=nullptr;ID3D12Resource* finalOutput_=nullptr;
    unsigned outputPipelines_=0,outputProviderCalls_=0;
    bool outputOpen_=false,outputProviderSuccess_=false,outputPipelineSuccess_=false;
    std::optional<GenshinGuides::SrOutputGrid> observedOutputGrid_;
    std::shared_ptr<GenshinTransport::ProducerFrame> producer_;
    std::unique_ptr<GenshinTransport::Scope> transport_;
    bool Match(const Observation& o,bool requireHandle) noexcept
    {
        if(thread_!=&threadIdentity || !owns_ || active!=this)return false;
        const auto& id=record_.identity;
        if(o.device!=id.device || o.context!=id.context || o.parameters!=id.parameters || o.output!=id.output ||
            (requireHandle && (!record_.handle || record_.handle!=o.handle)))
        {rejected_=true;record_.outcome=Outcome::IdentityMismatch;return false;}
        return true;
    }
public:
    Scope(const Identity& id,bool genshinAdmitted,std::shared_ptr<const GenshinGuides::Receipt> guides={}) noexcept
    {
        guides_=std::move(guides);producer_=GenshinTransport::ProducerInvocation::Current();record_.identity=id;record_.attempt=attempts.fetch_add(1,std::memory_order_relaxed);
        if(!genshinAdmitted || !record_.attempt || !id.instance || !id.epoch || !id.generation ||
            !id.device || !id.context || !id.parameters || !id.output)
        {rejected_=true;record_.outcome=Outcome::IdentityMismatch;return;}
        if(active){active->rejected_=true;active->record_.outcome=Outcome::NestedAttempt;
            rejected_=true;record_.outcome=Outcome::NestedAttempt;return;}
        active=this;owns_=true;
    }
    ~Scope(){
        if(producer_ && !UnsubmittedBackendRebuild() &&
            (rejected_ || record_.evaluateCalls!=1 || record_.outcome!=Outcome::ActualEvaluateSucceeded))producer_->Revoke();
        if(transport_ && producer_)producer_->Finish(*transport_);
        transport_.reset();
        if(owns_ && thread_==&threadIdentity && active==this)active=nullptr;
    }
    bool BeginObservationTransport(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* params,uint64_t configEpoch) {
        if(!owns_ || active!=this || rejected_ || transport_ || !guides_ || !producer_ || !producer_->Live() ||
            producer_->Guides()!=guides_ || !context || !params || thread_!=&threadIdentity)return false;
        auto owner=guides_->Get().owner;const auto& id=record_.identity;
        Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);
        owner.device=reinterpret_cast<uint64_t>(device.Get());owner.context=reinterpret_cast<uint64_t>(context);
        owner.thread=GetCurrentThreadId();owner.configEpoch=configEpoch;
        owner.instance=id.instance;owner.historyEpoch=id.epoch;owner.generation=id.generation;owner.sourceFrame=id.sourceFrame;
        if(reinterpret_cast<uint64_t>(params)!=id.parameters || owner.device!=id.device || owner.context!=id.context ||
            !GenshinGuides::SameOwner(owner,guides_->Get().owner)){producer_->Revoke();return false;}
        const char* names[]={NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Output};
        std::array<ID3D11Resource*,4> resources{};
        for(size_t i=0;i<4;++i)resources[i]=DlssNr::NgxParameter::ReadResource<ID3D11Resource>(params,names[i]);
        Microsoft::WRL::ComPtr<IUnknown> output;
        if(!resources[3] || FAILED(resources[3]->QueryInterface(IID_PPV_ARGS(&output))) ||
            reinterpret_cast<uint64_t>(output.Get())!=id.output){producer_->Revoke();return false;}
        transport_=producer_->Open(owner,resources,params);
        return transport_!=nullptr;
    }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    void BindHandle(uint64_t handle) noexcept
    {
        if(!owns_ || active!=this || thread_!=&threadIdentity || !handle){rejected_=true;record_.outcome=Outcome::NoHandle;return;}
        if(record_.handle && record_.handle!=handle){rejected_=true;record_.outcome=Outcome::IdentityMismatch;return;}
        record_.handle=handle;
    }
    void Publish(const Observation& observation,Outcome outcome,bool evaluated=false) noexcept
    {
        if(!Match(observation,record_.handle!=0))return;
        if((outcome==Outcome::ActualEvaluateSucceeded && !evaluated) ||
            (evaluated && outcome!=Outcome::ActualEvaluateSucceeded && outcome!=Outcome::EvaluateFailed))
        {rejected_=true;record_.outcome=Outcome::IdentityMismatch;return;}
        if(evaluated){++record_.evaluateCalls;
            if(!record_.handle || record_.evaluateCalls!=1){rejected_=true;record_.outcome=Outcome::IdentityMismatch;return;}}
        if(rejected_)return;
        record_.outcome=outcome;
        if(outcome==Outcome::ActualEvaluateSucceeded && evaluated && guides_ && outputPipelineSuccess_ &&
           outputPipelines_==1 && outputProviderCalls_==1 && observedOutputGrid_)
            guides_->RecordOutputGrid(*observedOutputGrid_);
        if(outcome==Outcome::BackendRebuild && UnsubmittedBackendRebuild()) {
            // No Evaluate/transport work occurred. Preserve the producer for the
            // next phase, but this attempt grants no old or new guide authority.
            if(guides_)guides_->Retire();
        } else if(outcome!=Outcome::AwaitingEvaluate && outcome!=Outcome::ActualEvaluateSucceeded)rejected_=true;
    }
    void Fail(Outcome outcome) noexcept
    {if(thread_==&threadIdentity && owns_ && active==this){rejected_=true;record_.outcome=outcome;}}
    GenshinGuides::Reason GuideAdmission(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* params,uint64_t configEpoch,GenshinGuides::Consumer consumer,bool publish,GuideStage stage=GuideStage::AfterEvaluate) noexcept
    {
        using namespace GenshinGuides;
        Reason reason=Reason::MissingReceipt;
        if(guides_ && owns_ && active==this && !rejected_ && context && params && reinterpret_cast<uint64_t>(params)==record_.identity.parameters)
        {
            Call call;call.owner=guides_->Get().owner;
            const auto& id=record_.identity;
            Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);
            call.owner.device=reinterpret_cast<uint64_t>(device.Get());call.owner.context=reinterpret_cast<uint64_t>(context);
            call.owner.instance=id.instance;call.owner.historyEpoch=id.epoch;call.owner.generation=id.generation;call.owner.sourceFrame=id.sourceFrame;
            call.owner.thread=GetCurrentThreadId();call.owner.configEpoch=configEpoch;
            const char* names[]={NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Output};
            for(unsigned i=0;i<4;++i)call.resources[i]=DlssNr::NgxParameter::ReadResource<ID3D11Resource>(params,names[i]);
            params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&call.renderWidth);
            params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&call.renderHeight);
            const auto jitterXResult=params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&call.jitterX);
            const auto jitterYResult=params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&call.jitterY);
            const auto mvXResult=params->Get(NVSDK_NGX_Parameter_MV_Scale_X,&call.motionScaleX);
            const auto mvYResult=params->Get(NVSDK_NGX_Parameter_MV_Scale_Y,&call.motionScaleY);
            call.temporalParametersRead=jitterXResult==NVSDK_NGX_Result_Success && jitterYResult==NVSDK_NGX_Result_Success &&
                mvXResult==NVSDK_NGX_Result_Success && mvYResult==NVSDK_NGX_Result_Success;
            const bool evaluated=record_.evaluateCalls==1 && record_.outcome==Outcome::ActualEvaluateSucceeded;
            if(stage==GuideStage::BeforeEvaluate)
                reason=record_.evaluateCalls==0 && record_.outcome==Outcome::AwaitingEvaluate
                    ? guides_->Prepare(call,consumer) : Reason::EvaluateUnconfirmed;
            else
                reason=guides_->Admit(call,consumer,evaluated);
        }
        auto& reported=consumer==GenshinGuides::Consumer::NoiseReduction?nrReported_:fgReported_;
        if(publish && !reported && reason!=GenshinGuides::Reason::None){
            reported=true;
            if(guides_)GenshinGuides::PublishRejected(guides_.get(),consumer,reason);
            else if(owns_ && active==this && thread_==&threadIdentity){
                // No receipt means no producer/device epoch evidence; report only this actual scope.
                const auto& id=record_.identity;Owner owner;
                owner.device=id.device;owner.context=id.context;owner.configEpoch=configEpoch;
                owner.instance=id.instance;owner.generation=id.generation;owner.historyEpoch=id.epoch;
                owner.sourceFrame=id.sourceFrame;owner.thread=GetCurrentThreadId();
                GenshinGuides::PublishRejected(owner,consumer,reason);
            }
        }
        return reason;
    }

    // Optional Genshin-only output-grid observation. No observer failure may
    // change the provider result or ordinary SR/motion-only Present NR.
    bool BeginOutputPipeline(ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* params,
        ID3D12Resource* vendorOutput,ID3D12Resource* finalOutput,bool coordinatePreserving) noexcept {
        ++outputPipelines_;
        if(outputPipelines_!=1 || !coordinatePreserving || thread_!=&threadIdentity || active!=this ||
           !owns_ || rejected_ || !guides_ || !producer_ || !producer_->Live() || !transport_ ||
           GenshinTransport::Scope::Current()!=transport_.get() || !transport_->lease ||
           record_.evaluateCalls || record_.outcome!=Outcome::AwaitingEvaluate || !list || !params ||
           reinterpret_cast<uint64_t>(params)!=record_.identity.parameters || !vendorOutput || !finalOutput ||
           transport_->lease->Resource(transport_->owner,3)!=finalOutput)return false;
        const auto& f=guides_->Get();const auto a=vendorOutput->GetDesc(),b=finalOutput->GetDesc();
        if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || b.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
           a.Width!=f.outputWidth.value || a.Height!=f.outputHeight.value || a.Width!=b.Width || a.Height!=b.Height ||
           a.DepthOrArraySize!=1 || a.MipLevels!=1 || a.SampleDesc.Count!=1 || a.SampleDesc.Quality ||
           b.DepthOrArraySize!=1 || b.MipLevels!=1 || b.SampleDesc.Count!=1 || b.SampleDesc.Quality)return false;
        Microsoft::WRL::ComPtr<ID3D12Device> ld,vd,fd;
        if(FAILED(list->GetDevice(IID_PPV_ARGS(&ld))) || FAILED(vendorOutput->GetDevice(IID_PPV_ARGS(&vd))) ||
           FAILED(finalOutput->GetDevice(IID_PPV_ARGS(&fd))) || ld.Get()!=vd.Get() || ld.Get()!=fd.Get())return false;
        outputList_=list;outputParams_=params;vendorOutput_=vendorOutput;finalOutput_=finalOutput;outputOpen_=true;return true;
    }
    void BeforeDlss12(ID3D12GraphicsCommandList* list,NVSDK_NGX_Parameter* params,
        uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh) noexcept {
        ++outputProviderCalls_;
        if(!outputOpen_ || outputProviderCalls_!=1 || active!=this || thread_!=&threadIdentity ||
           list!=outputList_ || params!=outputParams_ || !guides_ || !transport_ || !transport_->lease)return;
        const auto& f=guides_->Get();
        if(rw!=f.renderWidth.value || rh!=f.renderHeight.value || ow!=f.outputWidth.value || oh!=f.outputHeight.value)return;
        const char* names[]={NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Output};
        for(size_t i=0;i<4;++i){ID3D12Resource* resource=nullptr;void* generic=nullptr;
            auto* expected=i==3?vendorOutput_:transport_->lease->Resource(transport_->owner,i);
            if(!expected || params->Get(names[i],&resource)!=NVSDK_NGX_Result_Success || resource!=expected ||
               params->Get(names[i],&generic)!=NVSDK_NGX_Result_Success || generic!=expected)return;}
        // The private Genshin dispatch writes explicit full-frame selectors.
        const char* zeros[]={NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
            NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y};
        for(const auto* name:zeros){unsigned value=UINT_MAX;
            if(params->Get(name,&value)!=NVSDK_NGX_Result_Success || value)return;}
        unsigned sw=0,sh=0;
        if(params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&sw)!=NVSDK_NGX_Result_Success ||
           params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&sh)!=NVSDK_NGX_Result_Success || sw!=rw || sh!=rh)return;
        GenshinGuides::SrOutputGrid grid;grid.owner=f.owner;grid.attempt=record_.attempt;
        grid.renderWidth=rw;grid.renderHeight=rh;grid.outputWidth=ow;grid.outputHeight=oh;
        if(params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&grid.jitterPixelsX)!=NVSDK_NGX_Result_Success ||
           params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&grid.jitterPixelsY)!=NVSDK_NGX_Result_Success || !grid.Matches(f.owner))return;
        observedOutputGrid_=grid;
    }
    void Dlss12Returned(bool success) noexcept {
        if(active==this && thread_==&threadIdentity && outputOpen_ && outputProviderCalls_==1)
            outputProviderSuccess_=success && observedOutputGrid_.has_value();
    }
    void EndOutputPipeline(bool complete) noexcept {
        if(active!=this || thread_!=&threadIdentity)return;
        ID3D12Resource* actual=nullptr;
        outputPipelineSuccess_=complete && outputOpen_ && outputPipelines_==1 && outputProviderCalls_==1 &&
            outputProviderSuccess_ && outputParams_->Get(NVSDK_NGX_Parameter_Output,&actual)==NVSDK_NGX_Result_Success && actual==finalOutput_;
        outputOpen_=false;
    }
    bool UnsubmittedBackendRebuild() const noexcept
    {
        return thread_==&threadIdentity && owns_ && active==this && !rejected_ && record_.handle &&
            record_.outcome==Outcome::BackendRebuild && !record_.evaluateCalls && !transport_ &&
            !outputPipelines_ && !outputProviderCalls_ &&
            (!producer_ || (producer_->Live() && !producer_->LeaseCreated()));
    }
    Record Snapshot() const noexcept{return record_;}
    bool Accept(const Identity& id,uint64_t handle,uint64_t attempt) const noexcept
    {
        return thread_==&threadIdentity && owns_ && active==this && !rejected_ && (!producer_ || producer_->Live()) && record_.version==1 && record_.bytes==sizeof(Record) &&
            record_.attempt==attempt && attempt && record_.handle==handle && handle && SameIdentity(record_.identity,id) &&
            record_.evaluateCalls==1 && record_.outcome==Outcome::ActualEvaluateSucceeded;
    }
};
inline void Publish(const Observation& observation,Outcome outcome,bool evaluated=false) noexcept
{if(active)active->Publish(observation,outcome,evaluated);}
}

namespace GenshinSrReceipt {
// Only the actual pre-SR NR ticket/FG transport callers use this full-contract preflight.
// Post-SR resource capture retains AdmitGuides and its exact successful Evaluate witness.
inline bool PrepareGuides(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* params,uint64_t configEpoch,GenshinGuides::Consumer consumer,bool publish=false) noexcept
{ return !active || active->GuideAdmission(context,params,configEpoch,consumer,publish,GuideStage::BeforeEvaluate)==GenshinGuides::Reason::None; }
inline bool AdmitGuides(ID3D11DeviceContext* context,NVSDK_NGX_Parameter* params,uint64_t configEpoch,GenshinGuides::Consumer consumer,bool publish=false) noexcept
{ return !active || active->GuideAdmission(context,params,configEpoch,consumer,publish)==GenshinGuides::Reason::None; }
}
