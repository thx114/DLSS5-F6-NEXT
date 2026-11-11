// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/GuideSourceMetadata.h>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace GuideSourceMetadata;
using F=DlssNr::GuideMetadata::Field;
using O=DlssNr::GuideMetadata::Origin;
Descriptor Fixture(){
    Descriptor d;d.feature=10;d.context=11;d.generation=12;d.tick=100;
    d.inputDepth=20;d.inputMotion=21;d.translatedDepth=30;d.translatedMotion=31;
    d.flags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;d.flagsKnown=d.jitterKnown=d.lowRes=true;
    d.renderWidth=640;d.renderHeight=360;d.outputWidth=1280;d.outputHeight=720;
    d.depth={true,41,672,400,{16,20,640,360}};d.motion={true,16,800,400,{24,8,640,360}};
    d.scaleX=1;d.scaleY=-1;d.jitterX=.25f;d.jitterY=-.25f;
    const double values[]={double(d.flags),640,360,16,20,24,8,1,-1,.25,-.25,1,1};
    for(unsigned i=0;i<d.provenance.fields.size();++i)d.provenance.Set(static_cast<F>(i),values[i],true,O::Evaluate);
    return d;
}
int main(){try{
    unsigned checks=0;const auto check=[&](bool b){++checks;if(!b)throw std::runtime_error("raw metadata invariant");};
    auto a=Fixture();check(Valid(a));auto b=a;b.tick=200;b.evaluateFrame=97;
    // Each source may use a different sample jitter and different allocation identity.
    b.jitterX=-.25f;b.provenance.Set(F::JitterX,-.25,true,O::Evaluate);b.inputDepth=40;b.translatedDepth=50;
    check(Compatible(a,b,300));check(!Compatible(a,b,1701));check(!Compatible(a,b,99));
    for(unsigned i=0;i<a.provenance.fields.size();++i){
        auto x=a;x.provenance.fields[i].valid=false;check(!Valid(x));
        x=a;x.provenance.fields[i].origin=O::Unknown;check(!Valid(x));
        x=a;x.provenance.fields[i].value=std::numeric_limits<double>::quiet_NaN();check(!Valid(x));
    }
    for(unsigned i=0;i<3;++i){auto x=a;
        if(i==0)++x.feature;else if(i==1)++x.context;else ++x.generation;check(!Compatible(a,x,300));}
    auto x=a;x.lowRes=false;check(!Valid(x));x=a;x.jittered=true;check(!Valid(x));
    x=a;x.depthInverted=true;check(!Valid(x));x=a;x.motion.region.x=~0u;check(!Valid(x));
    x=a;x.scaleY=1;check(!Valid(x));x.provenance.Set(F::ScaleY,1,true,O::Evaluate);
    check(Valid(x)&&!Compatible(a,x,300));
    x=a;x.provenance.Set(F::DepthX,16,true,O::ApiDefault);check(!Compatible(a,x,300));
    x=a;x.motion.region.width=1280;check(!Valid(x));
    const auto sx=a.scaleX/640,sy=a.scaleY/360;
    check(AcceptedMapping(a,a.depth,a.motion,sx,sy));
    check(!AcceptedMapping(a,a.depth,a.motion,sx,-sy)); // double sign is not accepted
    auto converted=a.motion;converted.format=34;check(!AcceptedMapping(a,a.depth,converted,sx,sy));
    auto region=a.depth;region.region.y++;check(!AcceptedMapping(a,region,a.motion,sx,sy));
    x=a;x.inputMotion=0;check(!Valid(x));x=a;x.translatedDepth=0;check(!Valid(x));
    x=a;x.jitterX=.75f;x.provenance.Set(F::JitterX,.75,true,O::Evaluate);check(!Valid(x));
    x=a;x.flags&=~NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;x.lowRes=false;
    x.provenance.Set(F::CreateFlags,x.flags,true,O::RetainedFeature);
    x.motion={true,16,1344,760,{24,8,1280,720}};check(Valid(x));
    check(AcceptedMapping(x,x.depth,x.motion,x.scaleX/1280,x.scaleY/720));
    // These helpers never advance consumer observations or completion.
    GuideDiagnostics::View view;check(GuideDiagnostics::Read(view));
    for(const auto& lane:view.lanes)check(lane.observations==0&&lane.completed.tick==0);
    std::printf("GuideSourceMetadata: %u checks passed\n",checks);return 0;
}catch(const std::exception& e){std::puts(e.what());return 1;}}
