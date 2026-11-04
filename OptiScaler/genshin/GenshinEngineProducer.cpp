// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinEngineProducer.h"
#include "legacy/Fsr2FamilyTakeover.h"
#include <cmath>
#include <limits>

namespace GenshinLegacy::Engine
{
void Producer::Bind(ID3D11Device* device,ID3D11DeviceContext* context,std::uint64_t epoch,std::uint64_t owner,std::uint64_t thread) noexcept
{
    Reset();device_=reinterpret_cast<uintptr_t>(device);context_=reinterpret_cast<uintptr_t>(context);
    deviceEpoch_=epoch;ownerEpoch_=owner;thread_=thread;
}
bool Producer::Observe(const il2cpp_callsite::RenderToken& token,const CaptureSource& source,NoSrCapture::Signal& signal) noexcept
{
    signal={};receipt_={};
    if(!device_ || !context_ || !deviceEpoch_ || !ownerEpoch_ || !thread_ ||
        device_!=reinterpret_cast<uintptr_t>(source.device) || context_!=reinterpret_cast<uintptr_t>(source.context) ||
        deviceEpoch_!=source.deviceEpoch || ownerEpoch_!=source.ownerEpoch || thread_!=source.ownerThread ||
        thread_!=GetCurrentThreadId()){receipt_.reason=Reason::WrongOwner;return false;}
    receipt_.ownerEpoch=ownerEpoch_;receipt_.shaderHash=source.shaderHash;receipt_.shaderIdentity=source.shaderIdentity;
    receipt_.resources={reinterpret_cast<uintptr_t>(source.resources[0]),reinterpret_cast<uintptr_t>(source.resources[1]),reinterpret_cast<uintptr_t>(source.resources[2])};
    receipt_.srvSlots=source.srvSlots;
    if(source.shaderHash!=fsr2_family_takeover::k_accumulate_hash || !source.shaderIdentity || source.shaderDeviceEpoch!=deviceEpoch_)
    {receipt_.reason=Reason::UnknownPass;return false;}
    receipt_.knownLegacyPass=true;
    if(!source.resources[0] || !source.resources[1] || !source.resources[2] ||
        source.srvSlots!=std::array<std::uint32_t,3>{0,2,3} ||
        source.viewFormats[2]!=DXGI_FORMAT_R10G10B10A2_UNORM)
    {receipt_.reason=Reason::UnsupportedViews;return false;}
    D3D11_TEXTURE2D_DESC depth{};source.resources[1]->GetDesc(&depth);
    const bool readableDepth=(source.viewFormats[1]==DXGI_FORMAT_R32_FLOAT &&
        (depth.Format==DXGI_FORMAT_R32_FLOAT || depth.Format==DXGI_FORMAT_R32_TYPELESS)) ||
        (source.viewFormats[1]==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS && depth.Format==DXGI_FORMAT_R32G8X24_TYPELESS);
    if(!readableDepth || depth.MipLevels!=1 || depth.ArraySize!=1 || depth.SampleDesc.Count!=1){
        receipt_.reason=Reason::UnsupportedViews;return false;}
    const auto& p=token.params;
    if(!p.instance || !p.frame_index || !token.generation || !token.capture_tick || !p.render_w || !p.render_h ||
        !p.display_w || !p.display_h || !std::isfinite(p.jitter_x) || !std::isfinite(p.jitter_y))
    {receipt_.reason=Reason::MissingFrame;return false;}
    History* history=nullptr;
    for(auto& entry:histories_)if(entry.instance==p.instance){history=&entry;break;}
    if(!history)for(auto& entry:histories_)if(!entry.instance){history=&entry;break;}
    if(!history){receipt_.reason=Reason::InstanceCapacity;return false;}
    if(history->instance && (token.generation<=history->generation || p.frame_index<=history->frame))
    {receipt_.reason=Reason::DuplicateOrOutOfOrder;return false;}
    signal.producer=NoSrCapture::Producer::EngineMv;signal.confidence=NoSrCapture::Confidence::Candidate;
    signal.producerId=source.shaderHash;signal.device=device_;signal.context=context_;signal.ownerThread=thread_;signal.instance=p.instance;
    signal.deviceEpoch=deviceEpoch_;signal.producerEpoch=ownerEpoch_;
    // The Render generation is a source observation, not proof of each texture's last write.
    // Unknown swapchain/resource-write generation deliberately prevents admission.
    signal.renderedFrame=signal.sourceFrame=p.frame_index;signal.submissionSerial=token.generation;
    signal.nativeSize={p.render_w,p.render_h};signal.outputSize={p.display_w,p.display_h};
    signal.colorRect=signal.depthRect=signal.motionRect={0,0,p.render_w,p.render_h};
    signal.jitterX=p.jitter_x;signal.jitterY=p.jitter_y;
    const bool contiguous=history->instance && history->frame<(std::numeric_limits<std::uint32_t>::max)() && p.frame_index==history->frame+1;
    if(contiguous){signal.previousJitterX=history->jitterX;signal.previousJitterY=history->jitterY;}
    signal.depthFormat=source.viewFormats[1];signal.motionFormat=source.viewFormats[2];
    *history={p.instance,token.generation,p.frame_index,p.jitter_x,p.jitter_y};
    receipt_.reason=Reason::CandidateSemanticsIncomplete;receipt_.candidate=signal;receipt_.sourceGeneration=token.generation;
    receipt_.actualRenderWitness=true;receipt_.rawJitterFinite=true;receipt_.previousRawJitterContiguous=contiguous;
    receipt_.motionEncoding=MotionEncoding::LegacySignedSquare127Over255;
    receipt_.sourceWrites=source.sourceWrites;receipt_.outputIdentity=source.outputIdentity;
    // Explicitly no colourKnown/depthKnown/motionKnown/jitterKnown claims.

    // Capture-only observations grant no depth convention. The actual claimed
    // NativeMrt transaction independently supplies the optional raw preview.

    return true;
}
bool Producer::Publish(const il2cpp_callsite::RenderToken& token,const CaptureSource& source,NoSrCapture::Signal& signal,void* opaque) noexcept
{return opaque && static_cast<Producer*>(opaque)->Observe(token,source,signal);}
void Producer::Reset() noexcept
{histories_={};receipt_={};receipt_.reason=Reason::Reset;device_=context_=deviceEpoch_=ownerEpoch_=thread_=0;}
void Producer::AfterOriginalDraw(std::uint64_t generation) noexcept
{if(receipt_.actualRenderWitness && receipt_.sourceGeneration==generation)receipt_.originalDrawReturned=true;}
void Producer::NotePresent(const Presentation::Receipt& present,const ResourceProof::Witness& target,bool sameRender) noexcept
{
    receipt_.observedSwapchain=present.ticket.swapchain;receipt_.observedPresentSerial=present.completionSerial;
    receipt_.observedPresentResult=present.rawResult;receipt_.presentedBackbuffer=target;
    const auto& output=receipt_.outputIdentity;
    receipt_.directOutputMatchesBackbuffer=receipt_.originalDrawReturned && sameRender && present.recorded && present.successfulNonTest &&
        ResourceProof::CurrentIdentityPair(output,target) &&
        target.resourceLifetime && target.resourceLifetime==output.resourceLifetime && target.deviceEpoch==output.deviceEpoch &&
        target.observationEpoch==output.observationEpoch && target.subresourceKnown && output.subresourceKnown;
    // An intermediate output, uncovered mutations or absent semantic/build proof never closes the route.
    receipt_.sourceToTargetProven=false;
}
const char* ReasonText(Reason reason) noexcept
{
    constexpr const char* names[]{"unobserved","legacy FSR candidate; colour/depth/motion/jitter semantics incomplete","wrong owner",
        "unknown FSR pass","unsupported source views","missing actual Render frame","duplicate or out-of-order Render frame","eight producer instances occupied","producer reset"};
    return names[static_cast<unsigned>(reason)];
}
}
