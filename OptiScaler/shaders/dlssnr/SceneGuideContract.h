#pragma once

#include "DlssNr_FrameInfo.h"
#include "DlssNr_GuideCoordinates.h"

#include <array>
#include <cmath>
#include <cstring>

namespace DlssNr::SceneGuides
{
// Producer claims select a request, never grant admission. Native takes precedence:
// a retained-source claim must not add preparation work to the native endpoint.
enum class Request { None, NativeExisting, RetainedSource };
enum class Path { Rejected, NativeExisting, PrepareRetained };

constexpr Request Requested(const DlssNrFrameInfo& f)
{
    return f.NativeSceneRoute ? Request::NativeExisting :
        f.SceneGuideSourceVerified ? Request::RetainedSource : Request::None;
}

// The backend classifies the actual readable view, not a requested SDK format.
// Integer, compressed, unknown and unsigned motion encodings have no contract here.
enum class Samples { Unknown, DeviceDepth, SignedMotion };
struct Texture
{
    bool present = false, texture2D = false, shaderReadable = false;
    uint64_t width = 0;
    uint32_t height = 0, layers = 0, samples = 0, mipLevels = 0;
    Samples encoding = Samples::Unknown;
    uint32_t format = 0; // exact backend format, retained in the history key
};
struct Rect
{
    uint32_t x = 0, y = 0, width = 0, height = 0;
};
constexpr bool Fits(const Texture& t, const Rect& r)
{
    return t.present && t.texture2D && t.shaderReadable && t.layers == 1 && t.samples == 1 &&
        t.mipLevels == 1 && r.width && r.height && r.x < t.width && r.y < t.height &&
        r.width <= t.width - r.x && r.height <= t.height - r.y;
}

struct Consumer
{
    bool vulkanOrigin = false, spatial = false;
    uint32_t pictureWidth = 0, pictureHeight = 0, workWidth = 0, workHeight = 0;
    bool sourceMotionLowResolution = false; // retained create flags, not picture-flow flags
    Texture depth, motion;
    Rect depthRect, motionRect; // original source subrects, before any row normalization
    float scaleX = 0.0f, scaleY = 0.0f;
    GuideCoordinates::Mapping coordinates;
};
struct Decision
{
    Request requested = Request::None;
    Path path = Path::Rejected;
    bool routeEligible = false;
    float phaseUvX = 0.0f, phaseUvY = 0.0f;
    const char* reason = "outside-scene-route";
    bool Admitted() const { return path != Path::Rejected; }
    bool NeedsPreparation() const { return path == Path::PrepareRetained; }
};

constexpr bool ValidSerial(uint64_t serial) { return serial != 0 && serial != UINT64_MAX; }
constexpr bool RetainedOwnerKnown(const DlssNrFrameInfo& f)
{
    return f.SceneGuideOwner[0] != 0 ||
        (f.SceneGuideOwner[1] != 0 && ValidSerial(f.SceneGuideOwner[2]));
}

// Model-only contract: current source DeviceZ + nonjittered source-pixel motion,
// independent rects and an explicit canonical render-pixel address phase. No warp.
// This does not verify post-HUD depth, temporal guides, submission or GPU completion.
inline Decision Resolve(const DlssNrFrameInfo& f, const Consumer& c)
{
    Decision d;
    d.requested = Requested(f);
    const auto reject = [&](const char* reason) { d.reason = reason; return d; };
    d.routeEligible = d.requested != Request::None && !c.vulkanOrigin && f.FinishedPicture &&
        !f.BeforeUpscale && f.IndependentCommands && !f.FreshColorResetFallback && !c.spatial;
    if (!d.routeEligible) return d;
    if (!ValidSerial(f.SourceCaptureSerial) || f.SourceCaptureEpoch == UINT64_MAX)
        return reject("source-identity");
    if (d.requested == Request::NativeExisting)
    {
        if (!f.NativeBaseIdentityVerified || !f.NativeCurrentDepthVerified || !f.NativePresentQueueIdentity ||
            f.MotionIsPictureFlow)
            return reject("native-source-identity");
    }
    else if (!f.SceneGuideSourceVerified || !f.SceneGuideMotion || !RetainedOwnerKnown(f) ||
             !ValidSerial(f.SourceCaptureEpoch) || f.SourceCaptureSerial != f.SourceCaptureEpoch)
        return reject("retained-source-identity");
    if (!f.NativeFeatureFlagsKnown) return reject("unknown-create-flags");
    if (!f.NativeGuideMetadataKnown) return reject("unknown-source-guide-metadata");
    if (!f.NativeMotionJitterKnown) return reject("unknown-motion-jitter");
    if (f.NativeMotionVectorsJittered) return reject("jittered-motion-not-declared-canonical");
    if (!f.NativeCurrentJitterKnown) return reject("unknown-current-jitter");
    if (!std::isfinite(f.NativeJitterX) || !std::isfinite(f.NativeJitterY) ||
        std::abs(f.NativeJitterX) > 0.5f || std::abs(f.NativeJitterY) > 0.5f)
        return reject("invalid-current-jitter");
    if (!c.sourceMotionLowResolution || !f.RenderSubrectWidth || !f.RenderSubrectHeight ||
        c.depthRect.width != f.RenderSubrectWidth || c.depthRect.height != f.RenderSubrectHeight ||
        c.motionRect.width != f.RenderSubrectWidth || c.motionRect.height != f.RenderSubrectHeight ||
        c.depthRect.x != f.DepthSubrectBaseX || c.depthRect.y != f.DepthSubrectBaseY)
        return reject("source-extent-contract");
    const bool retained = d.requested == Request::RetainedSource;
    if (c.motionRect.x != (retained ? f.SceneMotionBaseX : f.MotionSubrectBaseX) ||
        c.motionRect.y != (retained ? f.SceneMotionBaseY : f.MotionSubrectBaseY) ||
        (retained && (c.motionRect.width != f.SceneMotionWidth || c.motionRect.height != f.SceneMotionHeight)) ||
        c.scaleX != (retained ? f.SceneMvScaleX : f.MvScaleX) ||
        c.scaleY != (retained ? f.SceneMvScaleY : f.MvScaleY))
        return reject("source-view-contract");
    if (!Fits(c.depth, c.depthRect) || !Fits(c.motion, c.motionRect))
        return reject("source-region-or-view");
    if (c.depth.encoding != Samples::DeviceDepth || c.motion.encoding != Samples::SignedMotion)
        return reject("source-format-contract");
    if (!std::isfinite(c.scaleX) || !std::isfinite(c.scaleY) || c.scaleX == 0 || c.scaleY == 0)
        return reject("source-motion-scale");
    if (!c.pictureWidth || !c.pictureHeight || !c.workWidth || !c.workHeight ||
        f.OutputWidth != c.pictureWidth || f.OutputHeight != c.pictureHeight)
        return reject("output-extent-contract");
    const uint32_t coordinateKey = (c.coordinates.depthRowsFlip ? 1u : 0u) |
        (c.coordinates.motionRowsFlip ? 2u : 0u) | (c.coordinates.vectorYNegated ? 4u : 0u);
    if (c.coordinates.key != coordinateKey) return reject("source-coordinate-contract");
    d.phaseUvX = f.NativeJitterX / static_cast<float>(c.motionRect.width);
    d.phaseUvY = f.NativeJitterY / static_cast<float>(c.motionRect.height);
    d.path = retained ? Path::PrepareRetained : Path::NativeExisting;
    d.reason = retained ? "preparation-required" : "native-existing-guide-path";
    return d;
}

using HistoryKey = std::array<uint64_t, 23>;
inline uint32_t FloatBits(float f)
{
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(f));
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// A recorded preparation is not completed GPU work. This key only isolates model
// and adjacent history consumers; their real queue/fence ownership stays external.
// Keep the existing 23-field layout consumed by DetailReuse::HistoryContract.
inline HistoryKey MakeHistoryKey(const DlssNrFrameInfo& f, const Consumer& c, const Decision& d,
                                bool preparationRecorded, bool conditionalGeometry)
{
    HistoryKey key {};
    if (d.Admitted() && (!d.NeedsPreparation() || preparationRecorded))
    {
        key = { 1ull, f.NativeFeatureFlags, c.depthRect.width, c.depthRect.height,
            c.depthRect.x, c.depthRect.y, c.motionRect.width, c.motionRect.height,
            c.motionRect.x, c.motionRect.y, FloatBits(c.scaleX), FloatBits(c.scaleY),
            c.coordinates.key, c.depth.format, c.motion.format, f.RayReconstruction ? 1ull : 0ull,
            d.NeedsPreparation() && conditionalGeometry ? 1ull : 0ull,
            f.OutputWidth, f.OutputHeight, d.NeedsPreparation() ? 2ull : 1ull };
    }
    // Preserve source replacement even on rejected metadata or failed recording.
    // These owner bits are isolation facts, never an admission/completion receipt.
    if (d.routeEligible)
        for (unsigned i = 0; i < 3; ++i) key[20 + i] = f.SceneGuideOwner[i];
    return key;
}
}
