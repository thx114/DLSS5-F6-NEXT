#pragma once

// Reuse detail between frames: the part both backends share (menu: "Reuse detail between frames"; ini DetailReuse*).
//
// DlssNr_DetailReuse.inl (D3D12) and DlssNr_DetailReuse_Vk.inl (Vulkan) own the textures and record the GPU work. What
// does not depend on the API lives here, once: whether reuse may run this frame and why not, the frame rate gate, the
// cadence and its decision, the shader constants, the recent GPU times and the status the menu reads. The cadence itself
// is DlssNrDetailReuse.h.

#include <Config.h>
#include <State.h>

#include <dlssnr/DlssNrDetailReuse.h>
#include <dlssnr/DlssNrTemporalDetailPolicy.h>
#include <dlssnr/DlssNrTemporalHandoff.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNr_GameDefaults.h>
#include <shaders/dlssnr/DlssNr_DetailReuseConstants.h>
#include <shaders/dlssnr/DlssNr_FinishedReady.h>
#include <shaders/dlssnr/DlssNr_ProxyCurve.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

namespace DlssNrDetailReuse
{

// What the backend knows about this frame, less its API's own objects.
struct HostFrame
{
    const Config* cfg = nullptr;
    bool beforeUpscale = false;   // NR runs before SR
    bool finishedPicture = false; // NR runs on the finished picture
    bool motionMatchesPicture = false; // the vectors were measured on this picture (DlssNrFrameInfo::MotionMatchesPicture)
    bool fullModelRequired = false; // valid measured motion does not grant output/detail reuse
    unsigned int workWidth = 0, workHeight = 0;
    unsigned int motionWidth = 0, motionHeight = 0, motionBaseX = 0, motionBaseY = 0;
    unsigned int motionAllocWidth = 0, motionAllocHeight = 0; // the motion texture itself
    unsigned int depthWidth = 0, depthHeight = 0, depthBaseX = 0, depthBaseY = 0;
    bool depthInverted = false;
    float mvScaleX = 1.0f, mvScaleY = 1.0f; // raw * scale is motion-subrect pixels (DX12 derives it from model units)
    bool nativeTemporalRouteRequested = false; // verified StarRail native scene route; DetailReuse still owns enablement
    // Independent full-model feedback. These fields never authorize interval
    // reuse or modify the motion passed to the NR model / DLSS / FG.
    bool independentFeedbackRequested = false;
    bool nativePreSrRoute = false;
    bool preSrProducerQueueVerified = false;
    float currentRasterJitterUvX = 0, currentRasterJitterUvY = 0;
    std::array<uint64_t, 3> preSrOwner {};
    bool temporalRequested = false; // opt-in persistent full-refresh feedback only
    bool lightEditFeedback = false; // StarRail/ZZZ: positive-light full-model edit, regional tone/detail feedback
    bool pictureTemporalRouteRequested = false; // actual Genshin finished-picture producer, not native scene MV
    bool pictureGuideContractVerified = false;  // exact current mapped depth + picture-flow/phase receipt
    bool pictureFlowFeedbackVerified = false; // exact no-depth picture-flow/trust, full model interval 1 only
    bool pictureFlowReuseRequested = false; // producer claim; failed consumer mapping must not fall back to legacy reuse
    bool sceneGeometryVerified = false; // ZZZ conditional scene-depth veto; never picture-depth authority
    bool pictureFlowReuseVerified = false; // distinct ordinary source interval-2..8 flow/confidence chain
    uint64_t pictureGuideEpoch = 0, pictureGuideCutSerial = 0; // stable producer lifetime/cut, not per-frame dt
    bool nativeGuidePhaseVerified = false;
    float nativeMotionPhaseUvX = 0, nativeMotionPhaseUvY = 0;
    float nativeDepthPhaseUvX = 0, nativeDepthPhaseUvY = 0;
    float nativeMotionJitterCancelUvX = 0, nativeMotionJitterCancelUvY = 0;
    bool temporalRecordingBlocked = false;
    bool nativeGuideContractVerified = false; // caller proves native scene MV/depth coordinate and jitter contract
    bool baseIdentityVerified = false;        // caller proves this is the corresponding real base picture
    bool currentDepthVerified = false;        // no old/synthetic/current-colour stand-in
    bool modelReset = false;                // the model starts over this frame
    bool blocked = false;                   // Tune or frame hold: the real model must run
    bool edgeCompression = false;           // Compress screen edges is packing the picture: the two do not run together
    unsigned long long frameNumber = 0;     // the present counter, or NR's own frame count
    unsigned long long present = 0;         // the API's frame clock (DXGI presents, DlssNr::VkFrameClock on Vulkan)
    bool vulkan = false;                    // which DLSS-G stamp goes with that clock
    bool unsupportedVulkanGameOrigin = false; // reject Vulkan-origin DX12 bridges without changing the clock
    unsigned long long revision = 0;        // changes when anything that changes the model's answer changes
    bool passthrough = false;               // the frame is already tone-mapped: no proxy curve, nothing to decode
    uint32_t reversibleMode = 0;            // the proxy curve this frame's input was encoded with (DlssNr_ProxyCurve.h)
    // Display only: copied from the backend's existing actual-call ledger.
    // No admission/history decision reads these observations.
    unsigned long long modelCalls = 0, modelFailures = 0;
    bool modelCallCountsKnown = false;
};

inline bool NativeGuideAuthority(const HostFrame& f)
{
    return f.nativeGuideContractVerified && f.nativeGuidePhaseVerified &&
        std::isfinite(f.nativeMotionPhaseUvX) && std::isfinite(f.nativeMotionPhaseUvY) &&
        std::isfinite(f.nativeDepthPhaseUvX) && std::isfinite(f.nativeDepthPhaseUvY) &&
        std::isfinite(f.nativeMotionJitterCancelUvX) && std::isfinite(f.nativeMotionJitterCancelUvY);
}

// The picture route already produces unjittered, current -> previous picture-pixel guides.
// Its separate capability must never manufacture NativeSceneRoute/NGX jitter authority.
inline bool TemporalGuideAuthority(const HostFrame& f)
{
    const bool fullFlowFeedback = f.pictureFlowFeedbackVerified && f.temporalRequested && f.cfg &&
        f.cfg->DlssNrDetailReuseInterval.value_or_default() == 1;
    const bool sourceFlowReuse = f.pictureFlowReuseVerified && !f.vulkan && f.cfg &&
        f.cfg->DlssNrDetailReuse.value_or_default() && f.cfg->DlssNrDetailReuseInterval.value_or_default() > 1 &&
        f.cfg->DlssNrDetailReuseInterval.value_or_default() <= 8;
    return NativeGuideAuthority(f) || (f.pictureTemporalRouteRequested && f.motionMatchesPicture &&
        f.baseIdentityVerified && ((f.pictureGuideContractVerified && f.currentDepthVerified) ||
        (!f.currentDepthVerified && (fullFlowFeedback || sourceFlowReuse))));
}

// Refine an existing route rejection for display/logging only. The acceptance predicate stays in Gate.
inline const char* NativeRouteRejection(const HostFrame& f, const char* reason)
{
    if (reason == nullptr || f.beforeUpscale || !f.finishedPicture || !f.nativeTemporalRouteRequested)
        return reason;
    if (!f.baseIdentityVerified)
        return "native scene reuse: corresponding base frame is not verified";
    if (!f.currentDepthVerified)
        return "native scene reuse: current depth is not verified";
    if (!f.nativeGuidePhaseVerified)
    {
        if (f.cfg != nullptr && (f.cfg->DlssNrMotionTextureFlipY.value_or_default() ||
                                f.cfg->DlssNrMotionYInverted.value_or_default()))
            return "native scene reuse: manual motion mapping has no verified phase";
        return "native scene reuse: guide jitter phase is not verified";
    }
    if (!f.nativeGuideContractVerified)
        return "native scene reuse: guide coordinate contract is not verified";
    if (!NativeGuideAuthority(f))
        return "native scene reuse: guide phase contains non-finite values";
    return reason;
}

// The curve the resolve decodes a Replace answer through, for a proxy curve on a frame that is (passthrough) or is not
// already tone-mapped. Every Replace curve needs one here, or reuse would land its moved changes as differences.
constexpr DlssNrReplaceCurve ReplaceCurveFor(uint32_t reversibleMode, bool passthrough)
{
    if (passthrough || !DlssNrProxyCurve::IsReplace(reversibleMode))
        return DlssNrReplaceCurve_None;
    return reversibleMode == DlssNrProxyCurve::kNeutwoReplace ? DlssNrReplaceCurve_Neutwo : DlssNrReplaceCurve_Hybrid;
}

constexpr bool ReplaceCurvesMatch()
{
    for (uint32_t mode = 0; mode < DlssNrProxyCurve::kCount; ++mode)
    {
        if ((ReplaceCurveFor(mode, false) != DlssNrReplaceCurve_None) != DlssNrProxyCurve::IsReplace(mode) ||
            ReplaceCurveFor(mode, true) != DlssNrReplaceCurve_None)
            return false;
    }
    return ReplaceCurveFor(DlssNrProxyCurve::kNeutwoReplace, false) == DlssNrReplaceCurve_Neutwo &&
           ReplaceCurveFor(DlssNrProxyCurve::kBalancedReplace, false) == DlssNrReplaceCurve_Hybrid;
}
static_assert(ReplaceCurvesMatch(), "a Replace proxy curve with no reuse curve (or the reverse)");

// What Gate decided: whether reuse's textures are wanted this frame, whether they may stay while it is held off, and
// the settings they follow.
struct Wanted
{
    bool wanted = false;
    bool keep = false;
    bool withFg = false;  // kept on under frame generation (A/B testing)
    bool measure = false; // measure how much of the frame had no detail to move (MotionGuard is on)
    bool hold = false;    // that measurement says the picture is moving too fast: run the model this frame
    bool temporal = false;
    float steady = 0.0f;
    float fill = 0.0f;
    unsigned int interval = 2; // the model every N frames (2 or 3)
    unsigned int spread = 1;   // frames the full frame's jump is spread over (1 = none)
};


// Frame generation is built from two real frames, so reused frames next to full ones show under it: the edges of the
// screen flickered in fast motion (NBA 2K27 with OptiFG 2026-09-28, The Witcher 3 2026-09-30) until MotionGuard stopped
// reuse from running through motion it cannot carry detail across. DetailReuseWithFG (on by default) keeps reuse running
// here; turned off, any of these reasons stands reuse down. Returns the reason, or null when none is seen: OptiScaler's own (active, not paused), the game's DLSS-G seen through NGX, or one owned by
// an external module.
inline const char* FrameGenerationInUse(unsigned long long present, bool vulkan)
{
    auto& state = State::Instance();
    if (state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused())
        return "off while frame generation is on";
    // A DLSS-G evaluate within the last 30 presents that generates frames (or does not say how many: DLSS-G builds
    // without multi frame generation leave the count out). PresentsSince reads "never" as endlessly long ago.
    // Each API's stamp against its own clock.
    const bool generates = vulkan ? state.dlssgLastEvaluateGeneratesVk.load() : state.dlssgLastEvaluateGenerates;
    const unsigned long long stamp = vulkan ? state.dlssgLastEvaluateFrameVk.load() : state.dlssgLastEvaluateFrame;
    if (generates && DlssNr::PresentsSince(present, stamp) <= 30)
        return "off while the game's frame generation is on";
    if (state.externalFrameGeneration)
        return "off while an external frame generation owns the frames";
    return nullptr;
}

class Host
{
  public:
    explicit Host(const char* logName) : _logName(logName) {}

