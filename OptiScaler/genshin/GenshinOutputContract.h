// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace GenshinGuides {
enum class Consumer : unsigned char { NoiseReduction, FrameGeneration };
struct Owner {
    uint64_t device=0,context=0,deviceEpoch=0,producerEpoch=0,configEpoch=0,instance=0,generation=0,historyEpoch=0;
    uint32_t sourceFrame=0,thread=0;
};
// Private vocabulary: never cast State's enums into this contract.
enum class PicturePoint : unsigned char { Unknown, BeforeUpscale, AfterUpscale, Present };
enum class ConsumerApi : unsigned char { Unknown, NativeDx11, BridgeDx12 };
enum class FgInput : unsigned char { Unknown, NoFG, Upscaler, DLSSG, NvngxFG, FSRFG, FSRFG30, XeFG };
enum class FgOutput : unsigned char { Unknown, NoFG, FSRFG, DLSSG, XeFG };
enum class CorrespondenceSource : unsigned char { Unknown, RequestedControl, ActualConsumerObservation };
enum class CorrespondenceReason : unsigned char { None, Unknown, InvalidRoute, RouteChanged, OwnerChanged, ResourceChanged };
struct ConsumerRoute {
    Consumer consumer=Consumer::NoiseReduction;
    PicturePoint point=PicturePoint::Unknown;
    ConsumerApi api=ConsumerApi::Unknown;
    FgInput input=FgInput::Unknown;
    FgOutput output=FgOutput::Unknown;
};
struct OutputRequest {
    ConsumerRoute route;
    uint64_t destination=0,consumerDevice=0,consumerContext=0;
};
struct OutputCorrespondence {
    CorrespondenceSource source=CorrespondenceSource::Unknown;
    Owner owner;
    uint64_t sourceOutput=0;
    OutputRequest observed;
};
inline bool SameOwner(const Owner& a,const Owner& b) noexcept {
    return a.device==b.device && a.context==b.context && a.deviceEpoch==b.deviceEpoch &&
        a.producerEpoch==b.producerEpoch && a.configEpoch==b.configEpoch && a.instance==b.instance &&
        a.generation==b.generation && a.historyEpoch==b.historyEpoch && a.sourceFrame==b.sourceFrame && a.thread==b.thread;
}
inline bool ValidRoute(const ConsumerRoute& r) noexcept {
    if(r.api!=ConsumerApi::NativeDx11 && r.api!=ConsumerApi::BridgeDx12)return false;
    if(r.point!=PicturePoint::BeforeUpscale && r.point!=PicturePoint::AfterUpscale && r.point!=PicturePoint::Present)return false;
    if(r.consumer==Consumer::NoiseReduction)return r.input==FgInput::NoFG && r.output==FgOutput::NoFG;
    // Genshin legacy FSR2 is not a native FSR3/DLSSG/XeFG input producer.
    return r.consumer==Consumer::FrameGeneration && r.point!=PicturePoint::BeforeUpscale &&
        r.api==ConsumerApi::BridgeDx12 && r.input==FgInput::Upscaler &&
        (r.output==FgOutput::FSRFG || r.output==FgOutput::DLSSG || r.output==FgOutput::XeFG);
}
inline bool SameRoute(const ConsumerRoute& a,const ConsumerRoute& b) noexcept {
    return a.consumer==b.consumer && a.point==b.point && a.api==b.api && a.input==b.input && a.output==b.output;
}
// Correspondence only. This does not prove guide semantics, queue ordering,
// live leases, Evaluate/GPU completion, supplier acceptance or presentation.
inline CorrespondenceReason MatchOutput(const OutputCorrespondence& proof,const OutputRequest& request,
    const Owner& current,uint64_t sourceOutput,Consumer consumer) noexcept {
    if(proof.source!=CorrespondenceSource::ActualConsumerObservation || !proof.sourceOutput ||
       !proof.observed.destination || !proof.observed.consumerDevice || !proof.observed.consumerContext)
        return CorrespondenceReason::Unknown;
    if(!ValidRoute(proof.observed.route) || !ValidRoute(request.route))return CorrespondenceReason::InvalidRoute;
    if(request.route.consumer!=consumer || !SameRoute(proof.observed.route,request.route))return CorrespondenceReason::RouteChanged;
    if(!current.device || !current.context || !current.deviceEpoch || !current.producerEpoch || !current.configEpoch ||
       !current.instance || !current.generation || !current.historyEpoch || !current.thread || !SameOwner(proof.owner,current))
        return CorrespondenceReason::OwnerChanged;
    if(!sourceOutput || proof.sourceOutput!=sourceOutput || proof.observed.destination!=request.destination ||
       proof.observed.consumerDevice!=request.consumerDevice || proof.observed.consumerContext!=request.consumerContext)
        return CorrespondenceReason::ResourceChanged;
    return CorrespondenceReason::None;
}
}
