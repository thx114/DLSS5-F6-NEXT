// Reuse detail between frames (dlssnr/DlssNrDetailReuse.h): the model runs on one frame, and on the next the saved detail
// (answer minus input, in the proxy domain at the model's working size) is moved with the motion vectors and added to
// the new input instead of running the model. The method is reverse reprojection caching (Nehab et al. 2007) with the
// history rejection TAA uses:
//   - two motion candidates per pixel: its own, and the closest surface's in the 3x3 neighbourhood so edges can move
//     with the foreground (Karis 2014); each is tested with the depth of its own surface, the better one is kept;
//   - a depth test against the saved 2x2 footprint the reprojected sample is read from;
//   - a motion test: the current vectors at the place it came from must be close to the pixel's own, or something
//     else moves there now (relative to the vector's length, so fast motion does not reject);
//   - a colour test against the current 3x3 neighbourhood's variance box (Salvi 2016), measured in standard
//     deviations, so noise does not reject and a real change does;
//   - Catmull-Rom for moved detail only when its outer support is valid and on the surface; otherwise bilinear.
//     Both are clamped to the central 2x2 range (sharp, no ringing).
// Trust fades smoothly rather than switching, and full frames can be pulled toward the moved detail (Steady), so
// frames next to each other differ less.
//
// Separate from dlssnr.hlsl so the shared shader and every ordinary pass stay as they are. It reuses that shader's
// root signature and descriptor table (t0..t5, u0..u2, b0, s0 = linear clamp) through
// DlssNr_Dx12::DispatchDetailReuse. On Vulkan (VK_MODE) it has its own pass and descriptor set layout,
// DlssNrDetailReuse_Vk, one binding per register: 0 b0, 1-5 t0-t4, 6-7 u0-u1, 8 s0, 9 t5. The storage images carry no
// fixed format (what they write varies by mode), which needs shaderStorageImageWriteWithoutFormat; they are never read.
// DX12 mode 24 also writes u2 birth fingerprint/depth; Vulkan retains its
// existing two-output ABI. Modes and bindings: DlssNr_DetailReuseConstants.h.
//
// Replace modes (replaceCurve != 0): there the answer IS the picture, decoded through the curve's inverse (Neutwo or the
// hybrid, dlssnr_replace_curve.hlsli), whose slope runs away near white. A saved change -- a difference of proxy values,
// as in every mode -- moved a pixel off at an edge, so that a mid grey's change landed on a highlight, decoded to over a
// hundred times the pixel, and the resolve's guard showed it at 2x: a white flash on every reused frame. So where a
// moved change lands in these modes it is landed two ways, as that difference and as the ratio of decoded light it made
// where it came from, and the one that changes the pixel less is kept: measured in stops of whichever channel moves
// most (the ratio is per channel, so a colour must not shift while its brightest channel holds still), at the trust the
// change is applied at. The difference is right except on a highlight
// (the pole); the ratio is right except where a dark pixel's change lands on something bright (grey patches on a lit
// floor); on the surface a change came from the two agree. Single forms were tried in game first (2026-09-30), and each
// broke one way: the ratio (grey patches), a difference of open-ended sRGB light (white patches beside reflections), and
// a logarithm above white (the same, milder). The Composed modes and passthrough frames land the difference, bit for bit.
//
// Raw vectors times MvScale are pixels of the motion texture's own subrect (render resolution with low-resolution
// vectors), so they are divided by its size into a uv displacement of the image; the saved vectors are kept in that
// form too, so a render-size change between two frames cannot misread them.
// Vectors point from the current pixel to where it was: previous = current + mv. Depth is kept far-is-zero on both
// conventions (reversed Z as is, standard Z as 1 - d): with reversed Z, |a - b| / max(a, b) is the relative
// difference of linear view depth, and sky compares equal to sky.

#ifdef VK_MODE
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint mode;
    uint workWidth;
    uint workHeight;
    uint motionWidth;
    uint motionHeight;
    uint motionBaseX;
    uint motionBaseY;
    uint depthWidth;
    uint depthHeight;
    uint depthBaseX;
    uint depthBaseY;
    uint depthInverted;
    float mvScaleX;
    float mvScaleY;
    float depthTolerance;
    float clipGamma;
    float clipFalloff;
    float sigmaFloor;
    float steady;
    uint debugView;
    float fillStrength;
    float fillRadius;
    uint replaceCurve; // DlssNrReplaceCurve: 0 none, 1 Neutwo, 2 hybrid (see the header)
    float motionReject;
    float steadyDeadZone;
    uint historyDistrust; // t5 holds an outside opinion of this pixel's history (see OutsideTrust); 0: none
    uint temporalMode;
    uint temporalFrameKind;
    uint missingDepthRun;
    uint maxMissingDepthRun;
    float temporalHistoryWeight;
    float missingDepthHistoryCap;
    float depthReturnHistoryCap;
    float minimumFlowTrust;
    float minimumColourTrust;
    float nativeMotionPhaseUvX;
    float nativeMotionPhaseUvY;
    float nativeDepthPhaseUvX;
    float nativeDepthPhaseUvY;
    float nativeMotionJitterCancelUvX;
    float nativeMotionJitterCancelUvY;
#ifndef VK_MODE
    uint temporalHandoff;
    uint temporalHandoffResetMask;
    uint temporalHandoffIncoming;
    float temporalHandoffBlend;
    uint temporalContent;
    uint sceneGuideFlags;
    uint scenePictureWidth;
    uint scenePictureHeight;
    float feedbackDeltaSeconds;
    float feedbackDurationSeconds;
    float feedbackMaxAgeSeconds;
    float feedbackStrength;
    uint feedbackHistoryValid;
    float feedbackRasterOffsetUvX;
    float feedbackRasterOffsetUvY;
    float feedbackStabilitySeconds;
    float feedbackStabilityFloor;
    float feedbackStabilityClamp;
#endif
};

#ifdef VK_MODE
#define DR_BINDING(n) [[vk::binding(n, 0)]]
// Without it dxc declares float4 storage images as rgba32f, which must then match the image's own format.
#define DR_ANY_FORMAT [[vk::image_format("unknown")]]
#else
#define DR_BINDING(n)
#define DR_ANY_FORMAT
#endif
DR_BINDING(1) Texture2D<float4> t0 : register(t0);
DR_BINDING(2) Texture2D<float4> t1 : register(t1);
DR_BINDING(3) Texture2D<float4> t2 : register(t2);
DR_BINDING(4) Texture2D<float4> t3 : register(t3);
DR_BINDING(5) Texture2D<float4> t4 : register(t4);
DR_BINDING(9) Texture2D<float4> t5 : register(t5);
DR_BINDING(6) DR_ANY_FORMAT RWTexture2D<float4> u0 : register(u0);
DR_BINDING(7) DR_ANY_FORMAT RWTexture2D<float4> u1 : register(u1);
#ifndef VK_MODE
// Independent history commits all three outputs in one dispatch. Legacy and
// Vulkan modes retain their two-output contract.
RWTexture2D<float4> u2 : register(u2);
Texture2D<float4> t6 : register(t6); // independent filtered NR residual / mean lag
RWTexture2D<float4> u3 : register(u3);
#endif
DR_BINDING(8) SamplerState gLinear : register(s0);

bool Finite3(float3 v) { return all(isfinite(v)); }

#include "dlssnr_replace_curve.hlsli"

// A proxy value -> the light the resolve decodes it to, and back (Replace modes only).
float3 ReplaceDecode(float3 proxy)
{
    const float3 y = SrgbToLinear(proxy);
    return replaceCurve == 1u ? NeutwoDecode(y) : HybridDecode(y);
}

float3 ReplaceEncode(float3 light) { return LinearToSrgb(replaceCurve == 1u ? NeutwoEncode(light) : HybridEncode(light)); }

// How far a candidate moves the pixel whose decoded light is `from`: in stops of whichever channel moves most.
float StopsFrom(float3 from, float3 candidate)
{
    const float3 stops = abs(log2((ReplaceDecode(candidate) + kRatioFloor) / (from + kRatioFloor)));
    return max(stops.r, max(stops.g, stops.b));
}

// Of two ways to land a change on `input` (decoded: `light`), as the values they take it to, the one that moves it less
// when added at `strength` (the trust it is applied at). Returned as the difference to add to input.
float3 SmallerLanding(float3 input, float3 light, float3 asRatio, float3 asDifference, float strength)
{
    const float3 byRatio = asRatio - input, byDifference = asDifference - input;
    const bool ratioMovesLess =
        StopsFrom(light, input + byRatio * strength) < StopsFrom(light, input + byDifference * strength);
    return ratioMovesLess ? byRatio : byDifference;
}