    Cadence cadence;
    Decision decision;
    DlssNrDetailReuseConstants params {};

    // Why reuse cannot run this frame, then what its textures should do. Measures the rendered frame rate whether or
    // not reuse runs, so the gate knows when to let it back. shaderReady() is asked last, and only when reuse is on:
    // it may build the shader. It returns null when the shader is ready, else why not.
    template <class ShaderReady> Wanted Gate(const HostFrame& f, ShaderReady&& shaderReady)
    {
        const Config& cfg = *f.cfg;
        const bool on = cfg.DlssNrDetailReuse.value_or_default();
        // A failed allocation is retried once the option is switched off and on, or at another working size.
        if ((_allocFailed || _extrasFailed) && (!on || f.workWidth != _failedWidth || f.workHeight != _failedHeight))
            _allocFailed = _extrasFailed = false;
        // Under frame generation it runs unless asked not to (DetailReuseWithFG, on by default).
        const char* fg = FrameGenerationInUse(f.present, f.vulkan);
        Wanted w;
        w.withFg = fg != nullptr && cfg.DlssNrDetailReuseWithFg.value_or_default();
        // NR runs once per rendered frame, so the time between two calls is the rendered frame rate, frame generation
        // or not.
        const auto now = std::chrono::steady_clock::now();
        const double sinceLast = _lastNrFrame.time_since_epoch().count() != 0
                                     ? std::chrono::duration<double>(now - _lastNrFrame).count()
                                     : 0.0;
        _lastNrFrame = _thisNrFrame = now;
        const bool fastEnough = _rateGate.Update(sinceLast, cfg.DlssNrDetailReuseMinFps.value_or_default());
        // How much of the picture arrived with no detail to move, from the GPU a few frames ago (RecordDropped). Read
        // once: a frame with no new reading keeps the guard's state.
        const float maxDropped = std::clamp(cfg.DlssNrDetailReuseMaxDropped.value_or_default(), 0.0f, 100.0f) / 100.0f;
        w.measure = maxDropped > 0.0f;
        w.hold = _motionGuard.Update(std::exchange(_dropped, -1.0f), maxDropped, sinceLast);
        if (!w.measure)
            _droppedLast = -1.0f; // nothing is being measured: the menu must not show an old reading
        const char* whyNot = f.fullModelRequired
                                 ? "current picture requires full model evaluation"
                                 : f.edgeCompression
                                 ? kEdgeCompression
                                 : UnavailableOnRoute(f.beforeUpscale, f.finishedPicture,
                                     f.motionMatchesPicture || (f.nativeTemporalRouteRequested && NativeGuideAuthority(f) &&
                                     f.baseIdentityVerified && f.currentDepthVerified));
        if ((f.vulkan || f.unsupportedVulkanGameOrigin) && whyNot == nullptr)
            whyNot = "detail reuse is disabled on Vulkan in this round";
        if (!f.edgeCompression)
            whyNot = NativeRouteRejection(f, whyNot);
        if (whyNot == nullptr)
        {
            if (fg != nullptr && !w.withFg)
                whyNot = fg;
            else if (!fastEnough)
                whyNot = kBelowMinimumFps;
            else if (_allocFailed)
                whyNot = "its history textures could not be allocated";
            else if (on)
                whyNot = shaderReady();
        }
        const bool feedback = f.temporalRequested && (f.nativeTemporalRouteRequested || f.pictureTemporalRouteRequested);
        const bool needsTemporalAuthority = feedback || f.pictureFlowReuseRequested;
        if (whyNot == nullptr && needsTemporalAuthority && !f.currentDepthVerified &&
            !f.pictureFlowFeedbackVerified && !f.pictureFlowReuseVerified)
            whyNot = "persistent feedback: current picture-mapped depth is not verified";
        else if (whyNot == nullptr && needsTemporalAuthority && !TemporalGuideAuthority(f))
            whyNot = "persistent feedback: motion/depth phase contract is not verified";
        _why = on && whyNot != nullptr ? whyNot : "";

        const std::string state = !_why.empty()       ? _why
                                  : on && w.withFg ? "available, kept on with frame generation"
                                                   : "available";
        if (state != _loggedWhy)
        {
            LOG_INFO("{}: {} (rendered frame rate {:.0f} fps)", _logName, state, _rateGate.Fps());
            _loggedWhy = state;
        }

        // Held off while the picture moves too fast: not logged as a state, it comes and goes with the motion (the
        // menu shows it, and the frame counts carry it).
        w.temporal = (f.nativeTemporalRouteRequested ||
            (f.pictureTemporalRouteRequested && (f.temporalRequested || f.pictureFlowReuseVerified))) &&
            f.finishedPicture && !f.beforeUpscale;
        w.steady = std::clamp(cfg.DlssNrDetailReuseSteady.value_or_default(), 0.0f, 1.0f);
        if (w.temporal && f.temporalRequested) w.steady = std::clamp(w.steady > 0.0f ? w.steady : 0.75f, 0.0f, 0.85f);
        w.fill = std::clamp(cfg.DlssNrDetailReuseFill.value_or_default(), 0.0f, 1.0f);
        w.interval = std::clamp(cfg.DlssNrDetailReuseInterval.value_or_default(), 1u, f.vulkan ? 3u : 8u);
        w.spread = SpreadFor(w.interval, cfg.DlssNrDetailReuseSpread.value_or_default());
        // Held off for now (frame generation, which often pauses and resumes, or the frame rate): the textures stay a
        // while.
        const bool heldOff = on && whyNot != nullptr && (whyNot == fg || whyNot == kBelowMinimumFps);
        _gatedFrames = heldOff ? _gatedFrames + 1 : 0;
        // Feedback does not skip the model: interval 1 still captures/fuses every real NR frame.
        // The selected consumer owns history even when its current guides are
        // rejected. Falling back to legacy reuse would hide that rejection and
        // feed an already-fused answer into the independent residual cache.
        const bool feedbackOwnsHistory = f.independentFeedbackRequested;
        w.wanted = on && !feedbackOwnsHistory && whyNot == nullptr &&
            (w.interval > 1 || (w.temporal && f.temporalRequested));
        w.keep = !feedbackOwnsHistory && heldOff && _gatedFrames <= kKeepGatedFrames;
        return w;
    }

