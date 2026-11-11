// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone CPU regression source. Lead owns compilation and execution.
#include <shaders/dlssnr/GuideMetadata.h>
#include <shaders/dlssnr/DlssNr_NativeGuidePhase.h>
#include <runtime/GuideDiagnostics.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>

int main() {
    using namespace DlssNr::GuideMetadata;
    unsigned base=13;
    assert(OptionalBase(base,true,false) && base==13);
    assert(OptionalBase(base,false,false) && base==0);
    base=13;
    assert(!OptionalBase(base,false,true) && base==13); // no borrowed DLSS defaults for RR
    base=~0u;
    assert(OptionalBase(base,true,false) && base==~0u); // bounds must reject, never clamp
    unsigned w=640,h=360;
    assert(RenderExtent(w,h,true,true,1920,1080,true) && w==640 && h==360);
    w=640;h=0;
    assert(RenderExtent(w,h,true,false,800,450,true) && w==800 && h==450);
    w=0;h=360;
    assert(!RenderExtent(w,h,true,true,1920,1080,true)); // supplied zero remains invalid
    assert(!RenderExtent(w,h,true,false,1920,1080,true));
    w=0;h=0;
    assert(!RenderExtent(w,h,false,false,1920,1080,false));
    assert(RenderExtent(w,h,false,false,1920,1080,true) && w==1920 && h==1080);
    Provenance provenance;
    provenance.Set(Field::DepthX,0,true,Origin::ApiDefault);
    provenance.Set(Field::ScaleX,1,false,Origin::ApiDefault);
    assert(provenance.fields[3].valid && provenance.fields[3].origin==Origin::ApiDefault);
    assert(!provenance.fields[7].valid && provenance.fields[7].origin==Origin::Unknown);

    DlssNr::NativeGuidePhase::Inputs phase;
    phase.nativeRoute=phase.baseIdentity=phase.currentDepth=phase.routeConvention=true;
    phase.metadataKnown=phase.flagsKnown=phase.jitterKnown=phase.motionJitterKnown=true;
    phase.motionLowResolution=phase.normalizedTexturesReady=true;
    phase.renderWidth=phase.depthWidth=phase.motionWidth=640;
    phase.renderHeight=phase.depthHeight=phase.motionHeight=360;
    phase.scaleX=phase.decodedScaleX=1;phase.scaleY=1;phase.decodedScaleY=-1;
    phase.depthRowsFlip=phase.motionRowsFlip=phase.vectorYNegated=true;
    phase.jitterX=.25f;phase.jitterY=-.5f;
    const auto admitted=DlssNr::NativeGuidePhase::Resolve(phase);
    assert(admitted.verified && admitted.motionX==.25f/640 && admitted.depthY==-.5f/360);
    assert(admitted.cancelX==0 && admitted.cancelY==0);
    phase.normalizedTexturesReady=false;
    assert(!DlssNr::NativeGuidePhase::Resolve(phase).verified);
    phase.normalizedTexturesReady=true;phase.motionJittered=true;
    assert(!DlssNr::NativeGuidePhase::Resolve(phase).verified);
    phase.motionJittered=false;phase.metadataKnown=false;
    assert(!DlssNr::NativeGuidePhase::Resolve(phase).verified);
    phase.metadataKnown=true;phase.decodedScaleY=1;
    assert(!DlssNr::NativeGuidePhase::Resolve(phase).verified);
    phase.decodedScaleY=-1;phase.jitterX=std::numeric_limits<float>::quiet_NaN();
    assert(!DlssNr::NativeGuidePhase::Resolve(phase).verified);

    GuideDiagnostics::Snapshot model;
    model.source=4;model.epoch=4;model.tick=10;model.modelSuccess=true;
    model.stage=GuideDiagnostics::Stage::Recorded;
    GuideDiagnostics::Publish(GuideDiagnostics::Consumer::NrModel,model);
    auto history=model;
    history.stage=GuideDiagnostics::Stage::Rejected;history.reason="source-gap";
    history.historySource=2;history.fusionRecorded=false;
    GuideDiagnostics::Publish(GuideDiagnostics::Consumer::NrHistory,history);
    GuideDiagnostics::View view;
    assert(GuideDiagnostics::Read(view));
    assert(view.lanes[0].latest.modelSuccess && !view.lanes[1].latest.fusionRecorded);
    assert(!view.lanes[1].latest.contributionKnown && view.lanes[1].latest.contribution==-1);
    model.stage=GuideDiagnostics::Stage::Completed;
    GuideDiagnostics::Publish(GuideDiagnostics::Consumer::NrModel,model);
    assert(GuideDiagnostics::Stale(model,2000)); // completion cannot refresh source age
    auto newer=model;newer.source=5;newer.tick=20;newer.stage=GuideDiagnostics::Stage::Received;
    GuideDiagnostics::Publish(GuideDiagnostics::Consumer::NrModel,newer);
    GuideDiagnostics::Publish(GuideDiagnostics::Consumer::NrModel,model);
    assert(GuideDiagnostics::Read(view));
    assert(view.lanes[0].latest.source==5 && view.lanes[0].completed.source==4);
}
