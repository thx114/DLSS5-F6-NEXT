// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DlssNr_FrameInfo.h"
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <cmath>

namespace DlssNr::EvaluateGuides
{
inline bool Fits(ID3D12Resource* resource, unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (!resource || !w || !h) return false;
    const auto d = resource->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
        d.DepthOrArraySize == 1 && d.MipLevels == 1 && x <= d.Width && y <= d.Height &&
        w <= d.Width - x && h <= d.Height - y;
}

// Numeric facts only. The caller separately certifies the live source, colour
// stage and command recording; neither a diagnostic descriptor nor this reader
// grants source ownership, ordered submission or retirement authority.
template<class Source>
inline void Read(DlssNrFrameInfo& f, const NVSDK_NGX_Parameter& p, const Source& source,
                 ID3D12Resource* depth, ID3D12Resource* motion)
{
    using F = GuideMetadata::Field;
    using O = GuideMetadata::Origin;
    const auto scalar = [&](F field, const char* key, float& value, bool nonzero) {
        const auto fallback = value;
        const bool read = p.Get(key, &value) == NVSDK_NGX_Result_Success;
        if (!read) value = fallback;
        const bool valid = read && std::isfinite(value) && (!nonzero || value != 0);
        f.GuideProvenance.Set(field, value, valid, O::Evaluate);
        return valid;
    };
    const auto* raw = source.rawMetadata.get();
    const bool sameBoundary = raw && source.virtualContext && source.virtualGeneration &&
        raw->feature == source.ngxHandle && raw->context == reinterpret_cast<uintptr_t>(source.virtualContext) &&
        raw->generation == source.virtualGeneration &&
        raw->translatedDepth == reinterpret_cast<uintptr_t>(depth) &&
        raw->translatedMotion == reinterpret_cast<uintptr_t>(motion) && raw->rr == f.RayReconstruction;
    if (sameBoundary)
    {
        f.RawSourceMetadata = source.rawMetadata;
        f.GuideObservationTick = raw->tick;
        f.GuideProvenance = raw->provenance;
        f.NativeFeatureFlags = raw->flags;
        f.NativeFeatureFlagsKnown = source.featureFlagsKnown && raw->flagsKnown && raw->flags == source.featureFlags;
        f.RenderSubrectWidth = raw->renderWidth; f.RenderSubrectHeight = raw->renderHeight;
        f.DepthSubrectBaseX = raw->depth.region.x; f.DepthSubrectBaseY = raw->depth.region.y;
        f.MotionSubrectBaseX = raw->motion.region.x; f.MotionSubrectBaseY = raw->motion.region.y;
        f.MvScaleX = raw->scaleX; f.MvScaleY = raw->scaleY;
        f.NativeCurrentJitterKnown = raw->jitterKnown;
        f.NativeJitterX = raw->jitterX; f.NativeJitterY = raw->jitterY;
    }
    else
    {
        int flags = 0;
        const bool suppliedFlags = source.featureFlagsKnown ||
            p.Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) == NVSDK_NGX_Result_Success;
        f.NativeFeatureFlags = source.featureFlagsKnown ? source.featureFlags : static_cast<unsigned>(flags);
        f.NativeFeatureFlagsKnown = suppliedFlags;
        f.GuideProvenance.Set(F::CreateFlags, f.NativeFeatureFlags, suppliedFlags,
            source.featureFlagsKnown ? O::RetainedFeature : O::Evaluate);
        const bool wk = p.Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &f.RenderSubrectWidth) == NVSDK_NGX_Result_Success;
        const bool hk = p.Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &f.RenderSubrectHeight) == NVSDK_NGX_Result_Success;
        const bool extent = GuideMetadata::RenderExtent(f.RenderSubrectWidth, f.RenderSubrectHeight, wk, hk,
            source.renderWidth, source.renderHeight, source.renderExtentKnown);
        f.GuideProvenance.Set(F::RenderWidth, f.RenderSubrectWidth, extent, wk && hk ? O::Evaluate : O::RetainedFeature);
        f.GuideProvenance.Set(F::RenderHeight, f.RenderSubrectHeight, extent, wk && hk ? O::Evaluate : O::RetainedFeature);
        const auto base = [&](F field, const char* key, unsigned& value) {
            const bool supplied = p.Get(key, &value) == NVSDK_NGX_Result_Success;
            const bool known = GuideMetadata::OptionalBase(value, supplied, f.RayReconstruction);
            f.GuideProvenance.Set(field, value, known, supplied ? O::Evaluate : O::ApiDefault);
        };
        base(F::DepthX, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, f.DepthSubrectBaseX);
        base(F::DepthY, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, f.DepthSubrectBaseY);
        base(F::MotionX, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, f.MotionSubrectBaseX);
        base(F::MotionY, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, f.MotionSubrectBaseY);
        // Model's historical default is retained when the scalar is absent;
        // provenance remains unknown and cannot admit persistent feedback.
        scalar(F::ScaleX, NVSDK_NGX_Parameter_MV_Scale_X, f.MvScaleX, true);
        scalar(F::ScaleY, NVSDK_NGX_Parameter_MV_Scale_Y, f.MvScaleY, true);
        const bool jx = scalar(F::JitterX, NVSDK_NGX_Parameter_Jitter_Offset_X, f.NativeJitterX, false);
        const bool jy = scalar(F::JitterY, NVSDK_NGX_Parameter_Jitter_Offset_Y, f.NativeJitterY, false);
        f.NativeCurrentJitterKnown = jx && jy;
    }
    f.DepthInverted = (f.NativeFeatureFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    f.MotionVectorsLowResolution = (f.NativeFeatureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    f.NativeMotionJitterKnown = f.NativeFeatureFlagsKnown;
    f.NativeMotionVectorsJittered = (f.NativeFeatureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
    const bool depthBounds = Fits(depth, f.DepthSubrectBaseX, f.DepthSubrectBaseY, f.RenderSubrectWidth, f.RenderSubrectHeight);
    const bool motionBounds = Fits(motion, f.MotionSubrectBaseX, f.MotionSubrectBaseY,
        f.MotionVectorsLowResolution ? f.RenderSubrectWidth : f.OutputWidth,
        f.MotionVectorsLowResolution ? f.RenderSubrectHeight : f.OutputHeight);
    f.GuideProvenance.Set(F::DepthBounds, depthBounds, true, O::ActualCall);
    f.GuideProvenance.Set(F::MotionBounds, motionBounds, true, O::ActualCall);
    f.NativeGuideMetadataKnown = f.NativeFeatureFlagsKnown && depthBounds && motionBounds;
    for (unsigned i = 0; i < f.GuideProvenance.fields.size(); ++i)
        if (i != static_cast<unsigned>(F::JitterX) && i != static_cast<unsigned>(F::JitterY))
            f.NativeGuideMetadataKnown &= f.GuideProvenance.fields[i].valid;
}
}
