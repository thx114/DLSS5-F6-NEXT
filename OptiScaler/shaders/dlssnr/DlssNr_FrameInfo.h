// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "GuideMetadata.h"
#include <runtime/GuideSourceMetadata.h>

struct ID3D12CommandQueue;

// The shared producer-to-NR frame descriptor. It does not own GPU resources
// or import shader constants, Config, State, a game name or a graphics SDK.
struct DlssNrFrameInfo
{
    DlssNr::GuideMetadata::Provenance GuideProvenance;
    GuideSourceMetadata::Ptr RawSourceMetadata;
    uint64_t GuideObservationTick = 0; // original capture time; completion must not refresh it
    // Which way round depth runs. The game states this when it creates its own upscaler.
    bool DepthInverted = false;

    // How the game encodes its motion vectors, as the game itself reports it. Passed through: every
    // resource already carries a subrect saying how big it is, so scaling by the resolution ratio on
    // top of that counts it twice.
    float MvScaleX = 1.0f;
    float MvScaleY = 1.0f;

    // Throw away the model's history. Set it on a cut, a teleport, or the first frame of a feature.
    bool Reset = false;
    // StarRail presented colour without a current native capture: motion is an ABI-only
    // zero placeholder, never current guide authority. No temporal/detail/ViT reuse.
    bool FreshColorResetFallback = false;
    // Evaluate every model layer and its bottleneck on this picture. Measured
    // motion can be valid model input without authorizing detail/output reuse.
    bool FullModelRequired = false;

    // Whether the colour buffer holds linear, open-ended light or a frame that has already been
    // through a tonemapper. Getting this wrong encodes an encoded frame a second time, which looks
    // washed out and banded.
    bool ColourIsLinearHdr = true;

    // The Colour encoding override (DlssNr_ColourEncoding.h) for this frame. InputEncoding is what the shared pass
    // converts at its edges (DlssNrConstants::InputEncoding: 3 gamma 2.2, 4 PQ, else nothing); ColourEncoding is the
    // encoding that was chosen, for the log, and differs from it where another pass does the conversion (Finished
    // Picture's PQ). ColourIsLinearHdr is already the choice's LinearHdr(). Zero for every path that does not set it.
    uint32_t InputEncoding = 0;
    uint32_t ColourEncoding = 0;
    bool ColourEncodingForced = false;

    // The SR colour input arrives readable, whereas a completed upscaler output normally arrives as
    // a UAV. The DX12 pass uses this to preserve the caller's state and to fall back through a copy
    // when a pre-SR colour resource was not created with UAV support.
    bool BeforeUpscale = false;
    // Owned copy, not the game's Color. Pre-SR copies arrive/return readable;
    // FinishedPicture copies use their explicit OutputArrivalState instead.
    bool PrivateColorCopy = false;
    bool FinishedPicture = false;
    bool GenshinPresentPipeline = false; // sole source-Present writer; no legacy output/cadence reuse
    // Native DLSS-G input: experimental spatial compression is held off after a device hang.
    bool FrameGenerationInput = false;
    // The motion vectors describe this very picture, HUD and post-processing included, because they were measured on
    // it (Optical F5Low's native input) rather than rendered by the game for its scene. Reuse detail between frames
    // needs that to run on a finished picture.
    bool MotionMatchesPicture = false;
    // Provenance, separate from permission to reuse native HUDless vectors.
    bool MotionIsPictureFlow = false; // optical vectors already in current colour coordinates
    bool DepthIsPictureAligned = false; // depth finder/synthetic guide already aligned to current colour
    // Request the existing native-present guide path, independently of guide validity.
    // Currently populated by the StarRail endpoint. Never select it from an exe name:
    // SceneGuides::Resolve rechecks the exact source, view and model-guide contract.
    // NativeGuideContractVerified/phase below separately govern temporal history.
    bool NativeSceneRoute = false;
    // Actual Color/MV/Depth from one live NGX Evaluate before SR. Unlike a final
    // picture, this colour and its guides share the current raster jitter.
    bool NativePreSrRoute = false;
    uint64_t NativePreSrOwner[3] {};
    bool NativeGuideContractVerified = false;
    bool NativeBaseIdentityVerified = false;
    bool NativeCurrentDepthVerified = false;
    uint64_t SourceCaptureSerial = 0;
    uint64_t SourceCaptureEpoch = 0; // actual capture epoch; never the model submission token
    ID3D12CommandQueue* NativePresentQueueIdentity = nullptr; // identity only, valid during Dispatch
    bool NativeFeatureFlagsKnown = false;
    uint32_t NativeFeatureFlags = 0;
    bool NativeMotionJitterKnown = false;
    bool NativeMotionVectorsJittered = false;
    bool NativeCurrentJitterKnown = false;
    float NativeJitterX = 0.0f, NativeJitterY = 0.0f;
    bool NativeGuideMetadataKnown = false; // successful region/scale reads, finite scales, valid bounds
    bool NativeRouteConventionVerified = false; // actual route direction/units/native-to-final mapping
    bool NativeGuidePhaseVerified = false; // includes address phase even when MV is unjittered
    // Normalized active-guide UV. Address = workUV + phase; MV = raw * scale / size - cancel.
    float NativeMotionPhaseUvX = 0.0f, NativeMotionPhaseUvY = 0.0f;
    float NativeDepthPhaseUvX = 0.0f, NativeDepthPhaseUvY = 0.0f;
    float NativeMotionJitterCancelUvX = 0.0f, NativeMotionJitterCancelUvY = 0.0f;
    // Optional: how far the previous frame's history can be trusted at each pixel, as distrust 0..1 in .r (0 trusts it
    // fully), at any size (read by uv). On native input it is Optical F5Low's trust mask. A D3D12 resource readable
    // from a compute shader, left in the state it came in. Null: no outside opinion, reuse trusts its own tests.
    void* HistoryDistrust = nullptr;
    uint32_t OutputArrivalState = 0;
    // Only used by a distinct, read-only whole-picture input. Ordinary in-place
    // Evaluate routes continue using OutputArrivalState as before.
    uint32_t InputArrivalState = 0;
    float WhitePointOverride = 0.0f;
    bool IndependentCommands = false; // owned command list, no game root signature to restore
    // R583: issued only by the authenticated Genshin final-picture NativeProducer endpoint.
    // That producer executes this exact list on the queue passed to Dispatch after the callback returns.
    // A timing/global queue hint, scene guides, or IndependentCommands alone never grants this authority.
    bool OrderedPictureReuseQueue = false;
    // Stable producer domain, distinct from the per-picture ordinal. Never key reuse on a new
    // source-frame/generation token each full frame; each picture remains authenticated by Scope.
    uint64_t OrderedPictureReuseOwner[6] {};
    // Separate from NativeSceneRoute: authenticated Genshin NativeProducer picture-flow guides.
    // Each capability proves its own guides on the exact callback and unjittered picture phase.
    bool PictureTemporalRoute = false;
    bool PictureTemporalGuidesVerified = false;
    bool PictureFlowGuidesVerified = false; // separate no-depth full-model feedback capability
    // Keep this successful SR source's scene depth for the model while
    // final-picture flow/trust owns history correspondence. A separately recorded
    // same-source scene mapping may veto history geometrically; neither source
    // depth nor the veto grants native/picture-depth reuse authority.
    bool PictureFlowModelSceneDepth = false;
    // Producer claim for the exact current successful SR source (currently Late ZZZ).
    // Retained motion is distinct from final-picture flow. The retained-source
    // endpoint supplies SourceCaptureSerial == SourceCaptureEpoch == source ordinal;
    // neither value is SubmissionEpoch, which may follow a different model clock.
    // Resolve must still check owner, flags, view/rect, units and phase before recording.
    // Kept alive by Late slot + actual descriptor recording, not this opaque view or
    // this bool. Model scene guides never grant picture/temporal depth authority.
    bool SceneGuideSourceVerified = false;
    void* SceneGuideMotion = nullptr;
    uint64_t SceneGuideOwner[3] {}; // actual SR handle, context/backend identity, source handle or producer generation
    unsigned SceneMotionWidth = 0, SceneMotionHeight = 0;
    unsigned SceneMotionBaseX = 0, SceneMotionBaseY = 0;
    float SceneMvScaleX = 0.0f, SceneMvScaleY = 0.0f;
    // Ordinary successful-source clock + raw final-color flow/trust on EVERY source.
    // Separate from interval-1 feedback; never grants SDK generated-frame authority.
    bool PictureFlowReuseGuidesVerified = false;
    bool PictureTemporalReset = false; // feedback history only; does not override model scene-cut policy
    uint64_t PictureGuideEpoch = 0, PictureGuideCutSerial = 0;
    // Reset temporal history when switching between ordinary SR and Ray Reconstruction.
    bool RayReconstruction = false;