    // An allocation failed at this working size: not retried until the size changes or the option is switched off
    // and on. extras: the estimate or the steadiness texture (reuse still runs without them).
    void AllocationFailed(const HostFrame& f, bool extras)
    {
        (extras ? _extrasFailed : _allocFailed) = true;
        _failedWidth = f.workWidth;
        _failedHeight = f.workHeight;
    }

    bool ExtrasFailed() const { return _extrasFailed; }

    // New textures: the recent GPU times were measured on the old ones.
    void TexturesRebuilt() { _gpuRecentCount = 0; }

    // Which frame this is (the cadence) and the shader constants for it.
    void Decide(const HostFrame& f, bool active, const Wanted& w)
    {
        FrameFacts facts;
        facts.enabled = active;
        const auto temporal = f.temporalRecordingBlocked
            ? DlssNrTemporalDetail::Decision { true, true, false, true, 0 } : _temporalPolicy.Next(active && w.temporal,
            f.currentDepthVerified && f.depthWidth && f.depthHeight,
            TemporalGuideAuthority(f), f.baseIdentityVerified, f.modelReset, f.frameNumber, 2,
            f.pictureFlowFeedbackVerified || f.pictureFlowReuseVerified);
        facts.reset = f.modelReset;
        if (temporal.enabled && !temporal.allowHistory) facts.blocked = true;
        facts.blocked = facts.blocked || f.blocked;
        const auto maxAgeMs = std::clamp(f.cfg->DlssNrPresentedTemporalMaxAgeMs.value_or_default(),
            Config::kPresentedTemporalMaxAgeMinMs, Config::kPresentedTemporalMaxAgeMaxMs);
        // Source-interval reuse also has a hard birth/deadline when feedback is off.
        // Reprojecting/capturing a reuse sample never renews that birth.
        const bool boundedFeedback = active && w.temporal && (f.temporalRequested || f.pictureFlowReuseVerified);
        // First slice: full-model interval 1, with no display Spread. Skipped-model
        // F6 flow/latent/confidence chains keep their existing cadence and hard TTL.
        const bool handoff = boundedFeedback && f.temporalRequested && !f.vulkan &&
            w.interval == 1 && w.spread == 1;
        const bool expired = boundedFeedback && !handoff && _temporalHistoryBorn.time_since_epoch().count() != 0 &&
            _thisNrFrame - _temporalHistoryBorn >= std::chrono::milliseconds(maxAgeMs);
        // Expiry forces real full evaluation even in an interval-2..8 reuse run.
        // Legacy routes keep composeMotion; source-flow reuse instead resets the model after skips.
        facts.hold = w.hold || temporal.forceFull || expired;
        facts.reuseRun = w.interval - 1;
        facts.spread = w.spread;
        // Without frame generation NR runs on every present, so any skipped present is a gap. With it, the present
        // counter also counts the generated frames: the step that is normal is learnt (StepTracker).
        facts.frame = f.frameNumber;
        const bool sourceClock = f.nativeTemporalRouteRequested || f.pictureFlowReuseVerified;
        if (!sourceClock) _steps.Add(f.frameNumber);
        // Native capture serials and successful SR source ordinals are not a physical FG Present clock.
        // A missing source is a real gap even when FG is active. Never learn it away.
        facts.maxStep = sourceClock ? 1ull : MaxStepFor(w.withFg, _steps.Typical());
        // The saved change is in the values of the proxy curve it was made in, so another curve (or a frame that turns
        // passthrough, or back) starts over like any other change to the model's answer.
        const DlssNrReplaceCurve replaceCurve = ReplaceCurveFor(f.reversibleMode, f.passthrough);
        facts.revision = f.revision;
        for (const unsigned long long part : { (unsigned long long) f.reversibleMode, f.passthrough ? 1ull : 0ull, w.temporal ? 1ull : 0ull, w.temporal && f.temporalRequested ? 1ull : 0ull })
            facts.revision = facts.revision * 1000003ull ^ part;
        facts.revision = facts.revision * 1000003ull ^ std::min(
            f.cfg->DlssNrPresentedTemporalDurationMs.value_or_default(), Config::kPresentedTemporalDurationMaxMs);
        facts.revision = facts.revision * 1000003ull ^ maxAgeMs;
        const float configuredStrength = f.cfg->DlssNrPresentedTemporalStrength.value_or_default();
        const float strength = std::isfinite(configuredStrength) ? std::clamp(configuredStrength, 0.0f, 0.95f) : 0.85f;
        facts.revision = facts.revision * 1000003ull ^ static_cast<unsigned long long>(strength * 1000000.0f);
        facts.revision = facts.revision * 1000003ull ^ (handoff ? 1ull : 0ull);
        decision = cadence.Next(facts);
        if (expired)
        {
            decision.historyUsable = false;
            decision.spreadStep = -1;
            decision.saveShown = false;
            cadence.EndSpread(); // display Spread must not bring expired detail back
        }
        if (!active || !w.temporal || !decision.historyUsable)
            _lastTemporalFullRefresh = _temporalHistoryBorn = {};

        params = {};
        params.WorkWidth = f.workWidth;
        params.WorkHeight = f.workHeight;
        params.MotionWidth = f.motionWidth;
        params.MotionHeight = f.motionHeight;
        params.MotionBaseX = f.motionBaseX;
        params.MotionBaseY = f.motionBaseY;
        params.DepthWidth = f.depthWidth;
        params.DepthHeight = f.depthHeight;
        params.DepthBaseX = f.depthBaseX;
        params.DepthBaseY = f.depthBaseY;
        params.DepthInverted = f.depthInverted ? 1u : 0u;
        // Backend-provided scale: raw times it is motion-subrect pixels; the shader divides by that same size.
        params.MvScaleX = f.mvScaleX;
        params.MvScaleY = f.mvScaleY;
        // The four values behind trust, live from the config so they can be found in a game rather than guessed.
        // Their defaults mirror the k constants in DlssNr_DetailReuseConstants.h, so an untouched install behaves
        // as before, with two exceptions: DepthTolerance's own default was raised at the same time (see there),
        // and it is now a per-game default rather than a single one -- DetailReuseDepthToleranceEffective
        // (DlssNr_GameDefaults.h) gives a short list of measured games (currently just RDR2) their own value
        // instead, unless the user set DetailReuseDepthTolerance themselves.
        params.DepthTolerance = DlssNr::DetailReuseDepthToleranceEffective(*f.cfg);
        params.ClipGamma = f.cfg->DlssNrDetailReuseClipGamma.value_or_default();
        params.ClipFalloff = f.cfg->DlssNrDetailReuseClipFalloff.value_or_default();
        params.SigmaFloor = f.cfg->DlssNrDetailReuseSigmaFloor.value_or_default();
        params.Steady = w.steady;
        params.FillStrength = w.fill;
        // Scaled with the working size, like the colour box: the same reach on screen at any model resolution.
        params.FillRadius = kDlssNrDetailReuseFillRadius1080p *
                            std::clamp(std::sqrt((float) f.workWidth * (float) f.workHeight / (1920.0f * 1080.0f)),
                                       0.5f, 2.0f);
        params.DebugView = f.cfg->DlssNrDetailReuseDebug.value_or_default() ? 1u : 0u;
        params.ReplaceCurve = replaceCurve;
        params.MotionReject = kDlssNrDetailReuseMotionReject;
        params.SteadyDeadZone = kDlssNrDetailReuseSteadyDeadZone;
        if (temporal.noAdvance) decision.captureHistory = false;
        // Flow-only feedback uses the existing colour/motion Capture+Estimate shader storage.
        // Native temporal storage deliberately zeros confidence without geometric support.
        // 2: ordinary no-depth source chain with latent/confidence capture; 1: native depth.
        // Other interval-1 flow feedback retains its legacy residual storage (0).
        params.TemporalMode = w.temporal && f.pictureFlowReuseVerified ? 2u :
            w.temporal && !f.pictureFlowFeedbackVerified ? 1u : 0u;
        // The new content layout is confined to the complete-model handoff.
        // Its flow history needs real support/confidence, not legacy validity=1.
        // Only domains with an existing exact positive-light encode/decode pair.
        // SDR is display sRGB; Replace uses its actual inverse. Lossy HDR composed
        // proxies keep their original residual contract, rather than guessing scene light.
        params.TemporalContent = handoff && f.lightEditFeedback &&
            (f.passthrough || replaceCurve != DlssNrReplaceCurve_None) ? 1u : 0u;
        if (params.TemporalContent != 0u && f.pictureFlowFeedbackVerified) params.TemporalMode = 2u;
        // Version 3 keeps measured picture flow, and requires conditional same-source
        // scene geometry on every estimate/capture, including interval-2..8 sources.
        if (w.temporal && f.sceneGeometryVerified &&
            (f.pictureFlowFeedbackVerified || f.pictureFlowReuseVerified)) params.TemporalMode = 3u;
        params.NativeMotionPhaseUvX = f.nativeMotionPhaseUvX;
        params.NativeMotionPhaseUvY = f.nativeMotionPhaseUvY;
        params.NativeDepthPhaseUvX = f.nativeDepthPhaseUvX;
        params.NativeDepthPhaseUvY = f.nativeDepthPhaseUvY;
        params.NativeMotionJitterCancelUvX = f.nativeMotionJitterCancelUvX;
        params.NativeMotionJitterCancelUvY = f.nativeMotionJitterCancelUvY;
        params.TemporalFrameKind = !decision.historyUsable ? 2u :
            decision.kind == Kind::Reuse ? 1u : 0u;
        params.MissingDepthRun = temporal.missingDepthRun;
        params.MaxMissingDepthRun = 2;
        params.TemporalHistoryWeight = 0.0f;
        if (w.temporal && f.temporalRequested)
        {
            const auto durationMs = std::min(f.cfg->DlssNrPresentedTemporalDurationMs.value_or_default(),
                                             Config::kPresentedTemporalDurationMaxMs);
            if (durationMs == 0 && decision.historyUsable) params.TemporalHistoryWeight = 0.75f;
            else if (decision.kind == Kind::Full && decision.historyUsable &&
                     _lastTemporalFullRefresh.time_since_epoch().count() != 0)
            {
                // Successful full captures, not every reuse sample or the FG Present clock.
                const double elapsedMs = std::chrono::duration<double, std::milli>(
                    _thisNrFrame - _lastTemporalFullRefresh).count();
                if (elapsedMs > 0.0 && std::isfinite(elapsedMs))
                    params.TemporalHistoryWeight = static_cast<float>(std::exp(-elapsedMs / durationMs));
            }
        }
        if (boundedFeedback)
        {
            params.TemporalHistoryWeight = std::min(params.TemporalHistoryWeight, strength);
            if (!handoff && _temporalHistoryBorn.time_since_epoch().count() != 0 &&
                _lastTemporalFullRefresh.time_since_epoch().count() != 0)
            {
                // Fused captures do not renew the birth time. Consecutive remaining-life
                // ratios telescope to zero at the deadline, independent of FPS and FG.
                const double previousAge = std::chrono::duration<double, std::milli>(
                    _lastTemporalFullRefresh - _temporalHistoryBorn).count();
                const double currentAge = std::chrono::duration<double, std::milli>(
                    _thisNrFrame - _temporalHistoryBorn).count();
                const double remaining = std::max(0.0, static_cast<double>(maxAgeMs) - currentAge);
                const double previousRemaining = static_cast<double>(maxAgeMs) - previousAge;
                const float lifetimeWeight = previousRemaining > 0.0
                    ? static_cast<float>(std::clamp(remaining / previousRemaining, 0.0, 1.0)) : 0.0f;
                params.TemporalHistoryWeight = std::min(params.TemporalHistoryWeight, lifetimeWeight);
            }
        }
        if (handoff)
        {
            const auto p = _temporalHandoff.Next(_thisNrFrame, maxAgeMs, decision.historyUsable);
            params.TemporalHandoff = 1;
            params.TemporalHandoffResetMask = p.resetMask;
            params.TemporalHandoffIncoming = p.incoming;
            params.TemporalHandoffBlend = p.blend;
        }
        else _temporalHandoff.Reset();
        params.MissingDepthHistoryCap = 0.65f;
        params.DepthReturnHistoryCap = 0.25f;
        params.MinimumFlowTrust = 0.95f; // reserved; native mode does not synthesize or require optical flow
        params.MinimumColourTrust = 0.95f;
    }

