#pragma once
#include <cmath>
#include <cstdint>

namespace DlssNr::NativeGuidePhase
{
// Jitter is the NGX current raster offset in canonical upper-left render pixels.
// Row correction extracts raw subrects into canonical textures before this phase is used.
struct Inputs
{
    bool nativeRoute = false, freshColorFallback = false;
    bool baseIdentity = false, currentDepth = false, routeConvention = false;
    bool metadataKnown = false, flagsKnown = false, jitterKnown = false, motionJitterKnown = false;
    bool motionJittered = false, motionLowResolution = false;
    bool depthRowsFlip = false, motionRowsFlip = false, vectorYNegated = false;
    bool normalizedTexturesReady = false;
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t depthWidth = 0, depthHeight = 0, motionWidth = 0, motionHeight = 0;
    float jitterX = 0, jitterY = 0, scaleX = 0, scaleY = 0, decodedScaleX = 0, decodedScaleY = 0;
};
struct Contract
{
    bool verified = false;
    const char* reason = "unresolved";
    float motionX = 0, motionY = 0, depthX = 0, depthY = 0;
    float cancelX = 0, cancelY = 0;
};
inline Contract Resolve(const Inputs& f)
{
    Contract c;
    const auto reject = [&](const char* reason) { c.reason = reason; return c; };
    if (!f.nativeRoute || f.freshColorFallback) return reject("not-current-native-route");
    if (!f.baseIdentity) return reject("base-identity");
    if (!f.currentDepth) return reject("current-depth");
    if (!f.routeConvention) return reject("canonical-route-convention");
    if (!f.metadataKnown || !f.flagsKnown) return reject("unknown-metadata-or-flags");
    if (!f.jitterKnown || !f.motionJitterKnown) return reject("unknown-jitter");
    if (f.motionJittered) return reject("jittered-motion-unsupported");
    if (!f.motionLowResolution) return reject("non-low-resolution-motion");
    if (!f.renderWidth || !f.renderHeight ||
        f.depthWidth != f.renderWidth || f.depthHeight != f.renderHeight ||
        f.motionWidth != f.renderWidth || f.motionHeight != f.renderHeight)
        return reject("extent-contract");
    if ((f.depthRowsFlip || f.motionRowsFlip) && !f.normalizedTexturesReady)
        return reject("normalization-not-recorded");
    if (!std::isfinite(f.jitterX) || !std::isfinite(f.jitterY) ||
        std::abs(f.jitterX) > 0.5f || std::abs(f.jitterY) > 0.5f)
        return reject("invalid-current-jitter");
    if (
        !std::isfinite(f.scaleX) || !std::isfinite(f.scaleY) || f.scaleX == 0 || f.scaleY == 0 ||
        f.decodedScaleX != f.scaleX || f.decodedScaleY != (f.vectorYNegated ? -f.scaleY : f.scaleY))
        return reject("vector-decoding-contract");
    c.motionX = c.depthX = f.jitterX / f.renderWidth;
    c.motionY = c.depthY = f.jitterY / f.renderHeight;
    // Declared nonjittered MV: row extraction never changes vector components,
    // and the independent decoded sign must not change a canonical sample phase.
    c.verified = std::isfinite(c.motionX) && std::isfinite(c.motionY);
    c.reason = c.verified ? "canonical-ngx-after-row-normalization" : "nonfinite-derived-phase";
    return c;
}
}
