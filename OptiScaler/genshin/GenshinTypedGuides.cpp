// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinTypedGuides.h"
#include "GenshinDepthEvidence.h"
#include "GenshinReturnedRaster.h"
#include "GenshinDepthSemantic.h"
#include "GenshinFgGameMotion.h"
#include "GenshinFamilySkip.h"
#include <pch.h>
#include <cmath>

namespace GenshinGameMotion {
std::shared_ptr<const NativeReference> NativeReference::Observe(ID3D11DeviceContext* context,
    const GenshinLegacy::NativeMrt::Frame& frame) noexcept {
    try {
        const auto refuse=[&](unsigned reason)->std::shared_ptr<const NativeReference> {
            static std::atomic<unsigned> reports{0};
            static std::atomic<unsigned> previousReason{0};
            static std::atomic<unsigned> transitions{0};
            const auto report=reports.fetch_add(1,std::memory_order_relaxed);
            const bool reasonChanged=previousReason.exchange(reason,std::memory_order_relaxed)!=reason;
            const bool boundedTransition=reasonChanged && transitions.fetch_add(1,std::memory_order_relaxed)<64;
            if(report<64 || boundedTransition || report%256==0)
                LOG_INFO("R835 native reference refusal: pid={} generation={} source_frame={} reason={} context={} buffer={} shader={} no_authority=true",
                    GetCurrentProcessId(),frame.generation,frame.frame,reason,frame.context,frame.constantBuffer,frame.shaderIdentity);
            return {};
        };
        if(!context || reinterpret_cast<uintptr_t>(context)!=frame.context || frame.thread!=GetCurrentThreadId() ||
           GenshinLegacy::NativeMrt::Validate(frame)!=GenshinLegacy::NativeMrt::Reason::Ready ||
           !GenshinFsrProfiles::Accumulate(frame.shader) || !frame.nrProfile ||
           !frame.nrProfile->MatchesShader(frame.deviceEpoch,frame.shaderIdentity,frame.shader))return refuse(1);
        Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);
        const auto shader=GenshinLegacy::Family::Current(context,device.Get(),frame.deviceEpoch);
        if(shader.identity!=frame.shaderIdentity || !shader.nrProfile)return refuse(2);
        // The original Bridge already owns CB/token selection for this exact
        // returned native transaction. Do not perform a second CB lookup or
        // impose a second phase oracle on the FG adapter.
        auto proof=std::shared_ptr<NativeReference>(new NativeReference);
        proof->context_=frame.context;proof->buffer_=frame.constantBuffer;
        proof->deviceEpoch_=frame.deviceEpoch;proof->configEpoch_=frame.configEpoch;
        proof->shader_=frame.shaderIdentity;proof->instance_=frame.instance;proof->generation_=frame.generation;
        proof->sourceFrame_=frame.frame;proof->renderWidth_=frame.renderWidth;proof->renderHeight_=frame.renderHeight;
        proof->renderInvX_=1.f/frame.renderWidth;proof->renderInvY_=1.f/frame.renderHeight;
        proof->outputInvX_=1.f/frame.outputWidth;proof->outputInvY_=1.f/frame.outputHeight;
        proof->cb28x_=proof->rawJitterX_=frame.rawJitterX;proof->cb28y_=proof->rawJitterY_=frame.rawJitterY;
        return proof;
    }catch(...){return {};}
}
}