    // The end of the frame's decision. Returns whether the model's history must start over: it last ran two frames ago
    // but gets no composed vectors now (reuse was switched off, frame generation came on, the size changed, or
    // composing failed), so its history would be moved by one frame of motion.
    bool Finish(const HostFrame& f, bool active, bool reused, bool composedReadable)
    {
        bool resetModel = false;
        if (!reused && _modelSkipped && !composedReadable && !f.modelReset)
        {
            resetModel = true;
            if (!_loggedRealign)
            {
                _loggedRealign = true;
                LOG_INFO("{}: model history reset after a reused frame (no composed vectors)", _logName);
            }
        }
        _modelSkipped = reused;
        _activeLast = active;
        Publish(f);
        return resetModel;
    }

    void RecordGpuTime(double ms) { _gpuRecent[_gpuRecentCount++ % 16] = ms; }

    // The share of a measured frame (0..1) that had no detail to move, read back from the GPU. Several may arrive
    // between two frames: the largest is kept, since one flickering frame is seen. Summed up for the log, which speaks
    // every kDroppedLogSeconds and only about that window, so a threshold can be chosen from real play without the
    // line becoming spam (the reuse status line next to it is every ~10 s too).
    void RecordDropped(float share)
    {
        if (!(share >= 0.0f) || !(share <= 1.0f))
            return;
        _dropped = std::max(_dropped, share);
        _droppedLast = share;
        _droppedLow = _droppedSeen ? std::min(_droppedLow, share) : share;
        _droppedHigh = _droppedSeen ? std::max(_droppedHigh, share) : share;
        _droppedSum += share;
        ++_droppedSeen;
        const unsigned long long held = cadence.Held();
        const auto now = std::chrono::steady_clock::now();
        if (_droppedLogged.time_since_epoch().count() == 0)
        {
            _droppedLogged = now;
            _droppedHeld = held;
        }
        else if (std::chrono::duration<double>(now - _droppedLogged).count() >= kDroppedLogSeconds)
        {
            LOG_INFO("{}: over the last {:.0f}s, {:.1f}% of the picture had no detail to move on average of {} measured "
                     "frames ({:.1f}% to {:.1f}%); reuse paused on {} of them",
                     _logName, kDroppedLogSeconds, 100.0 * _droppedSum / _droppedSeen, _droppedSeen,
                     100.0 * _droppedLow, 100.0 * _droppedHigh, held - _droppedHeld);
            _droppedLogged = now;
            _droppedHeld = held;
            _droppedSeen = 0;
            _droppedSum = 0.0;
        }
    }

