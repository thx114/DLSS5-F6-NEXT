// SPDX-License-Identifier: GPL-3.0-or-later
#include "../NrResident.h"
#include "../../shaders/dlssnr/DlssNr_FinishedReady.h"
#include "../../shaders/dlssnr/DlssNr_FrameInfo.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace FramePipeline::NrResident;
unsigned checks = 0;
void Check(bool value)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "NR residency check %u failed\n", checks); std::exit(1); }
}

int main()
{
    // Other games, disabled NR and the independent optical owner stay separate.
    for (unsigned bits = 0; bits < 16; ++bits)
        Check(Requested((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0) == (bits == 7));
    for (unsigned bits = 0; bits < 8; ++bits)
        Check(PlacementSupported((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0) == (bits == 1));
    for (const auto input : {Input::Missing, Input::Ready, Input::Pending, Input::DeviceRemoved})
        for (unsigned bits = 0; bits < 8; ++bits)
        {
            const bool resident = (bits & 1) != 0, legacy = (bits & 2) != 0, ordered = (bits & 4) != 0;
            const bool expected = ordered && input != Input::Ready && input != Input::DeviceRemoved &&
                (resident || (legacy && input == Input::Missing));
            Check(UseFreshColor(input, resident, legacy, ordered) == expected);
        }

    // A pending newest capture cannot fall back to older ready guides. A forced
    // current-color route is independent of that pending capture's fence.
    DlssNr::FinishedInput inputs[] = {{true, 10, false, 20, 20}, {true, 11, false, 20, 21}};
    size_t selected = 0;
    Check(DlssNr::PickFinishedInput(inputs, 2, selected) == DlssNr::FinishedPick::NotReady);
    Check(selected == 1);
    Check(UseFreshColor(Input::Pending, true, false, true));
    Check(!UseFreshColor(Input::Pending, false, true, true));
    inputs[1].completed = UINT64_MAX;
    Check(DlssNr::PickFinishedInput(inputs, 2, selected) == DlssNr::FinishedPick::NotReady);
    Check(!UseFreshColor(Input::DeviceRemoved, true, true, true));

    // Execute the actual producer frame downgrade used by Late, with stale
    // authority deliberately seeded. Current measured motion remains usable,
    // but no native receipt, old depth or skipped-model/output reuse survives.
    DlssNrFrameInfo frame;
    frame.OutputWidth = 2560; frame.OutputHeight = 1600;
    frame.RenderSubrectWidth = 1280; frame.RenderSubrectHeight = 800;
    frame.MvScaleX = 0.5f; frame.MvScaleY = -0.5f;
    frame.MotionSubrectBaseX = frame.MotionSubrectBaseY = 8;
    frame.MotionVectorsLowResolution = true;
    frame.NativeSceneRoute = frame.NativeGuideContractVerified = true;
    frame.NativeBaseIdentityVerified = frame.NativeCurrentDepthVerified = true;
    frame.NativeGuidePhaseVerified = frame.DepthIsPictureAligned = true;
    frame.SourceCaptureSerial = frame.SourceCaptureEpoch = 99;
    frame.SceneGuideSourceVerified = frame.OrderedPictureReuseQueue = true;
    frame.PictureTemporalRoute = frame.PictureTemporalGuidesVerified = true;
    frame.PictureFlowGuidesVerified = frame.PictureFlowReuseGuidesVerified = true;
    frame.PictureFlowModelSceneDepth = frame.FreshColorResetFallback = true;
    frame.ColourIsLinearHdr = true; frame.InputEncoding = 4; frame.PreExposure = 2.0f;
    frame.SubmissionEpoch = 345;
    UseMeasuredFlow(frame, true);
    Check(frame.MotionIsPictureFlow && frame.MotionMatchesPicture);
    Check(!frame.MotionVectorsLowResolution && frame.MotionSubrectBaseX == 0 && frame.MotionSubrectBaseY == 0);
    Check(frame.MvScaleX == 1.0f && frame.MvScaleY == 1.0f);
    Check(frame.RenderSubrectWidth == 2560 && frame.RenderSubrectHeight == 1600);
    Check(!frame.NativeSceneRoute && !frame.NativeGuideContractVerified);
    Check(!frame.NativeBaseIdentityVerified && !frame.NativeCurrentDepthVerified);
    Check(!frame.NativeGuidePhaseVerified && !frame.DepthIsPictureAligned);
    Check(frame.SourceCaptureSerial == 0 && frame.SourceCaptureEpoch == 0);
    Check(!frame.SceneGuideSourceVerified && !frame.OrderedPictureReuseQueue);
    Check(!frame.PictureTemporalRoute && !frame.PictureTemporalGuidesVerified);
    Check(!frame.PictureFlowGuidesVerified && !frame.PictureFlowReuseGuidesVerified && !frame.PictureFlowModelSceneDepth);
    Check(frame.NativePresentQueueIdentity == nullptr && frame.SceneGuideMotion == nullptr);
    Check(!frame.FreshColorResetFallback && frame.Reset);
    Check(frame.ColourIsLinearHdr && frame.InputEncoding == 4 && frame.PreExposure == 2.0f);
    Check(frame.SubmissionEpoch == 345);
    UseMeasuredFlow(frame, false);
    Check(!frame.Reset);

    Recovery recovery;
    Check(!recovery.ResetRequired());
    recovery.Enable(true);
    Check(recovery.ResetRequired());
    { Recovery::Attempt warmup(recovery); Check(warmup.ResetRequired()); warmup.Submitted(false, true); }
    Check(recovery.ResetRequired());
    { Recovery::Attempt fallback(recovery); fallback.Submitted(true, false); }
    Check(recovery.ResetRequired());
    // Recovery native Evaluate succeeds but Close/Signal fails: no Submitted.
    { Recovery::Attempt failedSubmit(recovery); Check(failedSubmit.ResetRequired()); }
    Check(recovery.ResetRequired());
    { Recovery::Attempt restored(recovery); Check(restored.ResetRequired()); restored.Submitted(true, true); }
    Check(!recovery.ResetRequired());
    { Recovery::Attempt continuous(recovery); Check(!continuous.ResetRequired()); continuous.Submitted(true, true); }
    Check(!recovery.ResetRequired());
    // No safe slot / invalid endpoint after a good run is a real history gap.
    { Recovery::Attempt gap(recovery); }
    Check(recovery.ResetRequired());
    for (unsigned i = 0; i < 100; ++i)
    {
        Recovery::Attempt gap(recovery);
        Check(gap.ResetRequired());
        if (i % 2) gap.Submitted(true, false);
    }
    { Recovery::Attempt restored(recovery); restored.Submitted(true, true); }
    Check(!recovery.ResetRequired());
    recovery.Enable(false);
    { Recovery::Attempt disabled(recovery); }
    Check(!recovery.ResetRequired());
    recovery.Enable(true);
    Check(recovery.ResetRequired());
    std::printf("NrResident: %u CPU checks passed\n", checks);
}