// Replace modes: a change `detail` saved where the picture was `source`, landing on `input` at `strength` (see the
// header). Returns the difference to add to input: the moved one, or the ratio it made at its source applied here.
float3 LandReplaceDetail(float3 input, float3 detail, float3 source, float strength)
{
    if (all(detail == 0.0))
        return 0.0;
    const float3 light = ReplaceDecode(input);
    const float3 ratio = (ReplaceDecode(source + detail) + kRatioFloor) / (ReplaceDecode(source) + kRatioFloor);
    const float3 asRatio = ReplaceEncode(max((light + kRatioFloor) * ratio - kRatioFloor, 0.0));
    return SmallerLanding(input, light, asRatio, input + detail, strength);
}

float2 WorkSize() { return float2(workWidth, workHeight); }
float2 WorkUv(uint2 p) { return (float2(p) + 0.5) / WorkSize(); }

// depthWidth/depthHeight are 0 on a frame the game (or the native producer) supplied no depth for -- the same
// signal DlssNr_Dx12::Dispatch's guide resolution already produces for "no depth" (DlssNr_Guides.h). t2/t4 are
// not valid depth data in that case (DispatchDetailReuse stands t0 in for any null guide so every descriptor
// slot stays bound), so nothing below may read them for trust.
bool HasDepth() { return depthWidth != 0 && depthHeight != 0; }

// Only the authenticated DX12 native-depth feedback opts into strict surface
// support. Legacy interval reuse, Vulkan and picture-flow keep their contracts.
bool NativeSurfaceHistory()
{
#ifndef VK_MODE
    return temporalMode == 1u;
#else
    return false;
#endif
}

bool SameHistorySurface(float a, float b)
{
    return isfinite(a) && isfinite(b) && a >= 0.0 && b >= 0.0 &&
        abs(a - b) / max(max(a, b), 1e-6) <= depthTolerance;
}

// A distinct DX12 source-chain storage contract. Never a Vulkan/native-depth permission.
bool PictureFlowRoute()
{
#ifndef VK_MODE
    return temporalMode == 2u || temporalMode == 3u;
#else
    return false;
#endif
}

// Scene geometry can only VETO final-colour flow; it never supplies HUD motion.
bool SceneGeometry()
{
#ifndef VK_MODE
    return temporalMode == 3u;
#else
    return false;
#endif
}

// Different storage semantics, selected only by the authenticated DX12 full-model caller.
bool DetailContent()
{
#ifndef VK_MODE
    return temporalContent == 1u && temporalHandoff != 0u;
#else
    return false;
#endif
}

float FarIsZero(float d) { return depthInverted != 0 ? d : 1.0 - d; }

int2 GuideTexel(float2 uv, uint2 size)
{
    return int2(min(uint2(saturate(uv) * float2(size)), size - 1));
}

float2 DepthUv(float2 uv) { return temporalMode != 0u ? uv + float2(nativeDepthPhaseUvX, nativeDepthPhaseUvY) : uv; }

float GuideDepth(Texture2D<float4> depth, int2 texel)
{
    const float4 sample = depth.Load(int3(uint2(texel) + uint2(depthBaseX, depthBaseY), 0));
    if (SceneGeometry())
        return isfinite(sample.a) && sample.a > 0.0 && isfinite(sample.z) && sample.z > 0.0 ? sample.z : -1.0;
    return FarIsZero(sample.r);
}

float3 ToYCoCg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

// Catmull-Rom from five bilinear taps (the four corners of the 4x4 are dropped).
float4 SampleCatmullRom(Texture2D<float4> tex, float2 uv, float2 size)
{
    const float2 position = uv * size;
    const float2 centre = floor(position - 0.5) + 0.5;
    const float2 f = position - centre;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 texel = 1.0 / size;
    const float2 uv0 = (centre - 1.0) * texel;
    const float2 uv3 = (centre + 2.0) * texel;
    const float2 uv12 = (centre + w2 / w12) * texel;

    float4 result = tex.SampleLevel(gLinear, float2(uv12.x, uv0.y), 0) * (w12.x * w0.y);
    result += tex.SampleLevel(gLinear, float2(uv0.x, uv12.y), 0) * (w0.x * w12.y);
    result += tex.SampleLevel(gLinear, uv12, 0) * (w12.x * w12.y);
    result += tex.SampleLevel(gLinear, float2(uv3.x, uv12.y), 0) * (w3.x * w12.y);
    result += tex.SampleLevel(gLinear, float2(uv12.x, uv3.y), 0) * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return result / max(weight, 1e-4);
}

float2 MotionSize() { return float2(motionWidth, motionHeight); }

// Raw vector at image position uv, read from the motion subrect.
float2 RawMotion(float2 uv)
{
    return t3.Load(int3(uint2(motionBaseX, motionBaseY) + uint2(GuideTexel(temporalMode != 0u ? uv + float2(nativeMotionPhaseUvX, nativeMotionPhaseUvY) : uv, uint2(motionWidth, motionHeight))),
                        0)).rg;
}

// Raw vector -> displacement in image uv.
float2 UvDisplacement(float2 raw)
{
    const float2 uv = raw * float2(mvScaleX, mvScaleY) / MotionSize();
    return temporalMode != 0u ? uv - float2(nativeMotionJitterCancelUvX, nativeMotionJitterCancelUvY) : uv;
}

// The 3x3 colour neighbourhood of the current input, in YCoCg: mean and standard deviation (floored).
void ColourBox(uint2 p, out float3 mean, out float3 sigma)
{
    const int2 last = int2(WorkSize()) - 1;
    float3 sum = 0.0, sumSquares = 0.0;
    [unroll] for (int cy = -1; cy <= 1; ++cy)
    {
        [unroll] for (int cx = -1; cx <= 1; ++cx)
        {
            const float3 c = ToYCoCg(t0.Load(int3(clamp(int2(p) + int2(cx, cy), int2(0, 0), last), 0)).rgb);
            sum += c;
            sumSquares += c * c;
        }
    }
    mean = sum / 9.0;
    sigma = max(sqrt(max(sumSquares / 9.0 - mean * mean, 0.0)), max(sigmaFloor, 1e-5));
}

// How far the vector `raw` a pixel moved by is trusted, given the current vectors where it points (0..1). A pixel and
// the place it came from should move alike; where they do not, something else moves there now (an edge crossing it, a
// surface that was behind) and what was saved there is not this pixel's. The difference is measured in working-size
// pixels against the vector's length plus one, so fast motion and its small errors do not reject, and fades out
// between half and the whole of motionReject times that.
float MotionTrust(float2 previousUv, float2 raw)
{
    if (motionReject <= 0.0)
        return 1.0;
    const float2 here = UvDisplacement(raw) * WorkSize();
    const float2 there = UvDisplacement(RawMotion(previousUv)) * WorkSize();
    const float difference = length(here - there);
    if (!isfinite(difference))
        return temporalMode != 0u ? 0.0 : 1.0; // native temporal mode fails closed on invalid motion
    return 1.0 - smoothstep(0.5, 1.0, difference / (motionReject * (length(here) + 1.0)));
}

#ifndef VK_MODE
// Residuals share an exact proxy/exposure contract. Compare adjacent raw
// input structure without the light-edit pow/log decode. Constant offsets
// cancel, but a displaced thin edge cannot acquire a new correspondence.
float NativeResidualStructureTrust(float2 currentUv, float2 previousUv)
{
    float3 deltaMean = 0.0, deltaSquare = 0.0;
    float3 nowMean = 0.0, nowSquare = 0.0, beforeMean = 0.0, beforeSquare = 0.0;
    const int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll] for (int i = 0; i < 5; ++i)
    {
        const float2 offset = float2(offsets[i]) / WorkSize();
        const float3 now = t0.SampleLevel(gLinear, currentUv + offset, 0).rgb;
        const float3 before = t2.SampleLevel(gLinear, previousUv + offset, 0).rgb;
        if (!Finite3(now) || !Finite3(before)) return 0.0;
        const float3 delta = now - before;
        deltaMean += delta; deltaSquare += delta * delta;
        nowMean += now; nowSquare += now * now;
        beforeMean += before; beforeSquare += before * before;
    }
    deltaMean *= 0.2; nowMean *= 0.2; beforeMean *= 0.2;
    const float3 error = sqrt(max(deltaSquare * 0.2 - deltaMean * deltaMean, 0.0));
    const float3 contrast = sqrt(max((nowSquare + beforeSquare) * 0.1 -
        (nowMean * nowMean + beforeMean * beforeMean) * 0.5, 0.0));
    const float3 allowance = max(sigmaFloor, 1e-5) + 0.25 * contrast;
    const float3 reject = smoothstep(allowance, 3.0 * allowance, error);
    return 1.0 - max(reject.r, max(reject.g, reject.b));
}