    DlssNr::DetailReuseInfo Status() const
    {
        DlssNr::DetailReuseInfo status { cadence.Full(), cadence.Reused(), cadence.Fallback(), cadence.Held(), _why };
        const unsigned int count = std::min(_gpuRecentCount, 16u);
        for (unsigned int i = 0; i < count; ++i)
        {
            status.lightMs = i == 0 ? _gpuRecent[i] : std::min(status.lightMs, _gpuRecent[i]);
            status.heavyMs = i == 0 ? _gpuRecent[i] : std::max(status.heavyMs, _gpuRecent[i]);
            status.averageMs += _gpuRecent[i] / count;
        }
        status.active = _activeLast;
        status.baseFps = _rateGate.Fps();
        status.holding = _motionGuard.Holding();
        status.dropped = _droppedLast;
        return status;
    }

    // For the menu, without the backend's own lock. present: the same counter the frames carried. A status not
    // published for kStalePresents reads as not active: NR stopped reaching the backend (NR turned off, the proxy
    // path, the game stops upscaling).
    DlssNr::DetailReuseInfo Published(unsigned long long present)
    {
        std::lock_guard<std::mutex> lock(_publishedMutex);
        DlssNr::DetailReuseInfo status = _published;
        if (present < _publishedPresent ||
            (present > _publishedPresent && present - _publishedPresent > kStalePresents))
        {
            status.active = false;
            status.modelCallCountsKnown = false;
        }
        return status;
    }

