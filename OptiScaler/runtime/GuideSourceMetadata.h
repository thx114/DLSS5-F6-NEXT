// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GuideDiagnostics.h"
#include <shaders/dlssnr/GuideMetadata.h>
#include <nvsdk_ngx_defs.h>
#include <cmath>
#include <memory>

// Observation only: numeric identities never own resources or admit GPU work.
namespace GuideSourceMetadata {
struct Descriptor {
    uint64_t feature=0,context=0,generation=0,tick=0;
    uint64_t inputDepth=0,inputMotion=0; // actual API-boundary resources, not capture copies
    uint64_t translatedDepth=0,translatedMotion=0,evaluateFrame=0;
    DlssNr::GuideMetadata::Provenance provenance;
    GuideDiagnostics::Resource depth,motion;
    unsigned flags=0,renderWidth=0,renderHeight=0,outputWidth=0,outputHeight=0;
    float scaleX=0,scaleY=0,jitterX=0,jitterY=0;
    bool flagsKnown=false,jitterKnown=false,lowRes=false,jittered=false,depthInverted=false,rr=false;
};
using Ptr=std::shared_ptr<const Descriptor>;
inline bool SameMapping(const GuideDiagnostics::Resource& a,const GuideDiagnostics::Resource& b) {
    return a.present&&b.present&&a.format==b.format&&a.width==b.width&&a.height==b.height&&
        a.region.x==b.region.x&&a.region.y==b.region.y&&a.region.width==b.region.width&&a.region.height==b.region.height;
}
inline bool Bounds(const GuideDiagnostics::Resource& r) {
    return r.present&&r.region.width&&r.region.height&&r.region.x<=r.width&&r.region.y<=r.height&&
        r.region.width<=r.width-r.region.x&&r.region.height<=r.height-r.region.y;
}
inline bool Valid(const Descriptor& d) {
    using F=DlssNr::GuideMetadata::Field;
    using O=DlssNr::GuideMetadata::Origin;
    for(const auto& v:d.provenance.fields)
        if(!v.valid||v.origin==O::Unknown||!std::isfinite(v.value))return false;
    const auto eq=[&](F f,double v){return d.provenance.fields[static_cast<unsigned>(f)].value==v;};
    return d.feature&&d.context&&d.generation&&d.tick&&d.inputDepth&&d.inputMotion&&
        d.translatedDepth&&d.translatedMotion&&d.flagsKnown&&d.jitterKnown&&
        d.lowRes==((d.flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)!=0)&&
        d.jittered==((d.flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0)&&
        d.depthInverted==((d.flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0)&&
        d.depth.format&&d.motion.format&&
        d.renderWidth&&d.renderHeight&&d.outputWidth&&d.outputHeight&&Bounds(d.depth)&&Bounds(d.motion)&&
        d.scaleX!=0&&d.scaleY!=0&&std::isfinite(d.scaleX)&&std::isfinite(d.scaleY)&&
        std::isfinite(d.jitterX)&&std::isfinite(d.jitterY)&&std::abs(d.jitterX)<=.5f&&std::abs(d.jitterY)<=.5f&&
        eq(F::CreateFlags,d.flags)&&eq(F::RenderWidth,d.renderWidth)&&eq(F::RenderHeight,d.renderHeight)&&
        eq(F::DepthX,d.depth.region.x)&&eq(F::DepthY,d.depth.region.y)&&
        eq(F::MotionX,d.motion.region.x)&&eq(F::MotionY,d.motion.region.y)&&
        eq(F::ScaleX,d.scaleX)&&eq(F::ScaleY,d.scaleY)&&eq(F::JitterX,d.jitterX)&&eq(F::JitterY,d.jitterY)&&
        eq(F::DepthBounds,1)&&eq(F::MotionBounds,1)&&
        d.depth.region.width==d.renderWidth&&d.depth.region.height==d.renderHeight&&
        d.motion.region.width==(d.lowRes?d.renderWidth:d.outputWidth)&&
        d.motion.region.height==(d.lowRes?d.renderHeight:d.outputHeight);
}
// Jitter belongs to each sample; compare its provenance, not different frame phases.
inline bool Compatible(const Descriptor& a,const Descriptor& b,uint64_t now) {
    if(!Valid(a)||!Valid(b)||now<a.tick||now<b.tick||now-a.tick>1500||now-b.tick>1500||
        a.feature!=b.feature||a.context!=b.context||a.generation!=b.generation||a.rr!=b.rr||
        a.flags!=b.flags||a.lowRes!=b.lowRes||a.jittered!=b.jittered||a.depthInverted!=b.depthInverted||
        a.outputWidth!=b.outputWidth||a.outputHeight!=b.outputHeight||
        !SameMapping(a.depth,b.depth)||!SameMapping(a.motion,b.motion))return false;
    using F=DlssNr::GuideMetadata::Field;
    for(unsigned i=0;i<a.provenance.fields.size();++i){
        const auto& x=a.provenance.fields[i];const auto& y=b.provenance.fields[i];
        if(x.origin!=y.origin)return false;
        if(i!=static_cast<unsigned>(F::JitterX)&&i!=static_cast<unsigned>(F::JitterY)&&x.value!=y.value)return false;
    }
    return true;
}
inline bool AcceptedMapping(const Descriptor& d,const GuideDiagnostics::Resource& depth,
    const GuideDiagnostics::Resource& motion,float sdkUvX,float sdkUvY) {
    return Valid(d)&&SameMapping(d.depth,depth)&&SameMapping(d.motion,motion)&&
        std::isfinite(sdkUvX)&&std::isfinite(sdkUvY)&&
        sdkUvX==d.scaleX/static_cast<float>(motion.region.width)&&
        sdkUvY==d.scaleY/static_cast<float>(motion.region.height);
}
} // namespace GuideSourceMetadata