float3 FeedbackLight(float3 proxy)
{
    return replaceCurve != 0u ? ReplaceDecode(proxy) : SrgbToLinear(proxy);
}

// Match local structure after removing the measured per-channel log-light offset.
// This compares the genuine current/previous inputs, not a recursively generated answer.
// Constant exposure/white-balance changes cancel; a displaced edge does not.
float FeedbackStructureTrust(float2 currentUv, float2 previousUv)
{
    float3 deltaMean = 0.0, deltaSquare = 0.0;
    float3 nowMean = 0.0, nowSquare = 0.0, beforeMean = 0.0, beforeSquare = 0.0;
    const int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll] for (int i = 0; i < 5; ++i)
    {
        const float2 offset = float2(offsets[i]) / WorkSize();
        const float3 nowProxy = t0.SampleLevel(gLinear, currentUv + offset, 0).rgb;
        const float3 beforeProxy = t2.SampleLevel(gLinear, previousUv + offset, 0).rgb;
        if (!Finite3(nowProxy) || !Finite3(beforeProxy) || any(nowProxy < 0.0) || any(beforeProxy < 0.0) ||
            any(nowProxy > 1.0) || any(beforeProxy > 1.0)) return 0.0;
        const float3 now = log2(FeedbackLight(nowProxy) + kRatioFloor);
        const float3 before = log2(FeedbackLight(beforeProxy) + kRatioFloor);
        if (!Finite3(now) || !Finite3(before)) return 0.0;
        const float3 delta = now - before;
        deltaMean += delta; deltaSquare += delta * delta;
        nowMean += now; nowSquare += now * now;
        beforeMean += before; beforeSquare += before * before;
    }
    deltaMean *= 0.2; nowMean *= 0.2; beforeMean *= 0.2;
    const float3 error = sqrt(max(deltaSquare * 0.2 - deltaMean * deltaMean, 0.0));
    const float3 contrast = sqrt(max((nowSquare + beforeSquare) * 0.1 -
        (nowMean * nowMean + beforeMean * beforeMean) * 0.5, 0.0));
    const float3 allowance = 0.02 + 0.25 * contrast;
    const float3 reject = smoothstep(allowance, 3.0 * allowance, error);
    return 1.0 - max(reject.r, max(reject.g, reject.b));
}
#endif

// The saved detail moved from previousUv (rgb) and how far it is trusted (a, 0..1), for a pixel whose surface has
// far-is-zero depth depthNow and which moved by the raw vector `raw`. Reads t1 saved detail, t2 saved colour + depth.
// byMotion is the share of trust the vectors' disagreement took away (only the debug view reads it).
float4 MovedFrom(float2 currentUv, float2 previousUv, float2 raw, float depthNow, float3 mean, float3 sigma, out float3 source,
                 out float byMotion)
{
    source = 0.0;
    byMotion = 0.0;
    const float2 work = WorkSize();
    const bool hasDepth = HasDepth();
    // depthNow is only meaningful (and only finite by construction) when hasDepth; a depth-less frame passes a
    // placeholder that must not reject the sample.
    if (!all(isfinite(previousUv)) || any(previousUv < 0.0) || any(previousUv > 1.0) ||
        (hasDepth && (!isfinite(depthNow) || (SceneGeometry() && depthNow <= 0.0))))
        return 0.0;

    // The 2x2 footprint the moved sample is built from: detail range, validity, depth range. Only texels with a
    // bilinear weight count: on a texel centre that is one texel, and its zero-weight neighbour (maybe another
    // surface) must not widen the depth range.
    const float2 position = previousUv * work - 0.5;
    const int2 base = int2(floor(position));
    const float2 f = position - float2(base);
    float3 detailLo = 65504.0, detailHi = -65504.0;
    float valid = 1.0, depthLo = 65504.0, depthHi = -65504.0;
    bool missingSavedDepth = false, hasSavedRealDepth = false;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const int2 corner = int2(i & 1, i >> 1);
        const float2 axis = float2(corner.x != 0 ? f.x : 1.0 - f.x, corner.y != 0 ? f.y : 1.0 - f.y);
        const float weight = axis.x * axis.y;
        if (weight <= 0.0 || (!NativeSurfaceHistory() && weight < 1e-3))
            continue;
        const int2 texel = clamp(base + corner, int2(0, 0), int2(work) - 1);
        const float4 d = t1.Load(int3(texel, 0));
        const float savedDepth = t2.Load(int3(texel, 0)).a;
        const bool ok = Finite3(d.rgb) && isfinite(d.a) && isfinite(savedDepth);
        valid = ok ? min(valid, d.a) : 0.0;
        detailLo = ok ? min(detailLo, d.rgb) : detailLo;
        detailHi = ok ? max(detailHi, d.rgb) : detailHi;
        const bool realDepth = ok && (temporalMode == 0u || savedDepth >= 0.0);
        // A depth interval spanning foreground and background is not a surface.
        // Hardware bilinear reads every nonzero tap: reject the entire native
        // footprint before it can mix a different surface's detail or colour.
        if (NativeSurfaceHistory() && (!realDepth || !SameHistorySurface(savedDepth, depthNow)))
            return 0.0;
        missingSavedDepth = missingSavedDepth || !realDepth;
        hasSavedRealDepth = hasSavedRealDepth || realDepth;
        depthLo = realDepth ? min(depthLo, savedDepth) : depthLo;
        depthHi = realDepth ? max(depthHi, savedDepth) : depthHi;
    }
    if (temporalMode != 0u ? valid <= 0.0 : valid < 0.999)
        return 0.0;

    // The five cubic taps also read eight outer texels. Their negative lobes can import invalid/other-surface
    // detail without leaving the central range, so the clamp alone cannot protect this sample. Validate every
    // contributing outer texel; never expand the central depth range to accommodate another surface.
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    bool cubicSafe = true;
    float cubicConfidence = valid;
    [unroll] for (int tap = 0; tap < 8; ++tap)
    {
        const bool horizontal = tap < 4;
        const int edge = (tap & 2) != 0 ? 2 : -1;
        const int inner = tap & 1;
        const int2 offset = horizontal ? int2(edge, inner) : int2(inner, edge);
        const float2 outerWeight = edge == -1 ? w0 : w3;
        const float2 innerWeight = inner == 0 ? w1 : w2;
        const float weight = horizontal ? outerWeight.x * innerWeight.y : innerWeight.x * outerWeight.y;
        // Negative weights matter too. At exact texel centres there is no outer support to validate.
        if (abs(weight) < 1e-7)
            continue;
        const int2 texel = clamp(base + offset, int2(0, 0), int2(work) - 1);
        const float4 d = t1.Load(int3(texel, 0));
        bool ok = Finite3(d.rgb) && isfinite(d.a) && (temporalMode != 0u ? d.a > 0.0 : d.a >= 0.999);
        if (hasDepth)
        {
            const float z = t2.Load(int3(texel, 0)).a;
            ok = ok && isfinite(z) && (temporalMode == 0u || z >= 0.0) &&
                abs(z - depthNow) / max(max(z, depthNow), 1e-6) <= depthTolerance;
        }
        cubicSafe = cubicSafe && ok;
        if (PictureFlowRoute() || DetailContent())
            cubicConfidence = min(cubicConfidence, isfinite(d.a) ? saturate(d.a) : 0.0);
    }
    float3 sample = t1.SampleLevel(gLinear, previousUv, 0).rgb;
    if (cubicSafe)
    {
        const float3 cubic = SampleCatmullRom(t1, previousUv, work).rgb;
        if (Finite3(cubic))
        {
            sample = cubic;
            // The source chain cannot promote a lower-confidence outer contributor.
            if (PictureFlowRoute() || DetailContent()) valid = min(valid, cubicConfidence);
        }
    }
    const float3 moved = clamp(sample, detailLo, detailHi);

    // Depth: how far this surface lies outside the saved range, relative to the nearer of the two. With no depth
    // this frame (hasDepth false) there is nothing to compare depthNow against, so trust falls back to colour alone.
    float depthTrust = 1.0;
    if (hasDepth && (temporalMode == 0u || hasSavedRealDepth))
    {
        const float outside = max(max(depthLo - depthNow, depthNow - depthHi), 0.0);
        const float relativeDepth = outside / max(max(depthNow, depthHi), 1e-6);
        depthTrust = 1.0 - smoothstep(depthTolerance, 2.0 * depthTolerance, relativeDepth);
    }

    // Colour: the saved input colour against the current input's variance box, in standard deviations.
    source = t2.SampleLevel(gLinear, previousUv, 0).rgb;
    const float3 saved = ToYCoCg(source);
    const float3 excess = max(abs(saved - mean) - clipGamma * sigma, 0.0) / sigma;
    float colourTrust = 1.0 - saturate(max(excess.x, max(excess.y, excess.z)) / max(clipFalloff, 1e-3));