    // Shutdown: everything the textures' lifetime decided is forgotten with them.
    // Only a successfully recorded full-model capture advances this clock. Submission/history
    // failure still invalidates it through Decide/Discard; this is not GPU completion evidence.
    void TemporalCaptured(bool fullModel)
    {
        if (fullModel)
        {
            if (_temporalHistoryBorn.time_since_epoch().count() == 0)
                _temporalHistoryBorn = _thisNrFrame;
            _lastTemporalFullRefresh = _thisNrFrame;
            if (params.TemporalHandoff != 0) _temporalHandoff.Captured(_thisNrFrame);
        }
    }
    void DiscardTemporalRecording()
    { cadence.Drop(); _temporalPolicy = {}; _temporalHandoff.Reset();
      _lastTemporalFullRefresh = _temporalHistoryBorn = {}; }

    void Reset()
    {
        _allocFailed = _extrasFailed = _modelSkipped = _activeLast = false;
        _failedWidth = _failedHeight = _gatedFrames = 0;
        _dropped = _droppedLast = -1.0f;
        _droppedSeen = 0;
        _droppedHeld = 0;
        _droppedSum = 0.0;
        _droppedLogged = {};
        _lastNrFrame = {}; // the next frame measures no interval, rather than the whole time NR was away
        _thisNrFrame = _lastTemporalFullRefresh = _temporalHistoryBorn = {};
        _motionGuard = {};
        _temporalPolicy = {};
        _temporalHandoff.Reset();
        cadence.Drop();
        std::lock_guard<std::mutex> lock(_publishedMutex);
        _published = {};
    }

