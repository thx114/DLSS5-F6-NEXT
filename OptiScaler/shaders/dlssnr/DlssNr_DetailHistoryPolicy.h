// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>

// Pure policy for complete-model, source-by-source residual feedback. No SDK,
// Config, diagnostics, command submission or completion authority lives here.
namespace NrDetailHistoryPolicy
{
inline bool RequestsPresentedFeedback(bool enabled, bool unsupportedBackend) noexcept
{
    return enabled && !unsupportedBackend;
}

inline bool RequiresFullModelEvaluation(bool legacyReuseEnabled, uint32_t legacyInterval,
                                       bool presentedFeedbackEnabled) noexcept
{
    return presentedFeedbackEnabled || (legacyReuseEnabled && legacyInterval > 1);
}

inline constexpr uint64_t ByteLimit = 512ull * 1024 * 1024;
inline constexpr uint32_t ResolveMode = 24;
enum class Placement : uint32_t { FinishedCanonical = 1, PreSrRaster = 2 };
enum class TimeDomain : uint32_t { SourceCaptureMilliseconds = 1, ConsumerSourceEntrySeconds = 2 };

inline bool ValidSerial(uint64_t n) { return n != 0 && n != UINT64_MAX; }
inline uint32_t FloatBits(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}
inline bool FitsBudget(uint64_t active, uint64_t retired)
{
    return active <= ByteLimit && retired <= ByteLimit - active;
}

struct Settings
{
    // Zero disables long-detail carry; the independent short stabilizer may still run.
    float durationSeconds = 0, maxAgeSeconds = 0, strength = 0;
    float stabilitySeconds = 0;
    bool Enabled() const { return (durationSeconds > 0 && strength > 0) || stabilitySeconds > 0; }
    bool Valid() const
    {
        return std::isfinite(stabilitySeconds) && stabilitySeconds >= 0 && std::isfinite(durationSeconds) && durationSeconds >= 0 &&
            std::isfinite(maxAgeSeconds) && maxAgeSeconds > 0 &&
            std::isfinite(strength) && strength >= 0 && strength < 1;
    }
};

struct PhaseFacts
{
    bool requested = false, preSr = false, nativeScene = false;
    bool beforeUpscale = false, finishedPicture = false;
    bool baseIdentity = false, currentDepth = false, consumerMappingVerified = false;
    bool routeConvention = false, metadataKnown = false, flagsKnown = false;
    bool jitterKnown = false, motionJitterKnown = false, motionJittered = false;
    bool motionLowResolution = false, unsupported = false;
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t motionWidth = 0, motionHeight = 0, depthWidth = 0, depthHeight = 0;
    float jitterX = 0, jitterY = 0, currentJitterUvX = 0, currentJitterUvY = 0;
    float motionPhaseX = 0, motionPhaseY = 0, depthPhaseX = 0, depthPhaseY = 0;
    float cancelX = 0, cancelY = 0, decodedScaleX = 0, decodedScaleY = 0;
};
struct Phase
{
    bool verified = false;
    Placement placement = Placement::FinishedCanonical;
    float jitterUvX = 0, jitterUvY = 0;
    const char* reason = "not-requested";
};
inline Phase ResolvePhase(const PhaseFacts& f)
{
    Phase p;
    const auto reject = [&](const char* reason) { p.reason = reason; return p; };
    if (!f.requested) return p;
    if (f.unsupported) return reject("unsupported-or-blocked-route");
    if (f.preSr ? (!f.beforeUpscale || f.finishedPicture) :
                  (!f.nativeScene || f.beforeUpscale || !f.finishedPicture))
        return reject("placement-contract");
    if (!f.baseIdentity || !f.currentDepth || !f.consumerMappingVerified || !f.routeConvention)
        return reject("current-source-or-consumer-contract");
    if (!f.metadataKnown || !f.flagsKnown || !f.jitterKnown || !f.motionJitterKnown)
        return reject("unknown-native-metadata");
    if (f.motionJittered || !f.motionLowResolution) return reject("unsupported-motion-convention");
    if (!f.renderWidth || !f.renderHeight || f.depthWidth != f.renderWidth ||
        f.depthHeight != f.renderHeight || f.motionWidth != f.renderWidth ||
        f.motionHeight != f.renderHeight) return reject("render-guide-extents");
    if (!std::isfinite(f.jitterX) || !std::isfinite(f.jitterY) ||
        std::abs(f.jitterX) > 0.5f || std::abs(f.jitterY) > 0.5f ||
        !std::isfinite(f.decodedScaleX) || !std::isfinite(f.decodedScaleY) ||
        f.decodedScaleX == 0 || f.decodedScaleY == 0)
        return reject("invalid-jitter-or-motion-scale");
    p.jitterUvX = f.jitterX / f.renderWidth;
    p.jitterUvY = f.jitterY / f.renderHeight;
    if (f.preSr && (f.currentJitterUvX != p.jitterUvX || f.currentJitterUvY != p.jitterUvY))
        return reject("raster-jitter-units");
    const float x = f.preSr ? 0.0f : p.jitterUvX;
    const float y = f.preSr ? 0.0f : p.jitterUvY;
    if (f.motionPhaseX != x || f.motionPhaseY != y || f.depthPhaseX != x || f.depthPhaseY != y ||
        f.cancelX != 0 || f.cancelY != 0) return reject("guide-address-phase");
    p.placement = f.preSr ? Placement::PreSrRaster : Placement::FinishedCanonical;
    p.verified = true;
    p.reason = f.preSr ? "pre-sr-raster" : "finished-canonical";
    return p;
}

struct Sample
{
    uint64_t ordinal = 0;
    double sourceSeconds = 0;
    float jitterUvX = 0, jitterUvY = 0;
    uint64_t captureEpoch = 0; // observation of this capture, NOT a stable feature generation
};
struct Identity
{
    uint64_t owner = 0, device = 0, queue = 0, revision = 0, sourceEpoch = 0;
    Placement placement = Placement::FinishedCanonical;
    TimeDomain timeDomain = TimeDomain::SourceCaptureMilliseconds;
    std::array<uint64_t, 3> sourceOwner {};
    // Effective/original rects, source flags, units, formats and normalization controls.
    std::array<uint64_t, 40> mapping {};
    std::array<uint64_t, 16> settings {};
    std::array<uint64_t, 8> exposure {};
    std::array<uint64_t, 23> scene {};
    bool operator==(const Identity& b) const
    {
        return std::tie(owner, device, queue, revision, sourceEpoch, placement, timeDomain, sourceOwner,
                        mapping, settings, exposure, scene) ==
               std::tie(b.owner, b.device, b.queue, b.revision, b.sourceEpoch, b.placement, b.timeDomain, b.sourceOwner,
                        b.mapping, b.settings, b.exposure, b.scene);
    }
};
template <size_t EncodingBytes> struct Key
{
    Identity identity;
    std::array<unsigned char, EncodingBytes> encoding {};
    Sample sample;
    // A recording owns its exact sample; equivalence deliberately excludes only
    // per-source ordinal/time/J. Never use a last-evaluated sample as a publisher.
    bool operator==(const Key& b) const { return identity == b.identity && encoding == b.encoding; }
};

struct SourceStep
{
    uint64_t previousObserved = 0;
    bool observed = false;
    bool Names(uint64_t published, uint64_t current) const
    {
        return observed && ValidSerial(published) && ValidSerial(current) &&
            previousObserved == published && published + 1 == current;
    }
};
class SourceSequence
{
    uint64_t last_ = 0;
    bool seen_ = false;
public:
    SourceStep Observe(uint64_t ordinal)
    {
        SourceStep step { last_, seen_ };
        last_ = ordinal; seen_ = true;
        return step;
    }
    void Reset() { last_ = 0; seen_ = false; }
};
struct History
{
    bool usable = false;
    float deltaSeconds = 0, rasterOffsetUvX = 0, rasterOffsetUvY = 0;
    const char* reason = "no-published-history";
};
template <class KeyType>
History Select(const KeyType& current, const KeyType& published, bool hasPublished,
               bool reset, const SourceStep& step, const Settings& settings)
{
    History h;
    if (!settings.Valid() || !settings.Enabled())
    { h.reason = "fresh-only-settings"; return h; }
    if (reset) { h.reason = "reset"; return h; }
    if (!hasPublished) return h;
    if (!(current == published)) { h.reason = "contract-changed"; return h; }
    if (!step.Names(published.sample.ordinal, current.sample.ordinal))
    { h.reason = "source-not-adjacent"; return h; }
    const double dt = current.sample.sourceSeconds - published.sample.sourceSeconds;
    if (!std::isfinite(current.sample.sourceSeconds) || !std::isfinite(published.sample.sourceSeconds) ||
        !std::isfinite(dt) || dt <= 0 || dt > std::numeric_limits<float>::max())
    { h.reason = "source-time-not-forward"; return h; }
    h.deltaSeconds = static_cast<float>(dt);
    if (!(h.deltaSeconds > 0)) { h.reason = "source-time-underflow"; return h; }
    if (current.identity.placement == Placement::PreSrRaster)
    {
        h.rasterOffsetUvX = published.sample.jitterUvX - current.sample.jitterUvX;
        h.rasterOffsetUvY = published.sample.jitterUvY - current.sample.jitterUvY;
        if (!std::isfinite(h.rasterOffsetUvX) || !std::isfinite(h.rasterOffsetUvY))
        { h.reason = "invalid-published-jitter"; return h; }
    }
    h.usable = true;
    h.reason = "adjacent-published-source";
    return h;
}

// CPU oracle for the shader age contract, not a replacement pixel implementation.
// Every nonzero tap must pass; callers take MAX accepted age, never average/min.
inline bool CarryAge(float savedAge, float delta, const Settings& s, float& ageNow)
{
    ageNow = -1;
    if (!s.Valid() || s.durationSeconds == 0 || !std::isfinite(savedAge) || savedAge < 0 ||
        !std::isfinite(delta) || delta <= 0) return false;
    const float age = savedAge + delta;
    if (!std::isfinite(age) || age >= s.maxAgeSeconds) return false;
    ageNow = age;
    return true;
}
} // namespace NrDetailHistoryPolicy