#ifndef VK_MODE
    // Light-edit feedback matches decoded structure rather than absolute colour.
    [branch] if (DetailContent()) colourTrust = FeedbackStructureTrust(currentUv, previousUv);
#endif

    if (!Finite3(moved) || !Finite3(saved))
        return 0.0;
    const float motionTrust = MotionTrust(previousUv, raw);
    byMotion = 1.0 - motionTrust;
#ifndef VK_MODE
    // A variance box contains both sides of a thin edge and cannot establish
    // correspondence. Native residuals also require matching raw input structure.
    // Reject already-invalid candidates before paying the structural sampling cost.
    [branch] if (NativeSurfaceHistory() && !DetailContent())
    {
        if (depthTrust <= 0.0 || colourTrust <= 0.0 || motionTrust <= 0.0) return 0.0;
        colourTrust = min(colourTrust, NativeResidualStructureTrust(currentUv, previousUv));
    }
#endif
    float reliability = saturate(depthTrust * colourTrust * motionTrust);
    if (temporalMode != 0u)
    {
        // Native mode and conditional scene veto both need real saved geometry.
        // Pure picture-flow mode retains its explicitly depth-free storage contract.
        if ((!PictureFlowRoute() || SceneGeometry()) && (!hasDepth || missingSavedDepth)) return float4(0.0, 0.0, 0.0, 0.0);
        reliability = min(reliability, saturate(valid));
    }
    return float4(moved, reliability);
}

// How far an outside opinion trusts this pixel's history (DlssNrFrameInfo::HistoryDistrust): t5 holds distrust 0..1 in
// .r at any size, read at the pixel's position. On native input it is Optical F5Low's trust mask, which checks what the
// tests here cannot: whether the flow agrees with itself from frame to frame, and whether a nearer surface has just
// moved off this pixel. Without one, or where it reads as no number, it says nothing (1).
#ifndef VK_MODE
// GuideMotion linearly enlarges half-size picture flow. Its full-guide texel
// must not inherit trust from an average that hides a rejected flow contributor.
float PictureFlowSupportTrust(float2 uv)
{
    uint guideWidth, guideHeight, maskWidth, maskHeight;
    t3.GetDimensions(guideWidth, guideHeight);
    t5.GetDimensions(maskWidth, maskHeight);
    // Query the actual bound SRVs, not assumed half-size constants. This source
    // contract has a full-picture guide, base zero, and exact 2x flow pixel pitch.
    if (motionBaseX != 0u || motionBaseY != 0u || motionWidth == 0u || motionHeight == 0u ||
        (motionWidth & 1u) != 0u || (motionHeight & 1u) != 0u ||
        guideWidth != motionWidth || guideHeight != motionHeight ||
        maskWidth == 0u || maskHeight == 0u ||
        maskWidth != motionWidth / 2u || maskHeight != motionHeight / 2u)
        return 0.0;

    // Match RawMotion's actual point address before recovering the half-flow
    // footprint used by GuideMotion. WorkUv itself can have different support
    // when the model's work grid is smaller than the final-picture guide.
    const float2 motionUv = uv + float2(nativeMotionPhaseUvX, nativeMotionPhaseUvY);
    if (!all(isfinite(motionUv))) return 0.0;
    const int2 guideTexel = GuideTexel(motionUv, uint2(motionWidth, motionHeight));
    const float2 guideUv = (float2(guideTexel) + 0.5) / float2(guideWidth, guideHeight);
    const float2 position = guideUv * float2(maskWidth, maskHeight) - 0.5;
    const int2 base = int2(floor(position));
    const float2 fraction = position - float2(base);
    float worst = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const int2 corner = int2(i & 1, i >> 1);
        const float2 weight = float2(corner.x != 0 ? fraction.x : 1.0 - fraction.x,
                                    corner.y != 0 ? fraction.y : 1.0 - fraction.y);
        // Only nonzero bilinear participants count; clamp exactly as the guide's
        // sampler does at image edges, without importing an out-of-range texel.
        if (weight.x <= 0.0 || weight.y <= 0.0) continue;
        const int2 texel = clamp(base + corner, int2(0, 0), int2(maskWidth, maskHeight) - 1);
        const float distrust = t5.Load(int3(texel, 0)).r;
        if (!isfinite(distrust)) return 0.0;
        worst = max(worst, saturate(distrust));
    }
    return 1.0 - worst;
}
#endif

float OutsideTrust(float2 uv)
{
    if (historyDistrust == 0u)
        return temporalMode != 0u && !HasDepth() ? 0.0 : 1.0;
    const float distrust = t5.SampleLevel(gLinear, uv, 0).r;
    const float trust = isfinite(distrust) ? 1.0 - saturate(distrust) : (temporalMode != 0u ? 0.0 : 1.0);
#ifndef VK_MODE
    if (PictureFlowRoute())
        return trust > 0.0 ? min(trust, PictureFlowSupportTrust(uv)) : 0.0;
#endif
    return trust;
}

// The moved detail for work pixel p (rgb), how far its own tests trust it (a, 0..1), and the saved picture it came
// from. Reads t0 input, t1 saved detail, t2 saved colour + depth, t3 motion guide, t4 depth guide.
float4 MovedDetailCandidate(uint2 p, out float3 source, out float byMotion)
{
    source = 0.0; // matches MovedFrom's own convention; keeps every exit, including the one below, defined
    byMotion = 0.0;
    const float2 uv = WorkUv(p);
    // Source flow never borrows a scene-depth donor. Version 3 has only a conditional
    // geometric veto at the pixel's OWN measured motion. Missing trust fails closed.
    if (PictureFlowRoute() && ((HasDepth() && !SceneGeometry()) || historyDistrust == 0u)) return 0.0;
    if (SceneGeometry() && !HasDepth()) return 0.0;

    float3 mean, sigma;
    ColourBox(p, mean, sigma);
    if (SceneGeometry())
    {
        const float2 raw = RawMotion(uv);
        const float z = GuideDepth(t4, GuideTexel(uv, uint2(depthWidth, depthHeight)));
        return MovedFrom(uv, uv + UvDisplacement(raw), raw, z, mean, sigma, source, byMotion);
    }

    // No depth this frame: t4 is not a real depth guide (DispatchDetailReuse stood a null In4 in with t0, the
    // colour), so there is no "closest surface in the 3x3" to find -- one motion candidate, trusted by colour alone
    // (MovedFrom's hasDepth check ignores the placeholder depth below).
    if (!HasDepth())
    {
        const float2 raw = RawMotion(uv);
        return MovedFrom(uv, uv + UvDisplacement(raw), raw, 0.0, mean, sigma, source, byMotion);
    }

    const uint2 depthSize = uint2(depthWidth, depthHeight);
    const int2 centre = GuideTexel(DepthUv(uv), depthSize);

    // The pixel's own surface, and the closest surface in its 3x3.
    const float ownDepth = GuideDepth(t4, centre);
    float closestDepth = isfinite(ownDepth) ? ownDepth : -1.0;
    int2 closest = centre;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 texel = clamp(centre + int2(dx, dy), int2(0, 0), int2(depthSize) - 1);
            const float z = GuideDepth(t4, texel);
            if (isfinite(z) && z > closestDepth)
            {
                closestDepth = z;
                closest = texel;
            }
        }
    }

    // Own motion with own depth. The closest surface's motion is read at this pixel's position shifted by the
    // depth-texel offset (motion may be finer than depth), and tested with that surface's depth.
    float3 ownSource, nearSource;
    float ownByMotion, nearByMotion;
    const float2 ownRaw = RawMotion(uv);
    const float4 own = MovedFrom(uv, uv + UvDisplacement(ownRaw), ownRaw, ownDepth, mean, sigma, ownSource, ownByMotion);
    source = ownSource;
    byMotion = ownByMotion;
    if (all(closest == centre) || (NativeSurfaceHistory() && own.a >= 1.0) ||
        (NativeSurfaceHistory() && !SameHistorySurface(closestDepth, ownDepth)))
        return own;
    const float2 closestUv = uv + float2(closest - centre) / float2(depthSize);
    const float2 nearRaw = RawMotion(closestUv);
    if (NativeSurfaceHistory() && all(nearRaw == ownRaw)) return own;
    const float4 near =
        MovedFrom(uv, uv + UvDisplacement(nearRaw), nearRaw,
            NativeSurfaceHistory() ? ownDepth : closestDepth, mean, sigma, nearSource, nearByMotion);
    // Native history always validates the recipient's own surface and retains
    // its own motion on ties. A neighbour never lends its depth as authority.
    // Preserve the legacy foreground-dilation tie convention.
    if (near.a > own.a || (!NativeSurfaceHistory() && near.a >= own.a))
    {
        source = nearSource;
        byMotion = nearByMotion;
        return near;
    }
    return own;
}