  private:
    static constexpr const char* kBelowMinimumFps = "off below the minimum frame rate";
    // The moved detail lives in the model's own picture; with the edges compressed that picture is packed, and the two
    // do not combine (yet).
    static constexpr const char* kEdgeCompression = "unavailable while Compress screen edges is on";
    DlssNrTemporalDetail::Policy _temporalPolicy;
    DlssNrTemporalDetail::Handoff _temporalHandoff;
    StepTracker _steps; // the frame number's normal step between two NR frames (see Decide)
    static constexpr unsigned long long kStalePresents = 30;
    // Frame generation pauses briefly and often (menu, loading, some games toggle it): the textures stay through this
    // many NR frames of it before they are released.
    static constexpr unsigned int kKeepGatedFrames = 300;

    // The status the menu reads, published at the end of each frame's decision under a lock of its own: the
    // backend's lock is held through the whole of NR's recording, and the menu must not wait on it every frame.
    void Publish(const HostFrame& frame)
    {
        auto status = Status();
        status.modelCalls = frame.modelCalls;
        status.modelFailures = frame.modelFailures;
        status.modelCallCountsKnown = frame.modelCallCountsKnown;
        std::lock_guard<std::mutex> lock(_publishedMutex);
        _published = std::move(status);
        _publishedPresent = frame.present;
    }

