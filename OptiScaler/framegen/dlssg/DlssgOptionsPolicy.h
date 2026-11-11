#pragma once

#include "DlssgInputContinuity.h"
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

// CPU configuration acknowledgements only. No token, resource, fence or SDK
// extension is owned here; none of these values can retire an input or output.
namespace dlssg::options
{
inline bool SameType(const sl::StructType& a, const sl::StructType& b)
{
    if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) return false;
    for (unsigned i = 0; i < 8; ++i)
        if (a.data4[i] != b.data4[i]) return false;
    return true;
}

template<typename T> bool Schema(const T& value, uint32_t version)
{
    return !value.next && value.structVersion == version && SameType(value.structType, T::s_structType);
}

inline bool Known(const sl::DLSSGOptions& o)
{
    return Schema(o, sl::kStructVersion5) && !o.onErrorCallback &&
        (o.mode == sl::DLSSGMode::eOff || o.mode == sl::DLSSGMode::eOn ||
         o.mode == sl::DLSSGMode::eAuto || o.mode == sl::DLSSGMode::eDynamic) &&
        (o.mode == sl::DLSSGMode::eOff || (o.numFramesToGenerate >= 1 && o.numFramesToGenerate <= 5)) &&
        !(static_cast<uint32_t>(o.flags) & ~static_cast<uint32_t>(sl::DLSSGFlags::eAll)) &&
        o.queueParallelismMode == sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue &&
        (o.bReserved15 == sl::eInvalid || o.bReserved15 == sl::eFalse || o.bReserved15 == sl::eTrue) &&
        (o.enableUserInterfaceRecomposition == sl::eFalse || o.enableUserInterfaceRecomposition == sl::eTrue) &&
        std::isfinite(o.dynamicTargetFrameRate) && o.dynamicTargetFrameRate >= 0;
}

inline bool Same(const sl::DLSSGOptions& a, const sl::DLSSGOptions& b)
{
    // Complete public v5 payload. Do not compare padding or copy extension links.
    return a.mode == b.mode && a.numFramesToGenerate == b.numFramesToGenerate &&
        a.flags == b.flags && a.dynamicResWidth == b.dynamicResWidth &&
        a.dynamicResHeight == b.dynamicResHeight && a.numBackBuffers == b.numBackBuffers &&
        a.mvecDepthWidth == b.mvecDepthWidth && a.mvecDepthHeight == b.mvecDepthHeight &&
        a.colorWidth == b.colorWidth && a.colorHeight == b.colorHeight &&
        a.colorBufferFormat == b.colorBufferFormat && a.mvecBufferFormat == b.mvecBufferFormat &&
        a.depthBufferFormat == b.depthBufferFormat && a.hudLessBufferFormat == b.hudLessBufferFormat &&
        a.uiBufferFormat == b.uiBufferFormat && a.bReserved15 == b.bReserved15 &&
        a.queueParallelismMode == b.queueParallelismMode &&
        a.enableUserInterfaceRecomposition == b.enableUserInterfaceRecomposition &&
        a.dynamicTargetFrameRate == b.dynamicTargetFrameRate;
}

inline bool Known(const sl::ReflexOptions& o)
{
    return Schema(o, sl::kStructVersion1) &&
        (o.mode == sl::ReflexMode::eOff || o.mode == sl::ReflexMode::eLowLatency ||
         o.mode == sl::ReflexMode::eLowLatencyWithBoost);
}

inline bool Same(const sl::ReflexOptions& a, const sl::ReflexOptions& b)
{
    return a.mode == b.mode && a.frameLimitUs == b.frameLimitUs &&
        a.useMarkersToOptimize == b.useMarkersToOptimize && a.virtualKey == b.virtualKey &&
        a.idThread == b.idThread;
}

inline bool Running(const sl::DLSSGOptions& o) { return o.mode != sl::DLSSGMode::eOff; }
inline bool Running(const sl::ReflexOptions& o) { return o.mode != sl::ReflexMode::eOff; }

inline bool CanRecomposeUserInterface(bool hudlessTagAccepted, bool uiTagAccepted, bool currentSource)
{
    return hudlessTagAccepted && uiTagAccepted && currentSource;
}

struct ProviderOwner
{
    uint64_t instance = 0, lifecycle = 0;
    uintptr_t device = 0, queue = 0, swapchain = 0, api = 0, plugin = 0;
    uintptr_t setOptions = 0, getState = 0;
    uint32_t viewport = 0, width = 0, height = 0;
    bool Known() const
    {
        return instance && instance != UINT64_MAX && lifecycle && lifecycle != UINT64_MAX &&
            device && queue && swapchain && api && plugin && setOptions && getState && width && height;
    }
    bool operator==(const ProviderOwner&) const = default;
};

