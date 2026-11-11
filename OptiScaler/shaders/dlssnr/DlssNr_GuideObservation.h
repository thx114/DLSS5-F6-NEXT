// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DlssNr_FrameInfo.h"
#include "DlssNr_RecordingLifetime.h"
#include <runtime/GuideCapture.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

// All callers hold the NR mutex. These observations never grant admission or
// retire GPU resources. The existing recording token is the completion witness.
namespace DlssNrGuideObservation {
static_assert(static_cast<unsigned>(DlssNr::GuideMetadata::Field::MotionBounds)==
    static_cast<unsigned>(GuideDiagnostics::Field::MotionBounds));
static_assert(static_cast<unsigned>(DlssNr::GuideMetadata::Origin::ActualCall)==
    static_cast<unsigned>(GuideDiagnostics::Origin::ActualCall));
inline GuideDiagnostics::Resource Resource(ID3D12Resource* r,GuideDiagnostics::Rect rect) {
    GuideDiagnostics::Resource result;
    if (r) { const auto d=r->GetDesc(); result={true,static_cast<uint32_t>(d.Format),d.Width,d.Height,rect}; }
    return result;
}
inline bool RawRasterAddressKnown(const DlssNrFrameInfo& f) {
    using Field=DlssNr::GuideMetadata::Field;
    using Origin=DlssNr::GuideMetadata::Origin;
    const auto known=[&](Field field,double actual){
        const auto& v=f.GuideProvenance.fields[static_cast<unsigned>(field)];
        return v.valid&&v.origin!=Origin::Unknown&&std::isfinite(v.value)&&v.value==actual;
    };
    // Late's diagnostic copy must FIRST populate the actual selected picture's
    // base/route contract. Source(slot.frame) before that seam stays unknown.
    // The NR PrepareSceneGuides shader uses uv+J/render
    // BEFORE row reflection. Declare that address contract from source metadata,
    // not NativeGuidePhaseVerified (which includes the LIVE direction overrides
    // being calibrated). This observation grants no input/lifetime authority.
    return f.FinishedPicture&&!f.BeforeUpscale&&!f.FreshColorResetFallback&&f.IndependentCommands&&
        f.NativeSceneRoute&&
        f.NativeBaseIdentityVerified&&f.NativeCurrentDepthVerified&&f.NativeRouteConventionVerified&&
        f.NativePresentQueueIdentity&&
        !f.MotionIsPictureFlow&&!f.DepthIsPictureAligned&&f.SourceCaptureSerial&&f.SourceCaptureEpoch&&
        f.SourceCaptureSerial!=UINT64_MAX&&f.SourceCaptureEpoch!=UINT64_MAX&&
        (f.SceneGuideOwner[0]||(f.SceneGuideOwner[1]&&f.SceneGuideOwner[2]))&&
        f.NativeFeatureFlagsKnown&&f.NativeGuideMetadataKnown&&f.NativeCurrentJitterKnown&&
        f.NativeMotionJitterKnown&&!f.NativeMotionVectorsJittered&&f.MotionVectorsLowResolution&&
        f.RenderSubrectWidth&&f.RenderSubrectHeight&&f.OutputWidth&&f.OutputHeight&&
        std::isfinite(f.NativeJitterX)&&std::isfinite(f.NativeJitterY)&&
        std::abs(f.NativeJitterX)<=.5f&&std::abs(f.NativeJitterY)<=.5f&&
        known(Field::CreateFlags,f.NativeFeatureFlags)&&known(Field::RenderWidth,f.RenderSubrectWidth)&&
        known(Field::RenderHeight,f.RenderSubrectHeight)&&known(Field::DepthX,f.DepthSubrectBaseX)&&
        known(Field::DepthY,f.DepthSubrectBaseY)&&known(Field::MotionX,f.MotionSubrectBaseX)&&
        known(Field::MotionY,f.MotionSubrectBaseY)&&known(Field::ScaleX,f.MvScaleX)&&known(Field::ScaleY,f.MvScaleY)&&
        f.MvScaleX!=0&&f.MvScaleY!=0&&known(Field::JitterX,f.NativeJitterX)&&known(Field::JitterY,f.NativeJitterY)&&
        known(Field::DepthBounds,1)&&known(Field::MotionBounds,1);
}
inline GuideDiagnostics::Snapshot Source(const DlssNrFrameInfo& f) {
    GuideDiagnostics::Snapshot s;
    s.rawSource=f.RawSourceMetadata;
    s.source=f.SourceCaptureSerial; s.epoch=f.SourceCaptureEpoch; s.picture=f.SourceCaptureSerial;
    s.feature=f.SceneGuideOwner[0]?f.SceneGuideOwner[0]:f.SceneGuideOwner[1]; s.owner=f.SceneGuideOwner[1];
    s.providerFrame=f.SubmissionEpoch; s.tick=f.GuideObservationTick?f.GuideObservationTick:GuideDiagnostics::Now();
    s.stage=GuideDiagnostics::Stage::Received;
    for (unsigned i=0;i<f.GuideProvenance.fields.size();++i) {
        const auto& v=f.GuideProvenance.fields[i];
        s.Set(static_cast<GuideDiagnostics::Field>(i),v.value,v.valid,
            static_cast<GuideDiagnostics::Origin>(v.origin));
    }
    s.scaleX=f.MvScaleX; s.scaleY=f.MvScaleY;
    s.jitterX=f.NativeJitterX; s.jitterY=f.NativeJitterY;
    s.depthInverted=f.DepthInverted; s.opticalFlow=f.MotionIsPictureFlow;
    s.phaseVerified=f.NativeGuidePhaseVerified;
    s.motionJitterKnown=f.NativeMotionJitterKnown;
    s.motionJittered=f.NativeMotionVectorsJittered;
    // Source captures canonical, unjittered final SR color beside independently
    // raw guide rows. Known NGX jitter alone does not align those sampled grids.
    // The calibration consumer must address guides at canonicalUV + J/renderSize
    // BEFORE each candidate's row reflection. Do not label this mixed grid as
    // CanonicalSampleOffsets (which also shifts the color correspondence).
    s.addressPhaseKnown=RawRasterAddressKnown(f);
    // Stable identity of this source interpretation, never a physical FG clock.
    uint64_t key=f.SceneGuideOwner[0]*1000003ull^f.SceneGuideOwner[1]*1000003ull^f.SceneGuideOwner[2];
    for (const auto& v:f.GuideProvenance.fields) {
        if (&v==&f.GuideProvenance.fields[static_cast<unsigned>(DlssNr::GuideMetadata::Field::JitterX)] ||
            &v==&f.GuideProvenance.fields[static_cast<unsigned>(DlssNr::GuideMetadata::Field::JitterY)]) continue;
        const auto bits=std::bit_cast<uint64_t>(v.value);
        key=(key*1000003ull)^bits^(static_cast<uint64_t>(v.origin)<<1)^static_cast<uint64_t>(v.valid);
    }
    s.configuration=key;
    s.reason=f.NativeGuideMetadataKnown?"source metadata resolved":"unknown-source-guide-metadata";
    if (!f.NativeGuideMetadataKnown) {
        for (unsigned i=0;i<static_cast<unsigned>(DlssNr::GuideMetadata::Field::JitterX);++i)
            if (!f.GuideProvenance.fields[i].valid) { s.reason=std::string("missing ")+GuideDiagnostics::FieldNames[i]; break; }
        const auto& fields=f.GuideProvenance.fields;
        if (fields[static_cast<unsigned>(DlssNr::GuideMetadata::Field::DepthBounds)].valid &&
            !fields[static_cast<unsigned>(DlssNr::GuideMetadata::Field::DepthBounds)].value) s.reason="depth bounds invalid";
        if (fields[static_cast<unsigned>(DlssNr::GuideMetadata::Field::MotionBounds)].valid &&
            !fields[static_cast<unsigned>(DlssNr::GuideMetadata::Field::MotionBounds)].value) s.reason="motion bounds invalid";
    }
    return s;
}
struct Pending {
    GuideDiagnostics::Consumer consumer;
    GuideDiagnostics::Snapshot snapshot;
    DlssNrGpuLifetime::Use use;
    bool submitted=false, completed=false;
};
inline std::vector<Pending>& PendingRecords() { static std::vector<Pending> records; return records; }
inline void Poll() {
    auto& records=PendingRecords();
    for (auto& p:records) {
        if (!p.use) continue;
        if (!p.submitted && p.use->submitted) {
            p.submitted=true;
            auto s=p.snapshot; s.stage=GuideDiagnostics::Stage::Submitted;
            s.submission=p.use->generation;
            GuideDiagnostics::Publish(p.consumer,std::move(s));
        }
        if (!p.completed && p.use->GpuComplete()) {
            p.completed=true;
            auto s=p.snapshot; s.stage=GuideDiagnostics::Stage::Completed;
            s.submission=p.use->generation;
            // Preserve source timestamp: a late completion must remain stale and
            // cannot masquerade as a newer source or measured pixel contribution.
            GuideDiagnostics::Publish(p.consumer,std::move(s));
        }
    }
    std::erase_if(records,[](const Pending& p) { return p.use && p.use->Complete(); });
}
inline void Recorded(GuideDiagnostics::Consumer c,GuideDiagnostics::Snapshot s,const DlssNrGpuLifetime::Use& use) {
    Poll();
    GuideDiagnostics::Publish(c,s);
    // Drop diagnostics at capacity; never block work or evict pending evidence.
    if (s.stage==GuideDiagnostics::Stage::Recorded && use && PendingRecords().size()<64)
        PendingRecords().push_back({c,std::move(s),use});
}
} // namespace DlssNrGuideObservation