    const char* _logName;
    bool _allocFailed = false, _extrasFailed = false;
    unsigned int _failedWidth = 0, _failedHeight = 0; // the working size an allocation failed at
    bool _modelSkipped = false;                       // the model was skipped on the last NR frame
    bool _activeLast = false;
    unsigned int _gatedFrames = 0; // NR frames in a row with reuse held off for now (frame generation, low frame rate)
    FrameRateGate _rateGate;
    MotionGuard _motionGuard;
    float _dropped = -1.0f;     // this frame's reading for the guard, taken once (negative: none)
    float _droppedLast = -1.0f; // the newest reading, for the menu
    float _droppedLow = 0.0f, _droppedHigh = 0.0f;
    double _droppedSum = 0.0;
    unsigned int _droppedSeen = 0;
    unsigned long long _droppedHeld = 0; // Held() when the window started, so the line counts that window only
    std::chrono::steady_clock::time_point _droppedLogged {};
    static constexpr double kDroppedLogSeconds = 10.0;
    std::chrono::steady_clock::time_point _lastNrFrame {};
    std::chrono::steady_clock::time_point _thisNrFrame {}, _lastTemporalFullRefresh {}, _temporalHistoryBorn {};
    std::string _why;
    std::string _loggedWhy;
    bool _loggedRealign = false;
    double _gpuRecent[16] = {}; // the last 16 whole-pass GPU times: full and reused frames cost differently
    unsigned int _gpuRecentCount = 0;

    std::mutex _publishedMutex;
    DlssNr::DetailReuseInfo _published;
    unsigned long long _publishedPresent = 0;
};

} // namespace DlssNrDetailReuse
