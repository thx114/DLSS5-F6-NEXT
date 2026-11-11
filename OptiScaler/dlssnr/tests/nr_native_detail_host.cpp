// Lead: compile with runtime/tests/nr-resident/stubs before OptiScaler on
// the include path, as for NrResidentConsumerTests.cpp. Fixtures supply only
// Config/State/status; native phase, Gate, Decide and cadence are production.
// Does not prove capture metadata, actual row normalization or GPU completion.
#pragma warning(push)
#pragma warning(disable:4324)
#include <dlssnr/DlssNrDetailReuseHost.h>
#pragma warning(pop)
#include <shaders/dlssnr/DlssNr_NativeGuidePhase.h>
#include <shaders/dlssnr/DlssNr_GuideCoordinates.h>
#include <cstdio>
#include <cstdlib>

namespace
{
using namespace DlssNrDetailReuse;
unsigned checks = 0;
void Require(bool condition, const char* why)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

void Exercise(unsigned interval)
{
    Config cfg;
    cfg.DlssNrDetailReuseInterval.value = interval;
    cfg.DlssNrMotionTextureFlipY.value = cfg.DlssNrMotionYInverted.value = true;
    TestFg fg;
    State::Instance().currentFG = &fg;

    DlssNr::NativeGuidePhase::Inputs phase;
    phase.nativeRoute = phase.baseIdentity = phase.currentDepth = phase.routeConvention = true;
    phase.metadataKnown = phase.flagsKnown = phase.jitterKnown = phase.motionJitterKnown = true;
    phase.motionLowResolution = true;
    phase.renderWidth = phase.depthWidth = phase.motionWidth = 1536;
    phase.renderHeight = phase.depthHeight = phase.motionHeight = 960;
    const auto mapping = DlssNr::GuideCoordinates::Resolve(true, true, true, true, false, false);
    phase.depthRowsFlip = mapping.depthRowsFlip;
    phase.motionRowsFlip = mapping.motionRowsFlip;
    phase.vectorYNegated = mapping.vectorYNegated;
    phase.scaleX = phase.scaleY = phase.decodedScaleX = 1.0f;
    phase.decodedScaleY = -1.0f;
    phase.jitterX = 0.25f; phase.jitterY = -0.375f;
    Require(!DlssNr::NativeGuidePhase::Resolve(phase).verified, "three flips require actual normalization");
    phase.normalizedTexturesReady = true; // fixture, not GPU recording proof
    const auto canonical = DlssNr::NativeGuidePhase::Resolve(phase);
    Require(canonical.verified, "row/sign normalization permits real NGX scene motion");
    Require(canonical.depthY == phase.jitterY / 960 && canonical.motionY == canonical.depthY,
        "vector Y sign never negates canonical jitter phase");

    HostFrame f;
    f.cfg = &cfg;
    f.finishedPicture = f.nativeTemporalRouteRequested = f.temporalRequested = true;
    f.baseIdentityVerified = f.currentDepthVerified = f.nativeGuideContractVerified = true;
    f.nativeGuidePhaseVerified = canonical.verified;
    f.nativeMotionPhaseUvX = canonical.motionX; f.nativeMotionPhaseUvY = canonical.motionY;
    f.nativeDepthPhaseUvX = canonical.depthX; f.nativeDepthPhaseUvY = canonical.depthY;
    f.workWidth = 1280; f.workHeight = 800;
    f.depthWidth = f.motionWidth = 1536; f.depthHeight = f.motionHeight = 960;
    f.mvScaleX = 1; f.mvScaleY = -1;
    f.revision = 123;
    Host host("native detail test");
    const auto next = [&](unsigned long long ordinal) {
        f.frameNumber = f.present = ordinal;
        const auto want = host.Gate(f, []() -> const char* { return nullptr; });
        Require(want.wanted && want.temporal && want.withFg, "scene route admitted without picture-flow authority");
        host.Decide(f, want.wanted, want);
        return host.decision;
    };
    for (unsigned long long ordinal = 10; ordinal <= 12; ++ordinal)
    {
        next(ordinal);
        host.cadence.Captured(true);
        host.TemporalCaptured(host.decision.kind == Kind::Full);
    }
    // The initial model/policy reset and blocked predecessor are now past.
    Require(host.params.TemporalMode == 1 && host.params.DepthWidth == 1536 && host.params.MvScaleY == -1,
        "native depth storage and decoded NGX motion reach shader constants");
    Require(host.decision.historyUsable && host.params.TemporalHistoryWeight > 0,
        "native full-model feedback has real history weight");
    Require(!f.motionMatchesPicture && !f.pictureFlowReuseVerified && !f.pictureFlowFeedbackVerified,
        "native path needs no optical-flow capability");

    next(14); // source 13 was missed, even though FG is active
    Require(!host.decision.historyUsable && host.decision.kind == Kind::Full &&
        !host.decision.composeMotion && host.decision.spreadStep == -1,
        "native source gap cannot be learnt as generated presents or compose old motion");
    f.currentDepthVerified = false;
    Require(!host.Gate(f, []() -> const char* { return nullptr; }).wanted,
        "scene history still rejects unverified current depth");
    f.currentDepthVerified = true; f.nativeGuidePhaseVerified = false;
    Require(!host.Gate(f, []() -> const char* { return nullptr; }).wanted,
        "manual flip without consumer phase authority still rejected");
    State::Instance().currentFG = nullptr;
}

void IndependentFeedbackOwnsHistory(unsigned interval)
{
    Config cfg;
    cfg.DlssNrDetailReuse.value = true;
    cfg.DlssNrDetailReuseInterval.value = interval;
    HostFrame f;
    f.cfg = &cfg;
    f.finishedPicture = f.nativeTemporalRouteRequested = f.temporalRequested = true;
    f.independentFeedbackRequested = true;
    f.baseIdentityVerified = f.currentDepthVerified = f.nativeGuideContractVerified = true;
    f.nativeGuidePhaseVerified = true;
    Host host("independent detail test");
    const auto wanted = host.Gate(f, []() -> const char* { return nullptr; });
    Require(!wanted.wanted && !wanted.keep,
        "independent residual history owns a route regardless of the legacy interval");
    TestFg fg;
    State::Instance().currentFG = &fg;
    cfg.DlssNrDetailReuseWithFg.value = false;
    const auto held = host.Gate(f, []() -> const char* { return nullptr; });
    Require(!held.wanted && !held.keep,
        "FG hold cannot retain legacy history while independent feedback owns the route");
    f.nativeTemporalRouteRequested = false;
    const auto rejected = host.Gate(f, []() -> const char* { return nullptr; });
    Require(!rejected.wanted && !rejected.keep,
        "an unsupported independent route cannot fall back to legacy history");
    f.nativeTemporalRouteRequested = true;
    f.independentFeedbackRequested = false;
    const auto legacyHeld = host.Gate(f, []() -> const char* { return nullptr; });
    Require(!legacyHeld.wanted && legacyHeld.keep,
        "legacy-only FG hold still retains its own history until the bounded deadline");
    State::Instance().currentFG = nullptr;
}
}

int main()
{
    for (unsigned interval = 1; interval <= 8; ++interval)
    {
        Exercise(interval);
        IndependentFeedbackOwnsHistory(interval);
    }
    std::printf("NR native detail host CPU PASS checks=%u\n", checks);
}