struct Scope
{
    ProviderOwner provider {};
    InputOwner input {};
    uint64_t sourceGeneration = 0;
    uintptr_t reflexPlugin = 0, reflexSetOptions = 0;
    uint32_t inputPath = 0, outputPath = 0, interop = 0, scheduling = 0;
    uint32_t consumers = 0, providerCap = 0, forceReflex = 0, guideFlags = 0;
    uint32_t colorSpace = 0, transfer = 0, primaries = 0, range = 0, colorModel = 0;
    bool colorSpaceValid = false, enabled = false, markers = false, boost = false;
    bool sharedSdk = false, external = false, secondDevice = false;
    bool operator==(const Scope&) const = default;
};

enum class SubmissionPolicy { OnChange, EveryDispatch };

template<typename T> struct Acknowledgement
{
    T value {};
    Scope scope {};
    uint64_t sourceFrame = 0, epoch = 0;
    bool valid = false;
};

struct Submission
{
    sl::Result sdkResult = sl::Result::eErrorInvalidState;
    bool submitted = false, confirmed = false;
    sl::Result Result() const
    {
        return sdkResult == sl::Result::eOk && !confirmed ? sl::Result::eErrorInvalidState : sdkResult;
    }
};

class ConfigurationState
{
    uint64_t serial = 1;
    uint64_t epoch = 1;
    bool inFlight = false;

  public:
    uint64_t Serial() const { return serial; }
    bool InFlight() const { return inFlight; }
    void Invalidate()
    {
        if (serial != UINT64_MAX) ++serial;
        if (epoch != UINT64_MAX) ++epoch;
    }

    // Release is independent of source admission: it must still run after FG
    // has been disabled or a source owner has gone away. Share the dispatch
    // transaction guard so an SDK callback cannot recursively submit Off,
    // certify a superseded return, or preserve a sibling On acknowledgement.
    template<typename T, typename Current, typename Submit>
    Submission Release(const T& value, Current&& current, Submit&& submit)
    {
        if (Running(value)) { Invalidate(); return {}; }
        // A refused recursive release did not call the SDK. The outer owner
        // predicate still detects real identity changes; a duplicate request
        // must not poison its successful Off acknowledgement.
        if (inFlight) return {};
        Acknowledgement<T> unused; // Off is never cached by Apply.
        return Apply(unused, Scope{}, value, 0, false, true, SubmissionPolicy::OnChange,
            std::forward<Current>(current), std::forward<Submit>(submit));
    }

    // Production Deactivate adapter: close the source generation once, even
    // when every SDK Off callback repeats Deactivate on the same owner.
    template<typename Close>
    bool Deactivate(bool logicalActive, bool& requested, Close&& close)
    {
        if (requested && !logicalActive) return false;
        requested = true; // Publish before any reentrant SDK call.
        close();
        return true;
    }

    template<typename T, typename Current, typename Submit>
    Submission Apply(Acknowledgement<T>& ack, const Scope& scope, const T& value,
        uint64_t sourceFrame, bool cacheEligible, bool force, SubmissionPolicy policy,
        Current&& current, Submit&& submit)
    {
        if (inFlight)
        {
            ack.valid = false;
            Invalidate();
            return {}; // Never call a non-thread-safe SDK recursively.
        }
        if (!current()) { ack.valid = false; Invalidate(); return {}; }
        const bool known = Known(value);
        const bool cacheable = known && Running(value) && cacheEligible && serial != UINT64_MAX && epoch != UINT64_MAX;
        constexpr uint64_t keepaliveSourceFrames = 600;
        if (cacheable && !force && policy == SubmissionPolicy::OnChange && ack.valid &&
            ack.epoch == epoch && ack.scope == scope && Same(ack.value, value) && sourceFrame >= ack.sourceFrame &&
            sourceFrame - ack.sourceFrame < keepaliveSourceFrames)
            return { sl::Result::eOk, false, true };

        ack.valid = false;
        if (!known || !Running(value)) Invalidate();
        else if (serial != UINT64_MAX) ++serial;
        const auto ticket = serial;
        inFlight = true;
        struct Finish
        {
            ConfigurationState& state;
            bool completed = false;
            ~Finish() { state.inFlight = false; if (!completed) state.Invalidate(); }
        } finish { *this };
        // On exceptions the acknowledgement stays invalid, the serial changes,
        // and the guard clears inFlight. Never swallow SDK/consumer exceptions.
        const auto result = submit();
        const bool stable = ticket != UINT64_MAX && ticket == serial && current();
        finish.completed = true;
        if (result != sl::Result::eOk || !stable || !known)
        {
            Invalidate(); // A recursive success cannot certify an outer failure.
            return { result, true, false };
        }
        if (cacheable)
        {
            ack.value = value;
            ack.scope = scope;
            ack.sourceFrame = sourceFrame;
            ack.epoch = epoch;
            ack.valid = true;
        }
        return { result, true, true };
    }
};