// The better of the pixel's candidates, with the outside opinion applied once (it is about this pixel, not about
// either candidate). Everything downstream -- Reproject, Fill, Steady, Coverage, the debug view -- reads its trust.
float4 MovedDetailFrom(uint2 p, out float3 source, out float byMotion)
{
    float4 moved = MovedDetailCandidate(p, source, byMotion);
    float external = OutsideTrust(WorkUv(p));
    if (SceneGeometry())
    {
        const float support = t4.Load(int3(GuideTexel(WorkUv(p), uint2(depthWidth, depthHeight)), 0)).a;
        external = min(external, isfinite(support) ? saturate(support) : 0.0);
    }
    if (temporalMode != 0u)
    {
        moved.a = min(moved.a, external);
        if (!PictureFlowRoute() && !HasDepth() && external < minimumFlowTrust) moved.a = 0.0;
    }
    else moved.a *= external;
    return moved;
}

// MovedDetailFrom, landed on this pixel's input in Replace modes (see the header), so everything that adds it --
// Reproject, Steady, Fill -- adds a change that is safe here.
float4 MovedDetail(uint2 p, out float byMotion)
{
    float3 source;
    float4 moved = MovedDetailFrom(p, source, byMotion);
    if (!DetailContent() && replaceCurve != 0u && moved.a > 0.0)
    {
        const float3 landed = LandReplaceDetail(t0.Load(int3(p, 0)).rgb, moved.rgb, source, moved.a);
        moved.rgb = Finite3(landed) ? landed : 0.0;
    }
    return moved;
}

// colourToo: also the input colour and depth into u1 (CaptureShown leaves u1 alone)
void Capture(uint2 p, bool colourToo)
{
    const float4 input = t0.Load(int3(p, 0));
    const float3 answer = t1.Load(int3(p, 0)).rgb;
    const float3 detail = answer - input.rgb;
    const bool valid = Finite3(detail) && Finite3(input.rgb) && all(abs(detail) <= 65504.0);
    float confidence = 1.0;
    if (!colourToo && PictureFlowRoute())
    {
        // FlowCaptureShown only: t3 is this source's already-captured main
        // latent, t4 the actual moved shown estimate used by this Spread.
        // This display residual is never promoted to a fresh model answer.
        const float4 current = t3.Load(int3(p, 0));
        const float4 movedShown = t4.Load(int3(p, 0));
        confidence = Finite3(current.rgb) && isfinite(current.a) &&
            Finite3(movedShown.rgb) && isfinite(movedShown.a)
            ? min(saturate(current.a), saturate(movedShown.a)) : 0.0;
    }
    u0[p] = float4(valid ? detail : 0.0, valid ? confidence : 0.0);
    if (!colourToo)
        return;
    // No depth this frame: t2 stands in with the colour, so save far (0) instead. A next frame with depth then finds
    // its surface nearer than the saved range and distrusts the moved detail for that one frame.
    const float depth = HasDepth() ? GuideDepth(t2, GuideTexel(DepthUv(WorkUv(p)), uint2(depthWidth, depthHeight))) : 0.0;
    u1[p] = float4(Finite3(input.rgb) ? input.rgb : 0.0, isfinite(depth) ? depth : 0.0);
}

// Versioned temporal capture: reusable latent amplitude is independent of display trust.
// Full frames provide a freshly fused model answer; reuse frames retain the raw moved estimate.
void TemporalCapture(uint2 p)
{
    const float3 input = t0.Load(int3(p, 0)).rgb;
    float3 latent = t1.Load(int3(p, 0)).rgb - input;
    float confidence = 1.0;
    if (temporalFrameKind == 1u)
    {
        const float4 estimate = t3.Load(int3(p, 0));
        latent = estimate.rgb;
        confidence = isfinite(estimate.a) ? saturate(estimate.a) : 0.0;
    }
    if (!HasDepth() && !PictureFlowRoute()) confidence = 0.0; // native mode still requires real depth
    // Reuse captures the raw moved latent AND its carried confidence, never
    // (display answer - input) as a new full-model residual with confidence 1.
    if (PictureFlowRoute() && ((HasDepth() && !SceneGeometry()) || historyDistrust == 0u)) confidence = 0.0;
    if (SceneGeometry())
    {
        const float support = t2.Load(int3(GuideTexel(WorkUv(p), uint2(depthWidth, depthHeight)), 0)).a;
        confidence = min(confidence, isfinite(support) ? saturate(support) : 0.0);
    }
    const bool valid = Finite3(input) && Finite3(latent) && all(abs(latent) <= 65504.0);
    u0[p] = float4(valid ? latent : 0.0, valid ? confidence : 0.0);
    float depth = -1.0;
    if (HasDepth())
    {
        const float supplied = GuideDepth(t2, GuideTexel(DepthUv(WorkUv(p)), uint2(depthWidth, depthHeight)));
        if (isfinite(supplied) && supplied >= 0.0) depth = supplied;
        else { confidence = 0.0; u0[p] = float4(valid ? latent : 0.0, 0.0); }
    }
    u1[p] = float4(Finite3(input) ? input : 0.0, depth);
}

void Reproject(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    float byMotion;
    const float4 moved = MovedDetail(p, byMotion);
    float4 result = input;
    const float3 reconstructed = input.rgb + moved.rgb * moved.a;
    if (Finite3(reconstructed) && all(abs(reconstructed) <= 65504.0))
        result.rgb = reconstructed;

    if (debugView != 0)
        result.rgb = lerp(lerp(float3(1.0, 0.0, 1.0), float3(1.0, 1.0, 0.0), byMotion), result.rgb, moved.a);

    u0[p] = result;
}

void Steady(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    const float4 answer = t1.Load(int3(p, 0));
    const float4 estimate = t2.Load(int3(p, 0));
    const float amount = saturate(steady) * saturate(estimate.a);
    const float3 fresh = answer.rgb - input.rgb;
    // A difference below the dead zone (about one 8-bit step) is no difference: the full frame takes the moved detail there
    // (as far as it is trusted), so rounding does not show the two frames apart. Fades out between one and two dead zones,
    // and only where steadiness is on.
    float3 pull = amount;
    if (steadyDeadZone > 0.0 && amount > 0.0)
    {
        const float3 apart = smoothstep(steadyDeadZone, 2.0 * steadyDeadZone, abs(estimate.rgb - fresh));
        pull = lerp(saturate(estimate.a), amount, apart);
    }
    const float3 steadied = input.rgb + lerp(fresh, estimate.rgb, pull);
    u0[p] = float4(Finite3(steadied) ? steadied : answer.rgb, answer.a);
}