namespace GenshinGuides {
using Microsoft::WRL::ComPtr;
static uint64_t Identity(IUnknown* object)noexcept {
    ComPtr<IUnknown> identity;
    return object&&SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)))?reinterpret_cast<uint64_t>(identity.Get()):0;
}
std::shared_ptr<const Receipt> Receipt::Observe(ID3D11DeviceContext* context,const GenshinFsr::Parameters& p,
    const std::array<ID3D11Texture2D*,4>& textures,uint64_t deviceEpoch,uint64_t producerEpoch,uint64_t configEpoch,
    const GenshinLegacy::NativeMrt::Transaction* native,
    std::shared_ptr<const GenshinReturnedRaster::Evidence> returnedRaster,
    const GenshinCameraHistory::Sample* camera,ObservationSubscription subscription) {
    if(!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!deviceEpoch||!producerEpoch||!configEpoch||
       !p.instance||!p.epoch||!p.generation||!std::isfinite(p.jitterX)||!std::isfinite(p.jitterY)||!std::isfinite(p.motionScale))return {};
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if(!device||FAILED(device->GetDeviceRemovedReason()))return {};
    std::unique_ptr<Receipt> next(new Receipt);
    auto& f=next->facts_;
    f.owner={reinterpret_cast<uint64_t>(device.Get()),reinterpret_cast<uint64_t>(context),deviceEpoch,producerEpoch,
             configEpoch,p.instance,p.generation,p.epoch,p.sourceFrame,GetCurrentThreadId()};
    for(unsigned i=0;i<textures.size();++i) {
        if(!textures[i])return {};next->lease_[i]=textures[i];
        D3D11_TEXTURE2D_DESC desc{};textures[i]->GetDesc(&desc);ComPtr<ID3D11Device> owner; textures[i]->GetDevice(&owner);
        if(owner.Get()!=device.Get()||!desc.Width||!desc.Height||desc.MipLevels!=1||desc.ArraySize!=1||desc.SampleDesc.Count!=1)return {};
        auto& t=f.textures[i];t.identity=Identity(textures[i]);t.device=reinterpret_cast<uint64_t>(owner.Get());
        if(!t.identity)return {};t.width={desc.Width,Source::TextureDescriptor};t.height={desc.Height,Source::TextureDescriptor};t.format={desc.Format,Source::TextureDescriptor};
        const auto w=i==3?p.outputWidth:p.renderWidth,h=i==3?p.outputHeight:p.renderHeight;
        if(desc.Width!=w||desc.Height!=h)return {};
    }
    if(f.textures[1].format.value!=DXGI_FORMAT_R32_FLOAT||f.textures[2].format.value!=DXGI_FORMAT_R16G16_FLOAT)return {};
    f.renderWidth={p.renderWidth,Source::ActualRenderToken};f.renderHeight={p.renderHeight,Source::ActualRenderToken};
    f.outputWidth={p.outputWidth,Source::ActualRenderToken};f.outputHeight={p.outputHeight,Source::ActualRenderToken};
    // Values are exactly what the actual caller requested; they do not establish their engine units/direction.
    f.jitterX={p.jitterX,Source::RequestedControl};f.jitterY={p.jitterY,Source::RequestedControl};
    f.motionScale={p.motionScale,Source::RequestedControl};f.motionEncoding={MotionEncoding::PreparedSignedSquareHalf2,Source::PreparationShader};
    // Same signed per-axis values written by MakeDispatch, not proof of engine motion direction.
    const float sign=p.positiveMotionScale?1.0f:-1.0f;
    f.motionScaleX={sign*p.renderWidth*p.motionScale,Source::RequestedControl};
    f.motionScaleY={sign*p.renderHeight*p.motionScale,Source::RequestedControl};
    if(!std::isfinite(f.motionScaleX.value)||!std::isfinite(f.motionScaleY.value))return {};
    if(native) {
        const auto* observed=native->ClaimedFrame();
        if(!observed || observed->device!=reinterpret_cast<uint64_t>(device.Get()) ||
           observed->context!=reinterpret_cast<uint64_t>(context) || observed->thread!=GetCurrentThreadId() ||
           observed->deviceEpoch!=deviceEpoch || observed->ownerEpoch!=producerEpoch || observed->configEpoch!=configEpoch ||
           observed->instance!=p.instance || observed->generation!=p.generation || observed->frame!=p.sourceFrame ||
           observed->renderWidth!=p.renderWidth || observed->renderHeight!=p.renderHeight ||
           observed->outputWidth!=p.outputWidth || observed->outputHeight!=p.outputHeight || p.originX || p.originY)return {};
        // The actual claimed transaction supplies the original source/token;
        // no external matrix, requested Z convention or diagnostic epoch may
        // create this record. Absence does not change existing NR admission.
        // Association occurred at this Claim, before scratch preparation.
        // Historical facts survive private state revocation; Current() does
        // not. The exact source/mip/full token must still match this receipt.
        if(returnedRaster && returnedRaster->Matches(*observed))f.returnedRaster=std::move(returnedRaster);
        f.depthSource=f.returnedRaster?GenshinReturnedRaster::Describe(f.returnedRaster->Get()):
            GenshinReturnedRaster::DescribeSelected(context,*observed);
        f.depthSource.prepared=f.textures[1].identity;
        // Observe the ORIGINAL native depth plane, not the private R32 prepared
        // texture. Keep the selected location for copy/clear correspondence;
        // it grants no DeviceZ/clear/jitter fact or CPU diagnostic subscription.
        if(subscription!=ObservationSubscription::SrReceiptOnly) {
            const auto originalDepth=GenshinReturnedRaster::detail::NativeDepthSource(*observed);
            if(originalDepth)GenshinDepthEvidence::ExpectDepthInput(context,reinterpret_cast<uintptr_t>(originalDepth.Get()),
                deviceEpoch,producerEpoch,configEpoch,p.instance,p.generation,p.sourceFrame,
                observed->inputs[1].firstMip,observed->inputs[1].viewFormat,
                observed->rawJitterX,observed->rawJitterY,p.jitterX,p.jitterY);
        }
        // Exact pinned shader + full native SRV/RTV/viewport/scissor + returned draw.
        // Texture layout certifies a full-frame resource profile, never colour or Z meaning.
        f.producerBuildKnown=true;f.engineSubrectKnown=true;
        // The Bridge decoder explicitly converts its signed-square encoding to
        // the FSR history-vector direction. Alternate flips/scales remain unqualified.
        if(!observed->controls.motionFlipY && observed->controls.positiveMotionScale && observed->controls.motionScale==1.0f &&
           p.positiveMotionScale && p.motionScale==observed->controls.motionScale)
            f.motionDirection={MotionDirection::CurrentToPrevious,Source::PinnedBridgeProfile};
        // Create-time SHA+size capability plus the completed native transaction.
        // This is a bounded numerical NR domain, not a scene-light/transfer claim.
        const auto& c=observed->controls;
        if(observed->nrProfile && observed->nrProfile->MatchesShader(deviceEpoch,observed->shaderIdentity,observed->shader) &&
           !c.depthFlipY && !c.motionFlipY && !c.motionJittered && c.positiveMotionScale && c.motionScale==1.0f &&
           !c.jitterDelay && c.jitterMode==3 && c.clampJitterHalf && !p.motionJittered && !p.hdr &&
           p.positiveMotionScale && p.motionScale==1.0f &&
           observed->inputs[0].viewFormat==DXGI_FORMAT_R8G8B8A8_UNORM &&
           observed->outputs[1].viewFormat==DXGI_FORMAT_R8G8B8A8_UNORM &&
           f.textures[0].format.value==DXGI_FORMAT_R8G8B8A8_UNORM &&
           f.textures[3].format.value==DXGI_FORMAT_R8G8B8A8_UNORM) {
            f.nrProfile=observed->nrProfile;
            f.colourDomain={ColourDomain::NativeBounded,Source::PinnedBridgeProfile};
            f.depthConvention={DepthConvention::NumericalMaximumWins,Source::PinnedBridgeProfile};
        }
        if(camera && camera->valid && SameOwner(camera->owner,f.owner)){
            next->depthCamera_=std::make_shared<const GenshinCameraHistory::Sample>(*camera);
            next->depthFrame_=std::make_shared<const GenshinLegacy::NativeMrt::Frame>(*observed);
            next->depthParameters_=p;
        }
        // Raw jitter UV evidence does not certify the SR transform's history phase.
        // Base after-SR NR does not use jitter; phase-dependent reuse stays disabled.
    }
    return std::shared_ptr<const Receipt>(next.release());
}
std::optional<SrOutputGrid> Receipt::OutputGrid()const noexcept {
    if(GetCurrentThreadId()!=facts_.owner.thread || retired_.load(std::memory_order_acquire))return {};
    if(producer_)return producer_->OutputGrid();
    return outputGrid_ && outputGrid_->Matches(facts_.owner)?outputGrid_:std::nullopt;
}
GenshinGameMotion::Reference Receipt::GameMotionReferenceForPresent()const noexcept {
    GenshinGameMotion::Reference result;
    if(producer_)return producer_->GameMotionReferenceForPresent();
    const auto grid=OutputGrid();
    if(!grid || !depthFrame_ || !depthFrame_->gameMotionReference ||
       retired_.load(std::memory_order_acquire) || GetCurrentThreadId()!=facts_.owner.thread)return result;
    const auto& f=*depthFrame_;const auto& native=*f.gameMotionReference;const auto& c=f.controls;
    if(!GenshinFsrProfiles::Accumulate(f.shader) || !f.nrProfile || !f.nrProfile->MatchesShader(f.deviceEpoch,f.shaderIdentity,f.shader) ||
       native.context_!=f.context || native.buffer_!=f.constantBuffer || native.shader_!=f.shaderIdentity ||
       native.instance_!=f.instance || native.generation_!=f.generation || native.sourceFrame_!=f.frame ||
       native.deviceEpoch_!=f.deviceEpoch || native.configEpoch_!=f.configEpoch ||
       native.renderWidth_!=f.renderWidth || native.renderHeight_!=f.renderHeight ||
       grid->renderWidth!=f.renderWidth || grid->renderHeight!=f.renderHeight ||
       grid->outputWidth!=f.outputWidth || grid->outputHeight!=f.outputHeight ||
       c.jitterDelay || c.jitterMode!=3 || !c.clampJitterHalf || c.depthFlipY || c.motionFlipY ||
       c.motionJittered || !c.positiveMotionScale || c.motionScale!=1.0f ||
       facts_.motionDirection.value!=MotionDirection::CurrentToPrevious ||
       facts_.motionDirection.source!=Source::PinnedBridgeProfile)return result;
    // Qualified native history ABI: previousNativeUV=currentNativeUV+MV.
    // rasterUV=nativeUV-CB28=srUV+Jaccepted/render. Therefore
    // delta=CB28+Jaccepted/render and vSR=vNative+deltaCur-deltaPrev.
    // ABI1 admits equal render extents and equal actual corrections only.
    const float preX=.5f-native.rawJitterX_*f.renderWidth,preY=.5f-native.rawJitterY_*f.renderHeight;
    if(preX<-.5f || preX>.5f || preY<-.5f || preY>.5f ||
       grid->jitterPixelsX!=preX || grid->jitterPixelsY!=preY)return result;
    const GenshinGuides::OwnerIdentity::Context ids(reinterpret_cast<ID3D11DeviceContext*>(f.context));
    if(!ids.ToCanonical(facts_.owner,result.owner))return {};
    // The accepted-SR phase comes from the current Render token, as in the
    // original Bridge. A lagging CB28 remains evidence only; it must not shift
    // this correction by one token.
    result.nativeToSrX=native.rawJitterX_+grid->jitterPixelsX/f.renderWidth;
    result.nativeToSrY=native.rawJitterY_+grid->jitterPixelsY/f.renderHeight;
    result.nativeEvidence_=f.gameMotionReference;result.acceptedGrid_=*grid;
    result.nativeShaderHash=f.shader;result.shaderIdentity=f.shaderIdentity;
    result.nativeOutputHistoryUv=true;result.outputReferenceProved=true;return result;
}
bool PreparedDepthSource::Matches(const Facts& facts)const noexcept {
    if(!frame_ || !camera_ || !prepared_ || !camera_->valid ||
       !SameOwner(owner_,facts.owner) || !SameOwner(camera_->owner,owner_) ||
       !grid_.Matches(owner_) || owner_.thread!=GetCurrentThreadId() ||
       !facts.producerBuildKnown || !facts.engineSubrectKnown ||
       grid_.renderWidth!=facts.renderWidth.value || grid_.renderHeight!=facts.renderHeight.value ||
       grid_.outputWidth!=facts.outputWidth.value || grid_.outputHeight!=facts.outputHeight.value)return false;
    const auto& content=facts.depthSource;
    if(provenance_.selected!=content.selected || provenance_.original!=content.original ||
       provenance_.prepared!=content.prepared || provenance_.lifetime!=content.lifetime ||
       provenance_.originalLifetime!=content.originalLifetime || provenance_.loss!=content.loss ||
       provenance_.version!=content.version || provenance_.clear!=content.clear ||
       provenance_.returned!=content.returned || provenance_.copyHops!=content.copyHops)return false;
    for(size_t i=0;i<textures_.size();++i){const auto& a=textures_[i];const auto& b=facts.textures[i];
        if(!a.identity || a.identity!=b.identity || a.device!=b.device || a.width.value!=b.width.value ||
           a.height.value!=b.height.value || a.format.value!=b.format.value)return false;}
    return textures_[1].format.value==DXGI_FORMAT_R32_FLOAT &&
        textures_[1].width.value==Width() && textures_[1].height.value==Height();
}
bool PreparedDepthSource::MatchesNumerics(const Facts& f)const noexcept {
    return NumericsKnown() && Matches(f) && f.preparedSource.get()==this &&
        f.depthConvention.source==Source::ActualRasterDepth && f.depthConvention.value==numerics_->convention &&
        f.depthClear.source==Source::ActualRasterDepth && f.depthClear.value==numerics_->clear &&
        f.depthToOutputJitterX.source==Source::ActualDepthMapping && f.depthToOutputJitterX.value==numerics_->jx &&
        f.depthToOutputJitterY.source==Source::ActualDepthMapping && f.depthToOutputJitterY.value==numerics_->jy;
}
void PreparedDepthSource::ApplyNumerics(Facts& result)const noexcept {
    if(!Matches(result))return;
    try {
        auto proof=numerics_;
        if(!proof){
            auto next=std::make_shared<Numerics>();bool retry=false;
            if(returnedRaster_ && returnedRaster_->Matches(*frame_)){
                const auto& interval=returnedRaster_->Get();
                if(!interval.clearReturned || !interval.semanticComplete || interval.coveredWriters!=interval.returnedDraws || interval.unresolvedWriters)
                    next->failure=interval.firstUnprovedReason?GenshinReturnedRaster::RefusalText(interval.firstUnprovedReason):"CLEAR_OR_ALL_WRITERS_UNPROVED";
                else {
                    // Actual DSV clear propagated through the selected source's
                    // returned copy chain. ResolvePrepared checks every real
                    // CB VP, W=1, instance domain, compare and viewport, proving
                    // one actual raster lens/rigid view. Newer Render pose/XY
                    // jitter is not source Z. Maximum donor is not Z proof.
                    const auto s=GenshinDepthSemantic::ResolvePrepared(interval.semanticWriters,interval.clearValue,
                        *frame_,parameters_,*camera_,&grid_);
                    next->failure=GenshinDepthSemantic::ReasonText(s.reason);
                    next->rasterX=s.rasterPixelsX;next->rasterY=s.rasterPixelsY;
                    next->submittedX=s.submittedPixelsX;next->submittedY=s.submittedPixelsY;next->clamped=s.submissionClamped;
                    retry=s.reason==GenshinDepthSemantic::Reason::Busy || s.reason==GenshinDepthSemantic::Reason::PayloadPending;
                    if(s.deviceZ && s.writers==interval.returnedDraws){
                        next->deviceZ=true;next->phase=s.phase;next->clear=s.clear;
                        next->convention=s.reversed?DepthConvention::DeviceZReversed:DepthConvention::DeviceZForward;
                        next->jx=s.depthToOutputUvX;next->jy=s.depthToOutputUvY;
                        next->rasterBasis=s.rasterBasis;next->rasterWorldToClip=s.rasterWorldToClip;next->rasterProjection=s.rasterProjection;
                    }else if(s.deviceZ)next->failure="INCOMPLETE_WRITER_MULTIPLICITY";
                }
            }
            proof=next;if(!retry)numerics_=std::move(next);
        }
        result.depthSemanticFailure=proof->failure;
        result.depthRasterPixelsX=proof->rasterX;result.depthRasterPixelsY=proof->rasterY;
        result.depthSubmittedPixelsX=proof->submittedX;result.depthSubmittedPixelsY=proof->submittedY;
        result.depthSubmissionClamped=proof->clamped;
        if(proof->deviceZ){result.depthConvention={proof->convention,Source::ActualRasterDepth};result.depthClear={proof->clear,Source::ActualRasterDepth};}
        if(proof->deviceZ && proof->phase){
            result.depthToOutputJitterX={proof->jx,Source::ActualDepthMapping};
            result.depthToOutputJitterY={proof->jy,Source::ActualDepthMapping};
        }
    }catch(...){result.depthSemanticFailure="SOURCE_NUMERIC_PROOF_ALLOCATION";}
}
std::shared_ptr<const PreparedDepthSource> Receipt::PreparedSourceForPresent()const noexcept {
    try {
        if(retired_.load(std::memory_order_acquire) || GetCurrentThreadId()!=facts_.owner.thread)return {};
        if(producer_)return producer_->PreparedSourceForPresent();
        if(preparedSource_)return preparedSource_->Matches(facts_)?preparedSource_:nullptr;
        if(!depthFrame_ || !depthCamera_ || !depthCamera_->valid || !SameOwner(depthCamera_->owner,facts_.owner) ||
           !lease_[1] || !facts_.producerBuildKnown || !facts_.engineSubrectKnown)return {};
        const auto& f=*depthFrame_;const auto& c=f.controls;
        const auto grid=OutputGrid();
        // Same accepted native producer/camera contract already used by FG.
        // No motion pair, game-motion readiness, FG switch or world-VS oracle.
        if(GenshinLegacy::NativeMrt::Validate(f)!=GenshinLegacy::NativeMrt::Reason::Ready ||
           !GenshinFsrProfiles::Accumulate(f.shader) || !f.nrProfile ||
           !f.nrProfile->MatchesShader(f.deviceEpoch,f.shaderIdentity,f.shader) || !grid ||
           c.depthFlipY || c.jitterDelay || c.jitterMode!=3 || !c.clampJitterHalf ||
           depthParameters_.originX || depthParameters_.originY ||
           grid->renderWidth!=f.renderWidth || grid->renderHeight!=f.renderHeight ||
           grid->outputWidth!=f.outputWidth || grid->outputHeight!=f.outputHeight ||
           grid->jitterPixelsX!=depthParameters_.jitterX || grid->jitterPixelsY!=depthParameters_.jitterY)return {};
        const float expectedX=std::clamp(.5f-f.rawJitterX*f.renderWidth,-.5f,.5f);
        const float expectedY=std::clamp(.5f-f.rawJitterY*f.renderHeight,-.5f,.5f);
        if(!GenshinCameraHistory::Near(expectedX,grid->jitterPixelsX,1.e-6f) ||
           !GenshinCameraHistory::Near(expectedY,grid->jitterPixelsY,1.e-6f))return {};
        auto next=std::shared_ptr<PreparedDepthSource>(new PreparedDepthSource);
        next->owner_=facts_.owner;
        if(!OwnerIdentity::Context(reinterpret_cast<ID3D11DeviceContext*>(f.context))
                .ToCanonical(facts_.owner,next->canonicalOwner_))return {};
        next->textures_=facts_.textures;next->provenance_=facts_.depthSource;next->prepared_=lease_[1];
        next->frame_=depthFrame_;next->camera_=depthCamera_;next->grid_=*grid;
        next->returnedRaster_=facts_.returnedRaster;next->parameters_=depthParameters_;
        // Accepted output -> prepared RASTER coordinates. FG's native-history
        // correction and DLSSG reflection remain separate consumer operations.
        next->outputToRasterX_=grid->jitterPixelsX/f.renderWidth;
        next->outputToRasterY_=grid->jitterPixelsY/f.renderHeight;
        if(!GenshinCameraHistory::Finite(next->outputToRasterX_) ||
           !GenshinCameraHistory::Finite(next->outputToRasterY_) || !next->Matches(facts_))return {};
        preparedSource_=next;return next;
    }catch(...){return {};}
}
std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> Receipt::NativeGuideForPresent()const noexcept {
    try {
        if(retired_.load(std::memory_order_acquire) || GetCurrentThreadId()!=facts_.owner.thread)return {};
        if(producer_)return producer_->NativeGuideForPresent();
        if(nativeGuide_)return nativeGuide_;
        GenshinGameMotion::Reference reference;std::optional<SrOutputGrid> grid;GenshinGuides::Owner canonicalOwner{};
        const auto refuse=[&](uint32_t mask)->std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> {
            static std::atomic<unsigned> reports{0};
            static std::atomic<uint32_t> previousMask{0};
            static std::atomic<unsigned> transitions{0};
            const auto report=reports.fetch_add(1,std::memory_order_relaxed);
            const bool maskChanged=previousMask.exchange(mask,std::memory_order_relaxed)!=mask;
            const bool boundedTransition=maskChanged && transitions.fetch_add(1,std::memory_order_relaxed)<64;
            if(report<16 || boundedTransition || report%256==0) {
                const auto* f=depthFrame_.get();const auto* n=f&&f->gameMotionReference?f->gameMotionReference.get():nullptr;
                const float preX=n&&f?.5f-n->rawJitterX_*f->renderWidth:0,preY=n&&f?.5f-n->rawJitterY_*f->renderHeight:0;
                LOG_INFO("R835 native guide refusal: pid={} generation={} source_frame={} mask={} camera={} grid={} reference={} phase={} output_uv={} native_reference={} validate={} cb28={},{} expected_jitter={},{} actual_jitter={},{} no_authority=true",
                    GetCurrentProcessId(),facts_.owner.generation,facts_.owner.sourceFrame,mask,bool(depthCamera_&&depthCamera_->valid),bool(grid),
                    reference.Known(canonicalOwner),reference.outputReferenceProved,reference.nativeOutputHistoryUv,bool(n),
                    f?static_cast<unsigned>(GenshinLegacy::NativeMrt::Validate(*f)):999u,n?n->cb28x_:0,n?n->cb28y_:0,preX,preY,
                    grid?grid->jitterPixelsX:0,grid?grid->jitterPixelsY:0);
            }
            return {};
        };
        uint32_t mask=(retired_.load(std::memory_order_acquire)?1u:0u)|(GetCurrentThreadId()!=facts_.owner.thread?2u:0u)|
            (!depthFrame_?4u:0u)|(!depthCamera_?8u:0u)|(depthCamera_&&!depthCamera_->valid?16u:0u)|
            (depthCamera_&&!SameOwner(depthCamera_->owner,facts_.owner)?32u:0u)|(!facts_.producerBuildKnown?64u:0u)|
            (!facts_.engineSubrectKnown?128u:0u)|(facts_.textures[3].identity==0?256u:0u);
        if(mask)return refuse(mask);
        if(!GenshinGuides::OwnerIdentity::Context(reinterpret_cast<ID3D11DeviceContext*>(depthFrame_->context)).ToCanonical(facts_.owner,canonicalOwner))return refuse(4096);
        grid=OutputGrid();reference=GameMotionReferenceForPresent();
        mask=(!grid?512u:0u)|(!reference.Known(canonicalOwner)?1024u:0u)|
            (GenshinLegacy::NativeMrt::Validate(*depthFrame_)!=GenshinLegacy::NativeMrt::Reason::Ready?2048u:0u);
        if(mask)return refuse(mask);
        const auto source=PreparedSourceForPresent();
        if(!source || !source->Matches(facts_) || !SameOwner(source->OwnerCanonical(),canonicalOwner))return refuse(8192);
        auto result=std::make_shared<GenshinGameMotion::NativeGuideBundle>();
        result->preparedSource=source;
        result->owner=canonicalOwner;result->frame=*depthFrame_;result->parameters=depthParameters_;
        result->camera=*depthCamera_;result->reference=reference;result->grid=*grid;result->textures=facts_.textures;
        result->provenance=Source::ActualNativeAccumulate;result->pictureIdentity=facts_.textures[3].identity;
        nativeGuide_=result;return result;
    }catch(...){return {};}
}