struct Selection
{
    int requestedGenerated = 0;
    uint32_t attemptedGenerated = 0;
    bool dynamic = false;
    explicit operator bool() const { return attemptedGenerated != 0; }
};

class GenerationState
{
  public:
    ProviderOwner capabilityOwner {}, acceptedOwner {};
    bool capabilityKnown = false, dynamicSupported = false, acceptedKnown = false;
    uint32_t rawMaxGenerated = 0, rawPresented = 0, rawStatus = 0;
    sl::Result stateResult = sl::Result::eErrorInvalidState;
    int requestedGenerated = 0;
    uint32_t attemptedGenerated = 0, acceptedGenerated = 0;
    uint64_t acceptedFrame = 0, acceptedGeneration = 0, acceptedEpoch = 0;
    sl::DLSSGMode acceptedMode = sl::DLSSGMode::eOff;
    sl::Result optionsResult = sl::Result::eErrorInvalidState;

    void InvalidateApplied()
    {
        acceptedKnown = false;
        acceptedOwner = {};
        acceptedGenerated = 0;
        acceptedFrame = acceptedGeneration = acceptedEpoch = 0;
        acceptedMode = sl::DLSSGMode::eOff;
    }
    void InvalidateCapability()
    {
        capabilityKnown = dynamicSupported = false;
        capabilityOwner = {};
        InvalidateApplied();
    }
    void Observe(const ProviderOwner& owner, const sl::DLSSGState& state, sl::Result result, bool current)
    {
        stateResult = result;
        rawMaxGenerated = state.numFramesToGenerateMax;
        rawPresented = state.numFramesActuallyPresented; // Interval count, including zero. Never a multiplier.
        rawStatus = static_cast<uint32_t>(state.status);
        capabilityKnown = current && owner.Known() && result == sl::Result::eOk && Schema(state, sl::kStructVersion4);
        capabilityOwner = capabilityKnown ? owner : ProviderOwner{};
        dynamicSupported = capabilityKnown && state.bIsDynamicMFGSupported == sl::eTrue;
        if (!capabilityKnown || (acceptedKnown && (acceptedOwner != owner ||
            acceptedGenerated > Maximum(owner) ||
            (acceptedMode == sl::DLSSGMode::eDynamic && !dynamicSupported)))) InvalidateApplied();
    }
    bool Current(const ProviderOwner& owner) const { return capabilityKnown && capabilityOwner == owner; }
    uint32_t Maximum(const ProviderOwner& owner) const
    {
        // Five generated + one real = 6x. A patch/config/UI hint never raises raw SDK max.
        return Current(owner) ? (std::min)(rawMaxGenerated, uint32_t{5}) : 0;
    }
    Selection Select(const ProviderOwner& owner, int requested, bool dynamic) const
    {
        const auto maximum = Maximum(owner);
        return { requested, maximum ? static_cast<uint32_t>((std::clamp)(requested, 1, static_cast<int>(maximum))) : 0,
                 dynamic && Current(owner) && dynamicSupported };
    }
    void Record(const Scope& scope, uint64_t frame, const Selection& selection,
                const sl::DLSSGOptions& options, const Submission& outcome)
    {
        requestedGenerated = selection.requestedGenerated;
        attemptedGenerated = options.numFramesToGenerate;
        optionsResult = outcome.sdkResult;
        InvalidateApplied();
        if (!outcome.confirmed || outcome.sdkResult != sl::Result::eOk || !Current(scope.provider) || !Known(options) ||
            (Running(options) && (!options.numFramesToGenerate || options.numFramesToGenerate > Maximum(scope.provider) ||
                options.numFramesToGenerate != selection.attemptedGenerated)) ||
            (options.mode == sl::DLSSGMode::eDynamic && !dynamicSupported)) return;
        acceptedKnown = true;
        acceptedOwner = scope.provider;
        acceptedFrame = frame;
        acceptedGeneration = scope.sourceGeneration;
        acceptedEpoch = scope.input.epoch;
        acceptedMode = options.mode;
        acceptedGenerated = Running(options) ? options.numFramesToGenerate : 0;
    }
    bool Accepted(const Scope& scope, uint64_t frame) const
    {
        return acceptedKnown && Current(scope.provider) && acceptedOwner == scope.provider &&
            acceptedFrame == frame && acceptedGeneration == scope.sourceGeneration && acceptedEpoch == scope.input.epoch;
    }
    uint32_t FixedTotal() const
    {
        // Dynamic configuration is a ceiling, not proof of a fixed applied total.
        return acceptedKnown && acceptedMode == sl::DLSSGMode::eOn ? acceptedGenerated + 1 : 0;
    }
};
}
