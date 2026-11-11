// Standalone CPU contract regression; no Windows headers, SDK, GPU or DLL.
// Example for Lead: cl /nologo /std:c++17 /EHsc /W4 /WX nr_scene_guide_contract.cpp
// Synthetic resource/queue identities below are never dereferenced. These tests
// establish policy boundaries, not genuine producer provenance or GPU readiness.
#include "../../shaders/dlssnr/SceneGuideContract.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

struct ID3D12CommandQueue {};

namespace
{
using namespace DlssNr::SceneGuides;
unsigned checks = 0;
ID3D12CommandQueue queue;
int retainedMotion = 0;

void Require(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct Fixture
{
    DlssNrFrameInfo frame;
    Consumer consumer;
    explicit Fixture(bool native = false)
    {
        frame.FinishedPicture = frame.IndependentCommands = true;
        frame.NativeSceneRoute = native;
        frame.SceneGuideSourceVerified = !native;
        frame.SceneGuideMotion = &retainedMotion;
        frame.SceneGuideOwner[0] = 23;
        frame.SourceCaptureSerial = frame.SourceCaptureEpoch = 41;
        frame.SubmissionEpoch = 900; // intentionally distinct from source ordinal
        frame.NativeBaseIdentityVerified = frame.NativeCurrentDepthVerified = native;
        frame.NativePresentQueueIdentity = native ? &queue : nullptr;
        frame.NativeFeatureFlagsKnown = frame.NativeGuideMetadataKnown = true;
        frame.NativeFeatureFlags = 2; // retained low-resolution MV create flag
        frame.NativeMotionJitterKnown = frame.NativeCurrentJitterKnown = true;
        frame.NativeJitterX = 0.5f; frame.NativeJitterY = -0.5f;
        frame.RenderSubrectWidth = frame.SceneMotionWidth = 1024;
        frame.RenderSubrectHeight = frame.SceneMotionHeight = 512;
        frame.OutputWidth = 2048; frame.OutputHeight = 1024;
        frame.DepthSubrectBaseX = 7; frame.DepthSubrectBaseY = 11;
        frame.MotionSubrectBaseX = frame.SceneMotionBaseX = 13;
        frame.MotionSubrectBaseY = frame.SceneMotionBaseY = 17;
        frame.MvScaleX = frame.SceneMvScaleX = 2.0f;
        frame.MvScaleY = frame.SceneMvScaleY = -3.0f;
        consumer.pictureWidth = 2048; consumer.pictureHeight = 1024;
        consumer.workWidth = 1536; consumer.workHeight = 768;
        consumer.sourceMotionLowResolution = true;
        consumer.depth = { true, true, true, 1050, 550, 1, 1, 1, Samples::DeviceDepth, 41 };
        consumer.motion = { true, true, true, 1100, 600, 1, 1, 1, Samples::SignedMotion, 16 };
        consumer.depthRect = { 7, 11, 1024, 512 };
        consumer.motionRect = { 13, 17, 1024, 512 };
        consumer.scaleX = 2.0f; consumer.scaleY = -3.0f;
    }
    Decision ResolveNow() const { return Resolve(frame, consumer); }
    HistoryKey Key(bool recorded = true, bool geometry = false) const
    {
        return MakeHistoryKey(frame, consumer, ResolveNow(), recorded, geometry);
    }
};

template<class Change> void Reject(Change change, const char* message)
{
    // Each adverse fact must reject both native and retained-source requests.
    for (bool native : { false, true })
    {
        Fixture f(native);
        change(f);
        const auto decision = f.ResolveNow();
        Require(!decision.Admitted() && !decision.NeedsPreparation(), message);
        Require(f.Key()[0] == 0, "rejected input cannot publish a model guide history contract");
    }
}

void AdmissionBoundaries()
{
    Fixture retained;
    Require(retained.ResolveNow().NeedsPreparation(), "qualified retained source can request existing preparation");
    Require(!retained.frame.NativeGuideContractVerified && !retained.frame.NativeGuidePhaseVerified &&
        !retained.frame.PictureTemporalGuidesVerified && retained.ResolveNow().Admitted(),
        "model source contract is independent of temporal guide authority");

    Fixture native(true);
    native.frame.SourceCaptureSerial = 41;
    native.frame.SourceCaptureEpoch = 73;
    native.frame.SubmissionEpoch = 900;
    Require(native.ResolveNow().path == Path::NativeExisting && !native.ResolveNow().NeedsPreparation(),
        "native source, capture and model clocks remain independent without adding preparation");
    native.frame.SceneGuideSourceVerified = true; // competing producer claim
    Require(native.ResolveNow().requested == Request::NativeExisting &&
        native.ResolveNow().path == Path::NativeExisting && !native.ResolveNow().NeedsPreparation(),
        "native endpoint always retains existing path without a new guide preparation");
    native.frame.NativeBaseIdentityVerified = false;
    Require(!native.ResolveNow().Admitted() && !native.ResolveNow().NeedsPreparation(),
        "invalid native identity cannot fall through to retained preparation");
    Fixture unknown;
    unknown.frame.SceneGuideSourceVerified = false;
    Require(unknown.ResolveNow().requested == Request::None && !unknown.ResolveNow().Admitted(),
        "resources and SDK metadata alone cannot invent an adapted source route");
    Fixture claimOnly;
    claimOnly.frame = {};
    claimOnly.frame.SceneGuideSourceVerified = true;
    Require(!claimOnly.ResolveNow().Admitted(), "a verified-named bool alone cannot qualify scene guides");

    Reject([](Fixture& f) { f.frame.FinishedPicture = false; }, "unfinished picture");
    Reject([](Fixture& f) { f.frame.BeforeUpscale = true; }, "pre-upscale route");
    Reject([](Fixture& f) { f.frame.IndependentCommands = false; }, "unowned command route");
    Reject([](Fixture& f) { f.frame.FreshColorResetFallback = true; }, "fresh color is not guide authority");
    Reject([](Fixture& f) { f.consumer.vulkanOrigin = true; }, "unsupported Vulkan origin");
    Reject([](Fixture& f) { f.consumer.spatial = true; }, "nonlinear packed coordinates need a different contract");
    Reject([](Fixture& f) { f.frame.SourceCaptureSerial = 0; }, "missing exact source ordinal");
    Reject([](Fixture& f) { f.frame.SourceCaptureSerial = UINT64_MAX; }, "invalid source ordinal");
    Reject([](Fixture& f) { f.frame.SourceCaptureEpoch = UINT64_MAX; }, "invalid capture epoch");
    Reject([](Fixture& f) { f.frame.NativeFeatureFlagsKnown = false; }, "unknown create flags");
    Reject([](Fixture& f) { f.frame.NativeGuideMetadataKnown = false; }, "unknown source metadata");
    Reject([](Fixture& f) { f.frame.NativeMotionJitterKnown = false; }, "unknown MV phase convention");
    Reject([](Fixture& f) { f.frame.NativeMotionVectorsJittered = true; }, "jittered MV unsupported");
    Reject([](Fixture& f) { f.frame.NativeCurrentJitterKnown = false; }, "unknown raster address phase");
    Reject([](Fixture& f) { f.frame.NativeJitterX = 0.5001f; }, "out of contract jitter");
    Reject([](Fixture& f) { f.frame.NativeJitterY = std::numeric_limits<float>::quiet_NaN(); }, "nonfinite phase");
    Reject([](Fixture& f) { f.consumer.sourceMotionLowResolution = false; }, "source resolution not declared");
    Reject([](Fixture& f) { --f.consumer.motionRect.width; }, "motion and render grid mismatch");
    Reject([](Fixture& f) { --f.consumer.depthRect.height; }, "depth and render grid mismatch");
    Reject([](Fixture& f) { f.consumer.motionRect.x = f.consumer.depthRect.x; }, "independent motion subrect");
    Reject([](Fixture& f) { f.consumer.depth.present = false; }, "missing current depth cannot borrow an old view");
    Reject([](Fixture& f) { f.consumer.motion.present = false; }, "missing current motion");
    Reject([](Fixture& f) { f.consumer.depth.width = 1030; }, "depth subrect exceeds allocation");
    Reject([](Fixture& f) { f.consumer.motion.height = 528; }, "motion subrect exceeds allocation");
    Reject([](Fixture& f) { f.consumer.depth.texture2D = false; }, "wrong texture dimension");
    Reject([](Fixture& f) { f.consumer.depth.layers = 2; }, "array view unsupported");
    Reject([](Fixture& f) { f.consumer.motion.samples = 4; }, "MSAA view unsupported");
    Reject([](Fixture& f) { f.consumer.depth.mipLevels = 2; }, "ambiguous source mip view");
    Reject([](Fixture& f) { f.consumer.motion.shaderReadable = false; }, "view denies shader reads");
    Reject([](Fixture& f) { f.consumer.depth.encoding = Samples::Unknown; }, "unknown depth format");
    Reject([](Fixture& f) { f.consumer.motion.encoding = Samples::DeviceDepth; }, "scalar depth cannot act as motion");
    Reject([](Fixture& f) { f.consumer.scaleX = 0; }, "zero scale");
    Reject([](Fixture& f) { f.consumer.scaleY = std::numeric_limits<float>::infinity(); }, "nonfinite scale");
    Reject([](Fixture& f) { --f.consumer.pictureWidth; }, "color and output extent mismatch");
    Reject([](Fixture& f) { f.consumer.workWidth = 0; }, "empty work raster");
    Reject([](Fixture& f) { f.consumer.coordinates.key = 8; }, "picture-flow mapping cannot stand in for scene mapping");

    retained.frame.SourceCaptureEpoch = 40;
    Require(!retained.ResolveNow().Admitted(), "retained guides from another successful source rejected");
    retained = Fixture();
    retained.frame.SceneGuideOwner[0] = 0;
    Require(!retained.ResolveNow().Admitted(), "anonymous retained source rejected");
    retained.frame.SceneGuideOwner[1] = 77;
    Require(!retained.ResolveNow().Admitted(), "virtual context without creation generation rejected");
    retained.frame.SceneGuideOwner[2] = 5;
    Require(retained.ResolveNow().NeedsPreparation(), "virtual source with real generation uses same contract");
}

void PhaseAndHistory()
{
    Fixture f;
    const auto original = f.Key();
    Require(f.ResolveNow().phaseUvX == 1.0f / 2048.0f && f.ResolveNow().phaseUvY == -1.0f / 1024.0f,
        "half-render-pixel jitter has canonical normalized phase, independent of MV scale");
    for (unsigned controls = 0; controls < 8; ++controls)
    {
        f.consumer.coordinates = DlssNr::GuideCoordinates::Resolve(
            (controls & 1u) != 0, (controls & 2u) != 0, (controls & 4u) != 0, true, false, false);
        Require(f.ResolveNow().NeedsPreparation(), "row and vector mappings are independently supported");
        Require(f.ResolveNow().phaseUvX == 1.0f / 2048.0f && f.ResolveNow().phaseUvY == -1.0f / 1024.0f,
            "row/sign correction cannot negate or scale canonical address phase again");
        Require((f.Key() == original) == (controls == 0), "coordinate changes invalidate the model/history contract");
    }
    f = Fixture();
    ++f.frame.SourceCaptureSerial; ++f.frame.SourceCaptureEpoch;
    f.frame.SubmissionEpoch += 7;
    f.frame.NativeJitterX = -0.25f; f.frame.NativeJitterY = 0.125f;
    Require(f.ResolveNow().Admitted() && f.Key() == original,
        "adjacent current sources and raster jitter do not cause periodic history reset");
    ++f.frame.SceneGuideOwner[0];
    Require(f.Key() != original, "replaced source with equal rect/format cannot inherit history");
    f = Fixture();
    ++f.consumer.motion.format;
    Require(f.Key() != original, "exact source format is part of history identity");
    f = Fixture();
    f.frame.SceneMvScaleX = f.consumer.scaleX = 4.0f;
    Require(f.Key() != original, "MV units are part of history identity");
    f = Fixture();
    Require(f.Key(false)[0] == 0 && f.Key(false) != original,
        "admission without actual preparation recording cannot publish a prepared contract");
    const auto failed = f.Key(false);
    ++f.frame.SceneGuideOwner[0];
    Require(f.Key(false) != failed, "failed preparation still isolates source replacement");
    f = Fixture();
    f.frame.NativeGuideMetadataKnown = false;
    Require(f.Key()[0] == 0 && f.Key() != original, "lost metadata invalidates formerly prepared history");
    f = Fixture();
    f.frame.SceneGuideSourceVerified = false;
    Require(f.Key() == HistoryKey {}, "leaving the capability clears its history contract");
    f = Fixture(true);
    Require(f.Key(false)[0] == 1 && f.Key(false) == f.Key(true),
        "native existing guides never depend on a new preparation recording");
    f = Fixture();
    Require(f.Key(true, false) != f.Key(true, true),
        "conditional geometry support is distinct from model-only source depth");

    Texture huge { true, true, true, UINT64_MAX, UINT32_MAX, 1, 1, 1, Samples::DeviceDepth, 41 };
    Require(Fits(huge, { UINT32_MAX, 0, UINT32_MAX, 1 }), "width bounds retain the descriptor's full 64 bits");
    Require(!Fits(huge, { 0, UINT32_MAX - 1, 1, 2 }), "height addition overflow cannot admit an out-of-bounds rect");
}
}

int main()
{
    AdmissionBoundaries();
    PhaseAndHistory();
    std::printf("NR scene-guide CPU contract PASS checks=%u; GPU/producer validation remains separate\n", checks);
}
