// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace FramePipeline::NrResident
{
// This policy selects work only AFTER the endpoint proves current-color ordering.
// It grants no capture identity, temporal reuse authority or GPU completion.
enum class Input { Missing, Ready, Pending, DeviceRemoved };

constexpr bool Requested(bool starRail, bool force, bool nrEnabled, bool opticalOwner)
{
    return starRail && force && nrEnabled && !opticalOwner;
}

constexpr bool PlacementSupported(bool finishedPicture, bool beforeSr, bool deferred)
{
    return finishedPicture && !beforeSr && !deferred;
}

constexpr bool UseFreshColor(Input input, bool resident, bool legacyFallback, bool orderedCurrentColor)
{
    if (!orderedCurrentColor || input == Input::DeviceRemoved || input == Input::Ready) return false;
    return resident || (input == Input::Missing && legacyFallback);
}

// Used only with the newly built current-picture flow/trust resources. Preserve
// color/exposure/endpoint metadata, but never turn measured motion into native
// source/depth identity or permission to skip this frame's model evaluation.
template<class Frame> void UseMeasuredFlow(Frame& frame, bool reset)
{
    frame.FullModelRequired = true;
    frame.MotionIsPictureFlow = frame.MotionMatchesPicture = true;
    frame.MotionVectorsLowResolution = false;
    frame.MotionSubrectBaseX = frame.MotionSubrectBaseY = 0;
    frame.MvScaleX = frame.MvScaleY = 1.0f;
    frame.RenderSubrectWidth = frame.OutputWidth;
    frame.RenderSubrectHeight = frame.OutputHeight;
    frame.NativeSceneRoute = frame.NativeGuideContractVerified = false;
    frame.NativeBaseIdentityVerified = frame.NativeCurrentDepthVerified = false;
    frame.NativeGuidePhaseVerified = frame.DepthIsPictureAligned = false;
    frame.NativePresentQueueIdentity = nullptr;
    frame.SourceCaptureSerial = frame.SourceCaptureEpoch = 0;
    frame.SceneGuideSourceVerified = false;
    frame.SceneGuideMotion = nullptr;
    frame.OrderedPictureReuseQueue = frame.PictureTemporalRoute = false;
    frame.PictureTemporalGuidesVerified = frame.PictureFlowGuidesVerified = false;
    frame.PictureFlowReuseGuidesVerified = frame.PictureFlowModelSceneDepth = false;
    frame.FreshColorResetFallback = false;
    frame.Reset = reset;
}

// Recording/Evaluate acceptance is not a successful endpoint submission. Every
// early return keeps recovery pending, including creation warmup and failed
// Close/Signal. A fresh-color reset-only run never qualifies history.
class Recovery
{
    bool enabled_ = false;
    bool reset_ = true;
public:
    void Enable(bool enabled)
    {
        if (enabled != enabled_) reset_ = true;
        enabled_ = enabled;
    }
    bool ResetRequired() const { return enabled_ && reset_; }

    class Attempt
    {
        Recovery& owner_;
        bool committed_ = false;
    public:
        explicit Attempt(Recovery& owner) : owner_(owner) {}
        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;
        ~Attempt() { if (owner_.enabled_ && !committed_) owner_.reset_ = true; }
        bool ResetRequired() const { return owner_.ResetRequired(); }
        // Call only after the actual queue Signal AND source-clock Commit.
        void Submitted(bool wroteCurrentPicture, bool compatibleGuides)
        {
            committed_ = true;
            if (owner_.enabled_) owner_.reset_ = !wroteCurrentPicture || !compatibleGuides;
        }
    };
};
}