#ifndef VK_MODE
// Source scene guides -> canonical NR work grid. This is an address/unit conversion,
// not a declaration that SR scene depth describes post-HUD pixels. ZZZ support is
// additionally conditional on SAME-source scene MV agreeing with final-picture flow.
void PrepareSceneGuides(uint2 p)
{
    const float2 uv = WorkUv(p);
    const float2 depthUv = uv + float2(nativeDepthPhaseUvX, nativeDepthPhaseUvY);
    const float2 motionUv = uv + float2(nativeMotionPhaseUvX, nativeMotionPhaseUvY);
    int2 d = GuideTexel(depthUv, uint2(depthWidth, depthHeight));
    int2 m = GuideTexel(motionUv, uint2(motionWidth, motionHeight));
    if ((sceneGuideFlags & 1u) != 0u) d.y = int(depthHeight) - 1 - d.y;
    if ((sceneGuideFlags & 2u) != 0u) m.y = int(motionHeight) - 1 - m.y;
    const float rawDepth = t4.Load(int3(d + int2(depthBaseX, depthBaseY), 0)).r;
    const float2 raw = t3.Load(int3(m + int2(motionBaseX, motionBaseY), 0)).xy;
    float2 scale = float2(mvScaleX, mvScaleY);
    if ((sceneGuideFlags & 4u) != 0u) scale.y = -scale.y;
    const float2 delta = raw * scale / float2(motionWidth, motionHeight);
    const float z = FarIsZero(rawDepth);
    const bool depthFinite = isfinite(rawDepth) && rawDepth >= 0.0 && rawDepth <= 1.0;
    const bool motionFinite = all(isfinite(delta)) && all(abs(raw) < 65504.0);
    bool valid = depthFinite && z > 0.0 && motionFinite &&
        all(depthUv >= 0.0) && all(depthUv < 1.0) && all(motionUv >= 0.0) && all(motionUv < 1.0);
    float support = valid ? 1.0 : 0.0;
    if ((sceneGuideFlags & 8u) != 0u)
    {
        const float2 pictureSize = float2(scenePictureWidth, scenePictureHeight);
        const float2 flow = t5.Load(int3(GuideTexel(uv, uint2(scenePictureWidth, scenePictureHeight)), 0)).xy;
        const float distrust = t2.SampleLevel(gLinear, uv, 0).r;
        const float2 uncertainty = 1.0 / float2(motionWidth, motionHeight) + 2.0 / pictureSize;
        const float2 error = abs(delta - flow / pictureSize) / uncertainty;
        const float agreement = 1.0 - smoothstep(1.0, 2.0, max(error.x, error.y));
        support = all(isfinite(flow)) && isfinite(distrust)
            ? min(support, min(agreement, 1.0 - saturate(distrust))) : 0.0;
    }
    // Z is far-zero and conditional. XY remains true scene motion in work pixels.
    u0[p] = float4(motionFinite ? delta * WorkSize() : 65504.0, valid ? z : -1.0, support);
    // Model depth is the true scene DeviceZ with canonical sample addressing;
    // flow disagreement never turns it into synthetic final-picture depth.
    u1[p] = float4(depthFinite ? rawDepth : (depthInverted != 0u ? 0.0 : 1.0), 0.0, 0.0, 0.0);
}

// Both full-model feedback paths use the same RGB disagreement bound in proxy units.
float3 BoundHistoricalResidual(float3 fresh, float3 historical)
{
    const float floor = max(sigmaFloor, 1e-6);
    const float freshSize = max(max(abs(fresh.r), abs(fresh.g)), abs(fresh.b));
    const float3 disagreement = historical - fresh;
    const float distance = max(max(abs(disagreement.r), abs(disagreement.g)), abs(disagreement.b));
    return fresh + disagreement * min(1.0, max(freshSize, floor) / max(distance, floor));
}

// Full-model feedback stores the edit in positive light, never an old picture.
// SDR uses the exact sRGB pair; Replace uses the resolve's exact inverse curve.
// The host excludes lossy/signed HDR composed domains. The ratio floor shares
// the resolve's normalized-white units, not a proxy-space brightness threshold.
bool FeedbackProxyValid(float3 proxy)
{
    return Finite3(proxy) && all(proxy >= 0.0) && all(proxy <= 1.0);
}
float3 FeedbackGain(uint2 p, Texture2D<float4> answerTexture, out bool valid)
{
    const float3 inputProxy = t0.Load(int3(p, 0)).rgb;
    const float3 answerProxy = answerTexture.Load(int3(p, 0)).rgb;
    const float3 input = FeedbackLight(inputProxy);
    const float3 answer = FeedbackLight(answerProxy);
    const float3 gain = log2((answer + kRatioFloor) / (input + kRatioFloor));
    valid = FeedbackProxyValid(inputProxy) && FeedbackProxyValid(answerProxy) &&
        Finite3(input) && Finite3(answer) && all(input >= 0.0) && all(answer >= 0.0) && Finite3(gain);
    return valid ? gain : 0.0;
}

// One regional split of this source's genuine model edit and the already-moved
// cohort. A 3x3 tent spans tens of work pixels, rather than a per-pixel variance.
// Invalid neighbour history uses fresh there and cannot grant regional support.
void FeedbackBands(uint2 p, out float3 fresh, out float3 lowNow, out float3 lowHistory,
    out float scale, out float regionTrust, out bool valid)
{
    fresh = FeedbackGain(p, t1, valid);
    lowNow = 0.0; lowHistory = 0.0;
    float3 square = 0.0;
    regionTrust = 1.0;
    const int pitch = max(1, int(round(float(workHeight) / 108.0)));
    [loop] for (int y = -1; y <= 1; ++y)
    {
        [loop] for (int x = -1; x <= 1; ++x)
        {
            const uint2 q = uint2(clamp(int2(p) + int2(x, y) * pitch, int2(0, 0), int2(WorkSize()) - 1));
            const float weight = float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) / 16.0;
            bool tapValid;
            const float3 gain = FeedbackGain(q, t1, tapValid);
            valid = valid && tapValid;
            float3 old = gain;
            if (temporalFrameKind != 2u)
            {
                const float4 estimate = t2.Load(int3(q, 0));
                const bool oldValid = Finite3(estimate.rgb) && isfinite(estimate.a) && estimate.a > 0.0;
                old = oldValid ? estimate.rgb : gain;
                regionTrust = min(regionTrust, oldValid ? saturate(estimate.a) : 0.0);
            }
            lowNow += gain * weight;
            lowHistory += old * weight;
            square += gain * gain * weight;
        }
    }
    const float3 sigma = sqrt(max(square - lowNow * lowNow, 0.0));
    scale = max(0.02, max(sigma.r, max(sigma.g, sigma.b)));
}