    // Submission epoch supplied by the caller. Native DX12 uses the wrapped swapchain Present count;
    // the DX11/Vulkan bridges use their successfully submitted frame counter. A feature created in an
    // epoch is never evaluated until this value changes.
    unsigned long long SubmissionEpoch = 0;

    // The game's own exposure: a 1x1 texture holding, in the SDK's words, "the final exposure scale".
    //
    // This is the number that makes a cave and a field comparable, and it is the reason a fixed paper
    // white cannot serve both. It comes from the game, decided before anything here runs, so unlike a
    // statistic measured off the frame it cannot be pulled around by what this pass writes.
    //
    // May be null on any given frame -- GTA V supplied it, then did not, three times in one session --
    // so whoever consumes it holds the last good value rather than falling back to a default.
    void* ExposureTexture = nullptr;

    // The scale the game multiplied its buffer by for float precision, which DLSS is told so it can
    // undo it. Usually 1. Divided out before the exposure is applied, exactly as FSR's PrepareRgb does.
    float PreExposure = 1.0f;

    // How much of the depth and motion vector textures the game actually rendered into.
    // Before SR this also selects the origin-zero active colour rectangle, not its allocation.
    //
    // Not the same thing as how big those textures are, and the difference is the whole point. A game
    // with dynamic resolution allocates its guides once at the largest size it will ever need and
    // then renders into the top-left corner of them, telling the upscaler how much is real through
    // DLSS.Render.Subrect.Dimensions. Sizing the guides from the resource instead means handing the
    // model whatever was left in the margin -- stale depth and stale vectors -- and calling it scene.
    //
    // Zero means the game did not say, in which case the resource's own size is the best answer
    // available and is what gets used.
    unsigned int RenderSubrectWidth = 0;
    unsigned int RenderSubrectHeight = 0;

    unsigned int DepthSubrectBaseX = 0;
    unsigned int DepthSubrectBaseY = 0;
    unsigned int MotionSubrectBaseX = 0;
    unsigned int MotionSubrectBaseY = 0;

    // DLSS permits motion vectors at either render or output resolution. This flag comes from the
    // feature-create flags and decides which valid-region dimensions apply to the motion texture.
    bool MotionVectorsLowResolution = false;
    // DLSS output extent, independent of the NR injection point or working scale.
    unsigned int OutputWidth = 0;
    unsigned int OutputHeight = 0;
};
