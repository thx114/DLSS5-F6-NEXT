// Compile with runtime/tests/nr-resident/stubs before OptiScaler on the include
// path. Only Config/State/status are fixtures; Gate, Decide and Cadence are real.
// The production GPU constants intentionally use explicit ABI alignment.
#pragma warning(push)
#pragma warning(disable:4324)
#include <dlssnr/DlssNrDetailReuseHost.h>
#pragma warning(pop)
#include <runtime/NrResident.h>
#include <shaders/dlssnr/DlssNr_FrameInfo.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace
{
using namespace DlssNrDetailReuse;
unsigned checks = 0;
void Check(bool value, const char* why)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "NR resident consumer FAIL: %s\n", why); std::exit(1); }
}

HostFrame ConsumerFrame(const DlssNrFrameInfo& frame, const Config& cfg, unsigned long long ordinal)
{
    HostFrame f;
    f.cfg = &cfg;
    f.finishedPicture = frame.FinishedPicture;
    f.beforeUpscale = frame.BeforeUpscale;
    f.motionMatchesPicture = frame.MotionMatchesPicture;
    f.fullModelRequired = frame.FullModelRequired;
    f.modelReset = frame.Reset;
    f.workWidth = f.motionWidth = 1280;
    f.workHeight = f.motionHeight = 800;
    f.frameNumber = f.present = ordinal;
    f.revision = 123;
    return f;
}

void Exercise(unsigned interval, unsigned spread, bool reset)
{
    Config cfg;
    cfg.DlssNrDetailReuseInterval.value = interval;
    cfg.DlssNrDetailReuseSpread.value = spread;
    Host host("NR resident consumer");
    DlssNrFrameInfo picture;
    picture.FinishedPicture = picture.MotionMatchesPicture = picture.MotionIsPictureFlow = true;
    picture.OutputWidth = 2560; picture.OutputHeight = 1600;
    unsigned shaderChecks = 0;
    const auto ready = [&]() -> const char* { ++shaderChecks; return nullptr; };
    const auto next = [&](const DlssNrFrameInfo& frame, unsigned long long ordinal) {
        auto f = ConsumerFrame(frame, cfg, ordinal);
        const auto want = host.Gate(f, ready);
        host.Decide(f, want.wanted, want);
        return want;
    };

    // Default false preserves the existing no-depth finished-picture route.
    Check(!picture.FullModelRequired, "ordinary picture has unchanged reuse semantics");
    Check(next(picture, 10).wanted, "ordinary measured-motion Gate remains available");
    Check(host.decision.kind == Kind::Full && host.decision.captureHistory, "seed real cadence history");
    host.cadence.Captured(true);
    Check(next(picture, 11).wanted, "ordinary next picture remains admitted");
    Check(host.decision.kind == Kind::Reuse && host.decision.historyUsable, "positive control actually skips model");
    host.cadence.Captured(true);
    host.Finish(ConsumerFrame(picture, cfg, 11), true, true, true);

    DlssNrFrameInfo resident = picture;
    resident.FreshColorResetFallback = true;
    FramePipeline::NrResident::UseMeasuredFlow(resident, reset);
    Check(resident.MotionMatchesPicture && !resident.FreshColorResetFallback,
          "measured flow still has the legacy Gate's former admission bits");
    const auto shaderBefore = shaderChecks;
    for (unsigned long long ordinal = 12; ordinal < 20; ++ordinal)
    {
        const auto want = next(resident, ordinal);
        Check(!want.wanted && !want.keep, "resident full-model Gate rejects reuse even with warm history");
        Check(shaderChecks == shaderBefore, "resident Gate does not build reuse shader");
        Check(host.decision.kind == Kind::Full && !host.decision.historyUsable && !host.decision.captureHistory,
              "actual cadence demands fresh model without reading or publishing reused detail");
        Check(!host.decision.composeMotion && !host.decision.saveMotion && !host.decision.accumulateMotion &&
              !host.decision.saveShown && host.decision.spreadStep == -1,
              "no composed old motion or displayed spread survives route switch");
        const auto f = ConsumerFrame(resident, cfg, ordinal);
        const bool realign = host.Finish(f, false, false, false);
        if (ordinal == 12) Check(realign == !reset, "existing post-skip model realignment is preserved");
        host.cadence.Captured(true); // cannot manufacture history while disabled
    }

    // Leaving the resident route must seed a new full answer before reuse.
    Check(next(picture, 20).wanted, "ordinary route can resume");
    Check(host.decision.kind == Kind::Full && !host.decision.historyUsable,
          "old pre-resident history cannot cross back into the ordinary route");
    host.cadence.Captured(true);
    next(picture, 21);
    Check(host.decision.kind == Kind::Reuse, "ordinary configured cadence resumes after fresh capture");
}

void CheckUnchangedSourceFlow(bool zzz)
{
    Config cfg;
    Host host("existing source-flow consumer");
    HostFrame f;
    f.cfg = &cfg;
    f.finishedPicture = f.motionMatchesPicture = true;
    f.pictureTemporalRouteRequested = f.pictureFlowReuseRequested = f.pictureFlowReuseVerified = true;
    f.baseIdentityVerified = true;
    f.sceneGeometryVerified = zzz;
    f.lightEditFeedback = zzz;
    f.workWidth = f.motionWidth = 1280;
    f.workHeight = f.motionHeight = 800;
    f.frameNumber = 100;
    const auto ready = []() -> const char* { return nullptr; };
    auto want = host.Gate(f, ready);
    Check(want.wanted && want.temporal, "existing Genshin/ZZZ source-flow Gate stays admitted");
    Check(TemporalGuideAuthority(f), "positive fixture uses authenticated no-depth source-flow authority");
    host.Decide(f, want.wanted, want);
    Check(host.decision.kind == Kind::Full && !host.decision.historyUsable && host.decision.captureHistory,
          "fresh temporal route starts with a blocked full capture");
    host.cadence.Captured(true);
    ++f.frameNumber;
    want = host.Gate(f, ready);
    Check(want.wanted && want.temporal, "source-flow Gate remains admitted during warmup");
    host.Decide(f, want.wanted, want);
    // Policy::Next marks the first temporal frame blocked (no prior temporal history).
    // Cadence deliberately runs the frame AFTER a blocked capture in full too;
    // Captured(true) publishes a logical answer, not permission to erase that guard.
    Check(host.decision.kind == Kind::Full && !host.decision.historyUsable && host.decision.captureHistory,
          "frame after the fresh blocked capture also evaluates the full model");
    host.cadence.Captured(true);
    ++f.frameNumber;
    want = host.Gate(f, ready);
    Check(want.wanted && want.temporal, "source-flow Gate admits the warmed adjacent source");
    host.Decide(f, want.wanted, want);
    Check(host.decision.kind == Kind::Reuse && host.decision.historyUsable,
          "existing source-flow retains configured model skipping after both full warmup frames");
}
}

int main()
{
    for (unsigned interval = 2; interval <= 8; ++interval)
        for (unsigned spread = 1; spread <= 3; ++spread)
            for (const bool reset : {false, true}) Exercise(interval, spread, reset);
    CheckUnchangedSourceFlow(false);
    CheckUnchangedSourceFlow(true);
    std::printf("NrResidentConsumer: %u actual Gate/cadence CPU checks passed\n", checks);
}