// A real adjacent-input context gate, independent of the model's edit. Each
// regional tap follows its own authenticated current->previous motion. t5 is
// the retained previous RAW input for this mode, never model output/readback.
// Exposure, colour or revealed content changes smoothly release both bands;
// subtracting the mean here would hide a real exposure change and cause lag.
float FeedbackContextTrust(uint2 p)
{
    float3 deltaSquare = 0.0;
    const int pitch = max(1, int(round(float(workHeight) / 108.0)));
    [loop] for (int y = -1; y <= 1; ++y)
    {
        [loop] for (int x = -1; x <= 1; ++x)
        {
            const uint2 q = uint2(clamp(int2(p) + int2(x, y) * pitch, int2(0, 0), int2(WorkSize()) - 1));
            const float2 uv = WorkUv(q);
            const float2 beforeUv = uv + UvDisplacement(RawMotion(uv));
            if (!all(isfinite(beforeUv)) || any(beforeUv < 0.0) || any(beforeUv > 1.0)) return 0.0;
            const float3 now = t0.Load(int3(q, 0)).rgb;
            const float3 before = t5.SampleLevel(gLinear, beforeUv, 0).rgb;
            if (!FeedbackProxyValid(now) || !FeedbackProxyValid(before)) return 0.0;
            const float3 a = FeedbackLight(now), b = FeedbackLight(before);
            if (!Finite3(a) || !Finite3(b) || any(a < 0.0) || any(b < 0.0)) return 0.0;
            const float3 delta = log2((a + kRatioFloor) / (b + kRatioFloor));
            const float weight = float((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) / 16.0;
            deltaSquare += delta * delta * weight;
        }
    }
    const float3 change = sqrt(max(deltaSquare, 0.0));
    return 1.0 - smoothstep(0.0, 0.12, max(change.r, max(change.g, change.b)));
}

// Independent full-model cohort capture. Reset branches do not read an old
// estimate. The display composite is deliberately not a capture input.
void HandoffCapture(uint2 p)
{
    const float3 input = t0.Load(int3(p, 0)).rgb;
    const float3 freshResidual = t1.Load(int3(p, 0)).rgb - input;
    float scale = 0.0;
    bool contentValid = true;
    float3 fresh = freshResidual;
    float3 lowNow = 0.0, lowHistory = 0.0;
    float regionTrust = 0.0;
    [branch] if (DetailContent())
        FeedbackBands(p, fresh, lowNow, lowHistory, scale, regionTrust, contentValid);
    float3 latent = fresh;
    float confidence = 1.0;
    if (temporalFrameKind != 2u)
    {
        const float4 estimate = t2.Load(int3(p, 0));
        const float amount = saturate(steady) * saturate(estimate.a);
        if (Finite3(fresh) && Finite3(estimate.rgb) && isfinite(estimate.a) &&
            isfinite(amount) && amount > 0.0)
        {
            [branch] if (DetailContent())
            {
                // Both bands require stable adjacent real input. Detail also has
                // its own disagreement/support in log-light units, so an input
                // exposure or content change cannot keep renewing an old edge.
                const float3 freshHigh = fresh - lowNow;
                const float3 oldHigh = estimate.rgb - lowHistory;
                const float3 disagreement = oldHigh - freshHigh;
                const float distance = max(abs(disagreement.r), max(abs(disagreement.g), abs(disagreement.b)));
                const float support = 1.0 - smoothstep(scale, 3.0 * scale, distance);
                const float contextTrust = FeedbackContextTrust(p);
                const float highPull = amount * support * contextTrust;
                const float3 carriedHigh = freshHigh + disagreement * min(1.0, scale / max(distance, 1e-6));
                const float3 lowDifference = abs(lowHistory - lowNow);
                const float lowDistance = max(lowDifference.r, max(lowDifference.g, lowDifference.b));
                const float lowSupport = 1.0 - smoothstep(0.25, 1.0, lowDistance);
                const float lowPull = saturate(steady) * regionTrust * lowSupport * contextTrust;
                latent = lerp(lowNow, lowHistory, lowPull) + lerp(freshHigh, carriedHigh, highPull);
                // Replenish only the genuine fresh share. This is confidence in the
                // mixture, not a new birth time for carried content; the unchanged
                // cohort deadline still bounds its oldest possible dependency.
                confidence = lerp(1.0, saturate(estimate.a), max(lowPull, highPull));
            }
            else
            {
                const float3 carried = BoundHistoricalResidual(fresh, estimate.rgb);
                latent = lerp(fresh, carried, amount);
            }
        }
    }
    bool valid = contentValid && Finite3(input) && Finite3(fresh) && Finite3(latent) && all(abs(latent) <= 65504.0);
    if (temporalMode == 1u)
    {
        const float depth = HasDepth() ? GuideDepth(t4, GuideTexel(DepthUv(WorkUv(p)),
            uint2(depthWidth, depthHeight))) : -1.0;
        valid = valid && isfinite(depth) && depth >= 0.0;
    }
    if (PictureFlowRoute()) valid = valid && (!HasDepth() || SceneGeometry()) && historyDistrust != 0u;
    if (SceneGeometry())
    {
        const float4 geometry = t4.Load(int3(GuideTexel(WorkUv(p), uint2(depthWidth, depthHeight)), 0));
        valid = valid && isfinite(geometry.z) && geometry.z > 0.0 && isfinite(geometry.a) && geometry.a > 0.0;
        confidence = min(confidence, isfinite(geometry.a) ? saturate(geometry.a) : 0.0);
    }
    u0[p] = float4(valid ? latent : 0.0, valid ? confidence : 0.0);
}

// Both histories were updated independently using the same fresh model answer.
// A generation's oldest dependency is excluded before its hard deadline; only
// its hidden slot is reseeded. Never feed this result back into either cohort.
void HandoffCompose(uint2 p)
{
    const float3 input = t0.Load(int3(p, 0)).rgb;
    const float4 answer = t3.Load(int3(p, 0));
    bool contentValid = true;
    float3 fresh = answer.rgb - input;
    [branch] if (DetailContent()) fresh = FeedbackGain(p, t3, contentValid);
    const float4 a = t1.Load(int3(p, 0));
    const float4 b = t2.Load(int3(p, 0));
    const float3 detailA = Finite3(a.rgb) && isfinite(a.a) && a.a > 0.0 ? a.rgb : fresh;
    const float3 detailB = Finite3(b.rgb) && isfinite(b.a) && b.a > 0.0 ? b.rgb : fresh;
    const float weightB = temporalHandoffIncoming != 0u ? temporalHandoffBlend : 1.0 - temporalHandoffBlend;
    const float3 detail = lerp(detailA, detailB, saturate(weightB));
    float3 result = input + detail;
    [branch] if (DetailContent())
    {
        // Apply the independently filtered edit to this source's current answer.
        // This is the floored input-light gain, expressed as a correction so
        // zero correction preserves the actual model answer exactly.
        const float3 correction = detail - fresh;
        result = answer.rgb;
        if (any(correction != 0.0))
        {
            const float3 light = FeedbackLight(answer.rgb);
            const float3 corrected = max((light + kRatioFloor) * exp2(correction) - kRatioFloor, 0.0);
            result = replaceCurve != 0u ? ReplaceEncode(corrected) : LinearToSrgb(corrected);
        }
    }
    const bool validResult = Finite3(result) && (!DetailContent() ||
        (contentValid && all(abs(result) <= 65504.0)));
    u0[p] = float4(validResult ? result : answer.rgb, answer.a);
    // Both old-guide reprojections have finished before this dispatch. Preserve
    // the native/flow missing-depth convention in their shared current guides.
    float depth = temporalMode == 0u ? 0.0 : -1.0;
    if (HasDepth())
    {
        const float supplied = GuideDepth(t4, GuideTexel(DepthUv(WorkUv(p)), uint2(depthWidth, depthHeight)));
        if (isfinite(supplied) && supplied >= 0.0) depth = supplied;
    }
    u1[p] = float4(Finite3(input) ? input : 0.0, depth);
}


// Original full-model feedback for the multi-interval path.
void FreshHistory(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    const float4 answer = t1.Load(int3(p, 0));
    const float4 estimate = t2.Load(int3(p, 0));
    const float3 fresh = answer.rgb - input.rgb;
    const float amount = saturate(steady) * saturate(estimate.a);
    if (!Finite3(fresh) || !Finite3(estimate.rgb) || !isfinite(estimate.a) ||
        !isfinite(amount) || !(amount > 0.0))
    {
        u0[p] = answer;
        return;
    }
    // Both residuals use this frame's model input and proxy units. A common
    // RGB scale limits their disagreement without per-channel hue clipping.
    const float3 carried = BoundHistoricalResidual(fresh, estimate.rgb);
    // Feedback always keeps its actual strength; the display dead zone does not apply.
    const float3 fused = input.rgb + lerp(fresh, carried, amount);
    u0[p] = float4(Finite3(fused) ? fused : answer.rgb, answer.a);
}
#endif

// Where the moved detail was dropped (another surface was there, or it came from off-screen), take the trusted moved
// detail of nearby pixels on the same surface instead of none. Uncovered background gets the background's detail from
// around it, not the body that uncovered it. The average is smooth -- mostly NR's tone rather than its fine detail --
// which is what stops the dropped areas flashing to the un-NR'd image with several passes. 16 taps on a golden-angle
// spiral out to fillRadius, each weighted by its trust and by depth agreement (within three times the reuse tolerance).
void Fill(uint2 p)
{
    const float4 input = t0.Load(int3(p, 0));
    const float4 estimate = t1.Load(int3(p, 0));
    const bool estimateOk = Finite3(estimate.rgb) && isfinite(estimate.a);
    const float trust = estimateOk ? saturate(estimate.a) : 0.0;
    const float3 own = estimateOk ? estimate.rgb * trust : 0.0;

    float filled = 0.0;
    float3 fillDetail = 0.0;
    if (trust < 0.999 && fillStrength > 0.0 && !SceneGeometry() && (temporalMode == 0u || HasDepth()))
    {
        const uint2 depthSize = uint2(depthWidth, depthHeight);
        const float2 work = WorkSize();
        const float depthHere = GuideDepth(t4, GuideTexel(DepthUv(WorkUv(p)), depthSize));
        const float tolerance = 3.0 * depthTolerance;
        float3 sum = 0.0, sumLogRatio = 0.0;
        float weightSum = 0.0;
        [unroll] for (int k = 0; k < 16; ++k)
        {
            const float radius = lerp(2.0, max(fillRadius, 2.0), sqrt((k + 0.5) / 16.0));
            const float angle = 2.39996323 * k;
            const int2 texel = clamp(int2(p) + int2(round(radius * float2(cos(angle), sin(angle)))), int2(0, 0),
                                     int2(work) - 1);
            const float4 tap = t1.Load(int3(texel, 0));
            if (!Finite3(tap.rgb) || !isfinite(tap.a) || tap.a <= 0.0)
                continue;
            const float depthThere = GuideDepth(t4, GuideTexel(DepthUv((float2(texel) + 0.5) / work), depthSize));
            const float relative = abs(depthHere - depthThere) / max(max(depthHere, depthThere), 1e-6);
            const float weight = saturate(tap.a) * (1.0 - smoothstep(tolerance, 2.0 * tolerance, relative));
            sum += tap.rgb * weight;
            weightSum += weight;
            if (replaceCurve != 0u)
            {
                // The ratio the tap's (already landed) change makes on the tap's own input.
                const float3 tapInput = t0.Load(int3(texel, 0)).rgb;
                const float3 tapRatio =
                    (ReplaceDecode(tapInput + tap.rgb) + kRatioFloor) / (ReplaceDecode(tapInput) + kRatioFloor);
                sumLogRatio += log(tapRatio) * weight;
            }
        }
        if (isfinite(depthHere) && weightSum > 1e-3)
        {
            fillDetail = sum / weightSum;
            // Full once two trusted neighbours agree; fewer fade it in.
            filled = (1.0 - trust) * saturate(fillStrength) * saturate(weightSum / 2.0);
            if (replaceCurve != 0u)
            {
                // Landed like a moved change, on the pixel as its own change left it and at the strength it is filled
                // at: the average difference, or the average ratio, whichever changes the pixel less.
                const float3 base = input.rgb + own;
                const float3 light = ReplaceDecode(base);
                const float3 asRatio = ReplaceEncode(
                    max((light + kRatioFloor) * exp(sumLogRatio / weightSum) - kRatioFloor, 0.0));
                const float3 landed = SmallerLanding(base, light, asRatio, base + fillDetail, filled);
                fillDetail = Finite3(landed) ? landed : 0.0;
            }
        }
    }

    float3 result = input.rgb + own + fillDetail * filled;
    if (!Finite3(result))
        result = input.rgb;

    // Debug: cyan where detail was filled, magenta where it is still missing.
    if (debugView != 0)
        result = result * trust + float3(0.0, 1.0, 1.0) * filled + float3(1.0, 0.0, 1.0) * max(1.0 - trust - filled, 0.0);

    u0[p] = float4(result, input.a);
}

// Work size: this frame's vectors as a uv displacement, so the next frame reads them right after a render-size change.
// How much of this frame arrived with no detail to move (DlssNrDetailReuse::MotionGuard, dlssnr/DlssNrDetailReuse.h):
// the moved detail's trust summed over a tile of the frame, one 8x8 thread group per tile. Every second pixel in each
// direction is measured, which is ample for a share. Written as sums rather than means, so the host can add tiles of
// unequal size exactly. Reads the estimate the same way Fill does: anything not finite is no detail at all.
static const uint kCoverageTiles = 32; // = kDlssNrDetailReuseCoverageTiles (DlssNr_DetailReuseConstants.h)

groupshared float2 gCoverage[64];

void Coverage(uint2 tile, uint2 lanePos)
{
    // The tiles partition the frame exactly: no pixel is measured twice and none is missed. A frame narrower than the
    // grid leaves the last tiles empty (x0 == x1) rather than letting neighbours overlap, which would weight some
    // columns double and bias the share.
    const uint lane = lanePos.y * 8u + lanePos.x;
    const uint x0 = min((tile.x * workWidth) / kCoverageTiles, workWidth);
    const uint x1 = min(((tile.x + 1u) * workWidth) / kCoverageTiles, workWidth);
    const uint y0 = min((tile.y * workHeight) / kCoverageTiles, workHeight);
    const uint y1 = min(((tile.y + 1u) * workHeight) / kCoverageTiles, workHeight);

    float2 sum = 0.0; // dropped (1 - trust), pixels measured
    [loop] for (uint y = y0 + lanePos.y * 2u; y < y1; y += 16u)
    {
        [loop] for (uint x = x0 + lanePos.x * 2u; x < x1; x += 16u)
        {
            const float4 estimate = t1.Load(int3(x, y, 0));
            const float trust = Finite3(estimate.rgb) && isfinite(estimate.a) ? saturate(estimate.a) : 0.0;
            sum += float2(1.0 - trust, 1.0);
        }
    }

    gCoverage[lane] = sum;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gCoverage[lane] += gCoverage[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
        u0[tile] = float4(gCoverage[0].x, gCoverage[0].y, 0.0, 0.0);
}

void SaveMotion(uint2 p)
{
    const float2 displacement = UvDisplacement(RawMotion(WorkUv(p)));
    u0[p] = float4(all(isfinite(displacement)) ? displacement : 0.0, 0.0, 0.0);
}

// The second reused frame in a row (every 3rd frame): this frame's displacement plus the saved one (the reused frame
// before) where it leads, so the next full frame composes back to the model's last run. Point-sampled, as Compose.
void AccumulateMotion(uint2 p)
{
    const float2 uv = WorkUv(p);
    const float2 displacement = UvDisplacement(RawMotion(uv));
    if (!all(isfinite(displacement)))
    {
        u0[p] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float2 sum = displacement;
    const float2 previousUv = uv + displacement;
    // Off screen there is no saved displacement: the one frame we know stands in, as Compose does
    if (all(previousUv >= 0.0) && all(previousUv <= 1.0))
    {
        const float2 prior = t4.Load(int3(GuideTexel(previousUv, uint2(workWidth, workHeight)), 0)).rg;
        if (all(isfinite(prior)) && all(abs(displacement + prior) <= 1.0))
            sum = displacement + prior;
    }
    u0[p] = float4(sum, 0.0, 0.0);
}

// Motion size: this frame's raw vector plus the saved displacement at the moved position, back in this frame's raw
// units. Vectors are point-sampled: blending two surfaces' vectors across an edge gives one that belongs to neither.
void Compose(uint2 q)
{
    const float2 raw = t3.Load(int3(uint2(motionBaseX, motionBaseY) + q, 0)).rg;
    const float2 scale = float2(mvScaleX, mvScaleY);
    float2 composed = all(isfinite(raw)) ? raw : 0.0;

    const float2 delta = UvDisplacement(composed);
    const float2 previousUv = (float2(q) + 0.5) / MotionSize() + delta;
    // The saved displacement exists only on screen; where the point came from off-screen, the model gets the one frame
    // of motion we know instead.
    if (all(abs(scale) > 1e-20) && all(isfinite(delta)) && all(previousUv >= 0.0) && all(previousUv <= 1.0))
    {
        const float2 prior = t4.Load(int3(GuideTexel(previousUv, uint2(workWidth, workHeight)), 0)).rg;
        const float2 sum = delta + prior;
        // A sanity bound, not an on-screen test: two frames of motion across more than the whole screen is not motion.
        if (all(isfinite(sum)) && all(abs(sum) <= 1.0))
            composed = sum * MotionSize() / scale;
    }

    u0[q] = float4(composed, 0.0, 0.0);
}

#ifndef VK_MODE
#include "dlssnr_detail_history.hlsli"
#endif

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint2 p = id.xy;
    // Before the bounds test below: a whole group reduces one tile together, so every one of its threads has to reach
    // the barriers in Coverage.
    if (mode == 7)
    {
        if (groupId.x < kCoverageTiles && groupId.y < kCoverageTiles)
            Coverage(groupId.xy, groupThreadId.xy);
        return;
    }

    if (mode == 3)
    {
        if (p.x < motionWidth && p.y < motionHeight)
            Compose(p);
        return;
    }

    if (p.x >= workWidth || p.y >= workHeight)
        return;
    if (mode == 0)
        Capture(p, true);
    else if (mode == 9)
        Capture(p, false);
    else if (mode == 2)
        SaveMotion(p);
    else if (mode == 8)
        AccumulateMotion(p);
    else if (mode == 10)
        TemporalCapture(p);
    else if (mode == 1)
        Reproject(p);
    else if (mode == 4)
        Steady(p);
#ifndef VK_MODE
    else if (mode == 24)
        DetailHistoryResolve(p);
    else if (mode == 25)
        DetailHistoryInspect(p);
    else if (mode == 16)
        PrepareSceneGuides(p);
    else if (mode == 11)
        FreshHistory(p);
    else if (mode == 14)
        HandoffCapture(p);
    else if (mode == 15)
        HandoffCompose(p);
    else if (mode == 12 && PictureFlowRoute())
        TemporalCapture(p);
    else if (mode == 13 && PictureFlowRoute())
        Capture(p, false);
#endif
    else if (mode == 6)
        Fill(p);
    else
    {
        float byMotion;
        u0[p] = MovedDetail(p, byMotion);
    }
}