Facts Receipt::DepthFactsForPresent()const noexcept {
    Facts result=facts_;
    // Get() remains the original immutable observation. Even ready results are
    // not cached back, so retirement/loss never returns previously minted Z.
    if(retired_.load(std::memory_order_acquire) || (producer_ && producer_->retired_.load(std::memory_order_acquire))){
        static std::atomic<unsigned> retiredReports{0};
        if(retiredReports.fetch_add(1,std::memory_order_relaxed)<16)
            LOG_INFO("R827 retired depth facts: pid={} generation={} source_frame={} receipt={} parent={} self_retired={} parent_retired={} thread={} owner_thread={} no_authority=true",
                GetCurrentProcessId(),facts_.owner.generation,facts_.owner.sourceFrame,reinterpret_cast<uintptr_t>(this),
                reinterpret_cast<uintptr_t>(producer_.get()),retired_.load(std::memory_order_acquire),
                producer_ && producer_->retired_.load(std::memory_order_acquire),GetCurrentThreadId(),facts_.owner.thread);
        result.depthSemanticFailure="RETIRED_DEPTH_OWNER";return result;}
    // One source is shared regardless of FG enablement. Its resource
    // identity is independent from the cached, actual numeric proof. Neither
    // numerical-maximum donor nor camera endpoint labels authorize DeviceZ.
    if(const auto source=PreparedSourceForPresent();source && source->Matches(result)){
        result.preparedSource=source;source->ApplyNumerics(result);return result;
    }
    if(!result.returnedRaster){result.depthSemanticFailure="NO_SELECTED_RETURNED_RASTER";return result;}
    if(!depthCamera_ || !depthFrame_){result.depthSemanticFailure="NO_MATCHED_CAMERA_OR_FRAME";return result;}
    if(result.owner.thread!=GetCurrentThreadId() || !SameOwner(depthCamera_->owner,result.owner) ||
       !result.returnedRaster->Matches(*depthFrame_)){
        result.depthSemanticFailure="DEPTH_OWNER_OR_FRAME_MISMATCH";return result;}
    const auto& interval=result.returnedRaster->Get();
    if(!interval.clearReturned || !interval.semanticComplete || interval.coveredWriters!=interval.returnedDraws || interval.unresolvedWriters){
        result.depthSemanticFailure=interval.firstUnprovedReason?GenshinReturnedRaster::RefusalText(interval.firstUnprovedReason):"CLEAR_OR_ALL_WRITERS_UNPROVED";return result;}
    // Source/camera/output grid are the same immutable producer facts used by
    // FG. The numerical all-writer proof and final picture mapping remain NR's
    // independent qualifications. Native absence never makes FG a prerequisite.
    const auto native=NativeGuideForPresent();
    const auto grid=native?std::optional<SrOutputGrid>(native->grid):OutputGrid();
    if(native && (native->textures[1].identity!=result.textures[1].identity ||
       native->pictureIdentity!=result.textures[3].identity || !SameOwner(native->camera.owner,result.owner))){
        result.depthSemanticFailure="PRODUCER_SOURCE_CAMERA_GRID_CONFLICT";return result;
    }
    const auto semantic=GenshinDepthSemantic::Resolve(interval.semanticWriters,interval.clearValue,
        *depthFrame_,depthParameters_,*depthCamera_,grid?&*grid:nullptr);
    result.depthSemanticFailure=GenshinDepthSemantic::ReasonText(semantic.reason);
    if(semantic.deviceZ && semantic.writers!=interval.returnedDraws){result.depthSemanticFailure="INCOMPLETE_WRITER_MULTIPLICITY";return result;}
    result.depthRasterPixelsX=semantic.rasterPixelsX;result.depthRasterPixelsY=semantic.rasterPixelsY;
    result.depthSubmittedPixelsX=semantic.submittedPixelsX;result.depthSubmittedPixelsY=semantic.submittedPixelsY;
    result.depthSubmissionClamped=semantic.submissionClamped;
    if(semantic.deviceZ){
        result.depthConvention={semantic.reversed?DepthConvention::DeviceZReversed:DepthConvention::DeviceZForward,Source::ActualRasterDepth};
        result.depthClear={semantic.clear,Source::ActualRasterDepth};
    }
    if(semantic.deviceZ && semantic.phase){
        result.depthToOutputJitterX={semantic.depthToOutputUvX,Source::ActualDepthMapping};
        result.depthToOutputJitterY={semantic.depthToOutputUvY,Source::ActualDepthMapping};
    }return result;
}
std::shared_ptr<const Receipt> Receipt::WithOutputConsumer(std::shared_ptr<const Receipt> producer,
    const OutputCorrespondence& observed) {
    if(!producer || producer->retired_.load(std::memory_order_acquire) ||
       GetCurrentThreadId()!=producer->facts_.owner.thread)return {};
    const auto consumer=observed.observed.route.consumer;
    if(MatchOutput(observed,observed.observed,producer->facts_.owner,
        producer->facts_.textures[3].identity,consumer)!=CorrespondenceReason::None)return {};
    auto next=std::shared_ptr<Receipt>(new Receipt);
    next->facts_=producer->facts_;next->lease_=producer->lease_;
    next->depthCamera_=producer->depthCamera_;next->depthFrame_=producer->depthFrame_;
    next->depthParameters_=producer->depthParameters_;next->producer_=std::move(producer);
    next->facts_.outputConsumers[consumer==Consumer::NoiseReduction?0u:1u]=observed;
    return next;
}
Reason Receipt::Match(const Call& call)const noexcept {
    if(producer_){const auto reason=producer_->Match(call);if(reason!=Reason::None)return reason;}
    const auto& a=facts_.owner;const auto& b=call.owner;
    if(retired_.load(std::memory_order_acquire))return Reason::Retired;
    if(GetCurrentThreadId()!=a.thread||b.thread!=a.thread)return Reason::WrongThread;
    if(a.device!=b.device||a.context!=b.context||a.deviceEpoch!=b.deviceEpoch||a.producerEpoch!=b.producerEpoch||a.instance!=b.instance)return Reason::WrongOwner;
    if(a.configEpoch!=b.configEpoch)return Reason::ConfigChanged;
    if(a.historyEpoch!=b.historyEpoch||a.generation!=b.generation||a.sourceFrame!=b.sourceFrame)return Reason::StaleFrame;
    if(call.renderWidth!=facts_.renderWidth.value||call.renderHeight!=facts_.renderHeight.value)return Reason::WrongDimensions;
    for(unsigned i=0;i<call.resources.size();++i)if(Identity(call.resources[i])!=facts_.textures[i].identity)return Reason::ResourceMismatch;
    if(!call.temporalParametersRead)return Reason::MissingTemporalParameters;
    if(!std::isfinite(call.jitterX)||!std::isfinite(call.jitterY)||
       !std::isfinite(call.motionScaleX)||!std::isfinite(call.motionScaleY))return Reason::NonFinite;
    if(call.jitterX!=facts_.jitterX.value||call.jitterY!=facts_.jitterY.value||
       call.motionScaleX!=facts_.motionScaleX.value||call.motionScaleY!=facts_.motionScaleY.value)
        return Reason::TemporalParametersChanged;
    return Reason::None;
}
Reason Receipt::Prepare(const Call& call,Consumer consumer)const noexcept {
    if(auto reason=Match(call);reason!=Reason::None)return reason;
    const auto& f=facts_;
    if(f.depthConvention.source==Source::Unknown)return Reason::DepthConventionUnknown;
    if(f.colourDomain.source==Source::Unknown)return Reason::ColourDomainUnknown;
    if(!f.producerBuildKnown)return Reason::ProducerBuildUnknown;
    if(f.motionDirection.source==Source::Unknown)return Reason::MotionDirectionUnknown;
    const bool baseNr=f.nrProfile && f.colourDomain.value==ColourDomain::NativeBounded &&
        consumer==Consumer::NoiseReduction && call.outputConsumer.route.point==PicturePoint::AfterUpscale &&
        call.outputConsumer.route.api==ConsumerApi::BridgeDx12;
    if(f.colourDomain.value==ColourDomain::NativeBounded && !baseNr)return Reason::ColourDomainUnknown;
    if(!baseNr && f.jitterUnits.source==Source::Unknown)return Reason::JitterUnitsUnknown;
    if(!f.engineSubrectKnown)return Reason::SubrectUnknown;
    const auto index=consumer==Consumer::NoiseReduction?0u:1u;
    if(MatchOutput(f.outputConsumers[index],call.outputConsumer,f.owner,f.textures[3].identity,consumer)!=CorrespondenceReason::None)
        return Reason::OutputConsumerUnknown;
    return Reason::None;
}
Reason Receipt::Admit(const Call& call,Consumer consumer,bool evaluated)const noexcept {
    if(auto reason=Prepare(call,consumer);reason!=Reason::None)return reason;
    return evaluated?Reason::None:Reason::EvaluateUnconfirmed;
}
const char* ReasonText(Reason reason)noexcept {
    constexpr const char* names[]={"matched typed guides","missing same-call guide receipt","guide owning thread changed","guide producer/device/context/instance changed","guide frame/generation changed","guide configuration epoch changed","guide receipt retired","guide resource identity changed","guide dimensions changed","unsupported guide format","non-finite guide value","current engine producer build/profile unverified","colour domain unknown","depth convention unknown","motion direction unknown","jitter units unknown","engine subrect unknown","source-to-output consumer unverified","actual Evaluate unconfirmed","missing actual NGX jitter or motion scale","actual NGX jitter or motion scale changed"};
    const auto index=static_cast<unsigned>(reason);return index<std::size(names)?names[index]:"invalid guide reason";
}
static bool ContractUnverified(Reason reason)noexcept {
    switch(reason) {
    case Reason::ProducerBuildUnknown: case Reason::ColourDomainUnknown: case Reason::DepthConventionUnknown:
    case Reason::MotionDirectionUnknown: case Reason::JitterUnitsUnknown: case Reason::SubrectUnknown:
    case Reason::OutputConsumerUnknown: return true;
    default: return false;
    }
}
static std::string ContractDetail(const Facts& facts) {
    std::string detail="Unverified guide evidence:";
    const auto missing=[&](bool unknown,const char* label) {
        if(unknown){detail+=" ";detail+=label;detail+=";";}
    };
    missing(facts.depthConvention.source==Source::Unknown,"depth convention");
    missing(facts.colourDomain.source==Source::Unknown,"colour domain");
    missing(!facts.producerBuildKnown,"producer build/profile");
    missing(facts.motionDirection.source==Source::Unknown,"motion direction");
    missing(facts.jitterUnits.source==Source::Unknown,"jitter units");
    missing(!facts.engineSubrectKnown,"engine subrect");
    missing(facts.outputConsumers[0].source!=CorrespondenceSource::ActualConsumerObservation ||
        facts.outputConsumers[1].source!=CorrespondenceSource::ActualConsumerObservation,"source-to-output consumer");
    return detail;
}
static void PublishRejectedImpl(const Owner& owner,Consumer consumer,Reason reason,const Facts* facts)noexcept {
    if(!owner.context||reason==Reason::None)return;
    constexpr unsigned reasonCount=static_cast<unsigned>(Reason::TemporalParametersChanged)+1;
    const auto reasonIndex=static_cast<unsigned>(reason);
    if(reasonIndex>=reasonCount)return;
    static std::array<std::atomic<uint64_t>,2*reasonCount> rejectedCounts{};
    const unsigned consumerIndex=consumer==Consumer::NoiseReduction?0:1;
    const auto occurrence=rejectedCounts[consumerIndex*reasonCount+reasonIndex].fetch_add(1,std::memory_order_relaxed)+1;
    // Count per consumer/reason, not a per-frame producer epoch. Build diagnostics only when sampled.
    if(occurrence!=1 && occurrence%300!=0)return;
    try {
        const bool unverified=ContractUnverified(reason);
        // Inspect only this immutable receipt. Requested controls do not establish engine semantics.
        const auto detail=unverified && facts?ContractDetail(*facts):std::string{};
        LOG_WARN("Genshin {} guide rejected: {}; config epoch {}; producer epoch {}; occurrence {}; {}",
            consumer==Consumer::NoiseReduction?"NR":"FG", ReasonText(reason),
            owner.configEpoch,owner.producerEpoch,occurrence,detail);
    }catch(...){} // Diagnostics do not change supplier/draw return behavior.
}
void PublishRejected(const Receipt* receipt,Consumer consumer,Reason reason)noexcept {
    if(!receipt||reason==Reason::None)return;
    const auto& facts=receipt->Get();
    PublishRejectedImpl(facts.owner,consumer,reason,&facts);
}
void PublishRejected(const Owner& owner,Consumer consumer,Reason reason)noexcept {
    PublishRejectedImpl(owner,consumer,reason,nullptr);
}
}
