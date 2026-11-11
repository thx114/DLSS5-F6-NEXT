// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DLSSNR_DETAIL_HISTORY_INCLUDED
#define DLSSNR_DETAIL_HISTORY_INCLUDED

// DX12 full-model history, mode 24. One dispatch writes the display answer,
// original residual/age and birth fingerprint/current canonical depth.
// This intentionally has no dependency on old residual trust, cohorts, colour
// variance boxes, replacement curves, motion donors, or history hole filling.
// The caller publishes history only after the actual frame/queue transaction.

// Use integer IEEE-754 exponent checks: optimized FXC can fold isfinite on a
// uniform used as a divisor (observed with +Inf feedbackDurationSeconds).
// These gates must reject NaN and either infinity under production flags too.
bool DhFinite(float v) { return (asuint(v) & 0x7f800000u) != 0x7f800000u; }
bool DhFinite2(float2 v) { return all((asuint(v) & 0x7f800000u) != 0x7f800000u); }
bool DhFinite3(float3 v) { return all((asuint(v) & 0x7f800000u) != 0x7f800000u); }
bool DhFinite4(float4 v) { return all((asuint(v) & 0x7f800000u) != 0x7f800000u); }
float DhPeak(float3 v) { return max(v.x, max(v.y, v.z)); }
bool DhInside(int2 p) { return all(p >= 0) && all(p < int2(workWidth, workHeight)); }

bool DhWorkTexture(Texture2D<float4> tex)
{
    uint w, h;
    tex.GetDimensions(w, h);
    return w == workWidth && h == workHeight;
}

// Point sample actual guides in their authenticated subrects. A shifted UV or
// rect outside the resource is invalid, never clamped back onto another surface.
bool DhGuideAddress(Texture2D<float4> tex, float2 uv, uint2 base, uint2 size, out int2 q)
{
    q = 0;
    if (any(size == 0u) || !DhFinite2(uv) || any(uv < 0.0) || any(uv >= 1.0))
        return false;
    uint w, h;
    tex.GetDimensions(w, h);
    if (any(base > uint2(w, h)) || any(size > uint2(w, h) - base))
        return false;
    const uint2 local = uint2(uv * float2(size));
    if (any(local >= size)) return false;
    q = int2(base + local);
    return true;
}

bool DhDepth(Texture2D<float4> tex, float2 uv, out float z)
{
    z = -1.0;
    int2 q;
    if (!DhGuideAddress(tex, uv + float2(nativeDepthPhaseUvX, nativeDepthPhaseUvY),
                        uint2(depthBaseX, depthBaseY), uint2(depthWidth, depthHeight), q))
        return false;
    const float raw = tex.Load(int3(q, 0)).r;
    if (!DhFinite(raw) || raw < 0.0 || raw > 1.0)
        return false;
    z = depthInverted != 0u ? raw : 1.0 - raw;
    return true;
}

bool DhMotion(float2 uv, out float2 mv)
{
    mv = 0.0;
    int2 q;
    if (!DhGuideAddress(t4, uv + float2(nativeMotionPhaseUvX, nativeMotionPhaseUvY),
                        uint2(motionBaseX, motionBaseY), uint2(motionWidth, motionHeight), q))
        return false;
    const float2 raw = t4.Load(int3(q, 0)).rg;
    if (!DhFinite2(raw) || !DhFinite2(float2(mvScaleX, mvScaleY)))
        return false;
    // Raw current -> previous, already oriented by the producer. In particular
    // do not apply NativeMotionJitterCancel, SceneGuideFlags, or another Y flip.
    mv = raw * float2(mvScaleX, mvScaleY) / float2(motionWidth, motionHeight);
    return DhFinite2(mv);
}

int2 DhCross(uint i)
{
    if (i == 1u) return int2(-1, 0);
    if (i == 2u) return int2(1, 0);
    if (i == 3u) return int2(0, -1);
    if (i == 4u) return int2(0, 1);
    return int2(0, 0);
}

// Coordinates/weights are computed without a sampler. Every positive-weight
// tap must pass before any interpolation; a depth average is never authority.
void DhFootprint(float2 uv, out int2 base, out float2 f)
{
    const float2 pos = uv * float2(workWidth, workHeight) - 0.5;
    base = int2(floor(pos));
    f = pos - floor(pos);
}

float DhTapWeight(uint i, float2 f)
{
    return ((i & 1u) != 0u ? f.x : 1.0 - f.x) * ((i & 2u) != 0u ? f.y : 1.0 - f.y);
}

int2 DhTap(int2 base, uint i) { return base + int2(i & 1u, (i >> 1u) & 1u); }

bool DhBirthProxy(float2 uv, float3 current, out float3 value, out float error, out float3 spread)
{
    value = 0.0;
    error = 0.0;
    spread = 0.0;
    if (!DhFinite2(uv) || any(uv < 0.0) || any(uv > 1.0))
        return false;
    int2 base;
    float2 f;
    DhFootprint(uv, base, f);
    float3 low = 1e30, high = -1e30;
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        const float w = DhTapWeight(i, f);
        if (w <= 0.0) continue;
        const int2 q = DhTap(base, i);
        if (!DhInside(q)) return false;
        const float3 c = t3.Load(int3(q, 0)).rgb;
        if (!DhFinite3(c)) return false;
        value += w * c;
        low = min(low, c); high = max(high, c);
    }
    // Compare the actual reprojected colour, not each unfiltered texel with the
    // current filtered pixel. A correct half-pixel edge otherwise always fails.
    // Geometry/finite/age checks still apply to EVERY positive history tap;
    // DhStructure additionally compares the same displaced five-point patch.
    error = DhPeak(abs(current - value));
    spread = high - low;
    return DhFinite3(value);
}

// Match each raw-proxy point at ITS OWN actual correspondence. Camera rotation,
// perspective and articulated motion do not translate a whole patch rigidly.
// Neighbours validate structure only; the centre's residual/geometry never use
// a neighbour vector as a donor. Reuse these vector reads for discontinuities.
bool DhStructure(uint2 p, float2 previousUv, float2 centreMotion,
                 out float confidence, out float3 amplitude,
                 out float3 rawCrossContrast, out float motionConfidence, out uint rejection)
{
    confidence = 0.0;
    amplitude = 0.0;
    rawCrossContrast = 0.0;
    motionConfidence = 0.0;
    rejection = 10u;
    float3 currentCentre = 0.0, previousCentre = 0.0;
    float3 birthSpread = 0.0;
    float error = 0.0;
    float motionJump = 0.0;
    float2 neighbourMotion[4];
    [unroll] for (uint i = 0u; i < 5u; ++i)
    {
        const int2 q = clamp(int2(p) + DhCross(i), int2(0, 0), int2(workWidth, workHeight) - 1);
        const float4 currentPixel = t0.Load(int3(q, 0));
        const float4 freshPixel = t1.Load(int3(q, 0));
        const float3 current = currentPixel.rgb;
        const float3 fresh = freshPixel.rgb;
        float3 previous;
        float3 tapSpread;
        float tapError;
        float2 mapped = previousUv;
        if (i != 0u)
        {
            const float2 uv = (float2(q) + 0.5) / float2(workWidth, workHeight);
            float2 ownMotion;
            if (!DhMotion(uv, ownMotion)) { rejection = 5u; return false; }
            neighbourMotion[i - 1u] = ownMotion;
            mapped = uv + ownMotion + float2(feedbackRasterOffsetUvX, feedbackRasterOffsetUvY);
            motionJump = max(motionJump, length((ownMotion - centreMotion) * float2(workWidth, workHeight)));
        }
        if (!DhFinite4(currentPixel) || !DhFinite4(freshPixel) ||
            !DhBirthProxy(mapped, current, previous, tapError, tapSpread))
            return false;
        birthSpread = max(birthSpread, tapSpread);
        amplitude = max(amplitude, abs(fresh - current));
        error = max(error, tapError);
        if (i == 0u)
        {
            currentCentre = current;
            previousCentre = previous;
        }
        else
        {
            error = max(error, DhPeak(abs((current - currentCentre) - (previous - previousCentre))));
            // Current raw proxy alone supplies bounded per-channel edge support.
            // Reuse these four existing loads; no extra neighbourhood sampling.
            rawCrossContrast = max(rawCrossContrast, abs(current - currentCentre));
        }
    }
    // A bounded noise floor and centre signal scale, NOT local variance: a thin
    // fence cannot enlarge its own acceptance box enough to admit a shifted edge.
    const float noise = DhFinite(sigmaFloor) ? clamp(sigmaFloor, 1.0 / 1024.0, 0.1) : 0.01;
    const float limit = clipGamma > 0.0 && DhFinite(clipGamma) ? max(clipGamma, 2.0 * noise) :
        max(2.0 * noise, 0.015) + 0.05 * max(DhPeak(abs(currentCentre)), DhPeak(abs(previousCentre)));
    // Filtering must not hide opposite silhouettes behind their average. A
    // fractional edge may have up to twice the CURRENT filtered cross contrast;
    // an old alternating edge over a flat current patch has no such support.
    // Reuse the existing loads/ranges, rather than querying more neighbours.
    error = max(error, DhPeak(max(birthSpread - 2.0 * rawCrossContrast, 0.0)));
    confidence = 1.0 - smoothstep(0.25 * limit, limit, error);
    // Independent neighbour matching must not rescue a wrong centre vector
    // just because its mistaken destination has the same centre colour. At
    // least one opposite pair must agree locally with the actual centre field.
    // An affine camera warp passes exactly; a thin vertical/horizontal surface
    // can use its coherent axis without borrowing any residual from that axis.
    const float2 pixels = float2(workWidth, workHeight);
    const float curvature = min(length((0.5 * (neighbourMotion[0] + neighbourMotion[1]) - centreMotion) * pixels),
                                length((0.5 * (neighbourMotion[2] + neighbourMotion[3]) - centreMotion) * pixels));
    const float motionLimit = DhFinite(motionReject) && motionReject > 0.0 ? motionReject : 0.75;
    if (!DhFinite(curvature) || curvature >= motionLimit) { rejection = 5u; return false; }
    motionConfidence = 1.0 / (1.0 + 0.125 * max(motionJump - 1.0, 0.0));
    motionConfidence *= 1.0 - smoothstep(motionLimit / 3.0, motionLimit, curvature);
    return confidence > 0.0 && DhFinite3(amplitude) && DhFinite3(rawCrossContrast);
}

bool DhHistory(float2 uv, float z, out float3 residual, out float3 fingerprint,
               out float savedAge, out float depthConfidence, out uint rejection)
{
    residual = 0.0;
    fingerprint = 0.0;
    savedAge = -1.0;
    depthConfidence = 1.0;
    rejection = 7u;
    if (!DhFinite2(uv) || any(uv < 0.0) || any(uv > 1.0) ||
        !DhFinite(depthTolerance) || depthTolerance < 0.0)
        { rejection = 6u; return false; }
    int2 base;
    float2 f;
    DhFootprint(uv, base, f);
    const float tolerance = max(depthTolerance, 1e-6);
    // Filtering original residuals also filters their original fingerprints.
    // Use the OLDEST contributor's age: fractional motion may join valid births
    // of different ages, but can never rejuvenate one by averaging with a seed.
    // Depth is checked per tap before any interpolation, not on an average.
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        const float w = DhTapWeight(i, f);
        if (w <= 0.0) continue;
        const int2 q = DhTap(base, i);
        if (!DhInside(q)) { rejection = 6u; return false; }
        const float4 sample = t2.Load(int3(q, 0));
        const float a = sample.a;
        if (!DhFinite4(sample) || a < 0.0 || !DhFinite(a + feedbackDeltaSeconds))
            return false;
        if (a + feedbackDeltaSeconds >= feedbackMaxAgeSeconds) { rejection = 8u; return false; }
        savedAge = max(savedAge, a);
        const float4 context = t3.Load(int3(q, 0));
        if (!DhFinite4(context) || context.a < 0.0 || context.a > 1.0)
            return false;
        const float relative = abs(z - context.a) / max(max(z, context.a), 1e-6);
        if (tolerance < 1.0)
        {
            if (relative >= tolerance) { rejection = 9u; return false; }
            depthConfidence = min(depthConfidence, 1.0 - smoothstep(0.5 * tolerance, tolerance, relative));
        }
        residual += w * sample.rgb;
        fingerprint += w * context.rgb;
    }
    return savedAge >= 0.0 && DhFinite3(residual) && DhFinite3(fingerprint) && depthConfidence > 0.0;
}

struct DhResult
{
    float4 answer;
    float4 detail;
    float4 context;
    float4 stabilized; // independent residual mean and effective mean lag
    float stabilityWeight;
    float3 reprojected;
    float weight;
    uint reason;
};

DhResult DhEvaluateDetail(uint2 p)
{
    DhResult result = (DhResult)0;
    result.reason = 2u;
    const float4 proxy = t0.Load(int3(p, 0));
    const float4 fresh = t1.Load(int3(p, 0));
    const bool finite = DhFinite4(proxy) && DhFinite4(fresh);
    const float4 safeFresh = DhFinite4(fresh) ? fresh : float4(DhFinite3(proxy.rgb) ? proxy.rgb : 0.0,
                                                   DhFinite(fresh.a) ? fresh.a : 1.0);
    float3 freshResidual = finite ? fresh.rgb - proxy.rgb : 0.0;
    if (!DhFinite3(freshResidual)) freshResidual = 0.0;
    result.answer = safeFresh;
    result.detail = float4(freshResidual, 0.0);
    float z = -1.0;
    const bool validDepth = finite && DhDepth(t5, WorkUv(p), z);
    // RGB belongs to the residual's birth; alpha independently belongs to THIS
    // source's geometry. A missing depth is invalid, never synthetic far plane.
    result.context = float4(DhFinite3(proxy.rgb) ? proxy.rgb : 0.0, validDepth ? z : -1.0);
    result.reprojected = result.context.rgb;

    // Explicit branch is also the first-frame/unbound-history contract. No t2,
    // t3 or t4 dimensions or data are queried before this permission check.
    [branch] if (!finite) return result;
    [branch] if (feedbackHistoryValid == 0u) { result.reason = 1u; return result; }
    [branch] if (
        !DhFinite(feedbackDeltaSeconds) || feedbackDeltaSeconds <= 0.0 ||
        !DhFinite(feedbackDurationSeconds) || feedbackDurationSeconds <= 0.0 ||
        !DhFinite(feedbackMaxAgeSeconds) || feedbackMaxAgeSeconds <= 0.0 ||
        feedbackDeltaSeconds >= feedbackMaxAgeSeconds ||
        !DhFinite(feedbackStrength) || feedbackStrength <= 0.0 ||
        !DhFinite2(float2(feedbackRasterOffsetUvX, feedbackRasterOffsetUvY)))
        return result;
    [branch] if (!validDepth) { result.reason = 3u; return result; }
    if (!DhWorkTexture(t0) || !DhWorkTexture(t1) || !DhWorkTexture(t2) || !DhWorkTexture(t3))
        { result.reason = 4u; return result; }
    float2 mv;
    if (!DhMotion(WorkUv(p), mv)) { result.reason = 5u; return result; }
    const float2 previousUv = WorkUv(p) + mv + float2(feedbackRasterOffsetUvX, feedbackRasterOffsetUvY);
    float3 oldResidual, fingerprint;
    float savedAge, depthConfidence;
    if (!DhHistory(previousUv, z, oldResidual, fingerprint, savedAge, depthConfidence, result.reason)) return result;
    result.reprojected = fingerprint;
    const float ageNow = savedAge + feedbackDeltaSeconds;
    float colourConfidence;
    float motionConfidence;
    float3 amplitude, rawCrossContrast;
    if (!DhStructure(p, previousUv, mv, colourConfidence, amplitude, rawCrossContrast, motionConfidence, result.reason)) return result;
    // Only the displayed contribution fades. The original residual AND its
    // original fingerprint travel together; comparing with last frame's raw
    // colour instead would allow gradual drift to keep an unrelated old edit.
    const float anchorFade = min(saturate(feedbackStrength), exp(-ageNow / feedbackDurationSeconds)) *
                             saturate(1.0 - ageNow / feedbackMaxAgeSeconds);
    const float weight = anchorFade *
                         depthConfidence * colourConfidence * motionConfidence;
    if (!DhFinite(weight) || weight <= 0.0) { result.reason = 11u; return result; }
    // Persistent detail must survive a fresh model temporarily emitting NONE.
    // That is exactly when a current-residual-only envelope becomes zero.
    // Give a geometrically/photometrically verified surface a small signal-bound
    // carry budget. Neither old residual magnitude nor neighbouring edits can
    // enlarge it; even malformed huge history remains bounded. Birth matching,
    // disocclusion, motion continuity and the original age still gate the carry.
    const float3 signalBudget = 0.125 * saturate(min(abs(proxy.rgb), abs(fingerprint)));
    const float3 limit = max(max(2.0 * amplitude, 0.25 * rawCrossContrast), signalBudget);
    if (!DhFinite3(limit)) { result.reason = 13u; return result; }
    const float3 landed = clamp(oldResidual, -limit, limit);
    // Preserve means retaining a useful edit, not replacing a detailed current
    // model result with an empty, weaker or opposite old residual. That former
    // lerp visibly erased contrast every time a moving patch met a weak anchor.
    const bool3 aligned = landed * freshResidual >= 0.0;
    const bool3 stronger = abs(landed) > abs(freshResidual) + 1e-6;
    const bool3 useful = bool3(aligned.x && stronger.x, aligned.y && stronger.y, aligned.z && stronger.z);
    const float3 supported = float3(useful.x ? landed.x : freshResidual.x,
                                    useful.y ? landed.y : freshResidual.y,
                                    useful.z ? landed.z : freshResidual.z);
    // Save new ORIGINAL model channels independently of the displayed blend.
    // Each channel keeps its own corresponding birth fingerprint. With one age
    // per pixel, a mixture conservatively keeps the oldest carried age; a new
    // channel may expire earlier, but can never rejuvenate transported detail.
    const bool3 replace = bool3(
        abs(freshResidual.x) > abs(landed.x) + 1e-6 || freshResidual.x * landed.x < 0.0,
        abs(freshResidual.y) > abs(landed.y) + 1e-6 || freshResidual.y * landed.y < 0.0,
        abs(freshResidual.z) > abs(landed.z) + 1e-6 || freshResidual.z * landed.z < 0.0);
    const bool transported = !any(replace) || (!replace.x && abs(oldResidual.x) > 1e-6) ||
                             (!replace.y && abs(oldResidual.y) > 1e-6) ||
                             (!replace.z && abs(oldResidual.z) > 1e-6);
    const float3 residual = lerp(freshResidual, supported, weight);
    const float3 answer = proxy.rgb + residual;
    if (!DhFinite3(residual) || !DhFinite3(answer)) { result.reason = 13u; return result; }
    result.answer = float4(answer, fresh.a);
    const float3 saved = float3(replace.x ? freshResidual.x : oldResidual.x,
                                replace.y ? freshResidual.y : oldResidual.y,
                                replace.z ? freshResidual.z : oldResidual.z);
    const float3 birth = float3(replace.x ? proxy.x : fingerprint.x,
                                replace.y ? proxy.y : fingerprint.y,
                                replace.z ? proxy.z : fingerprint.z);
    result.detail = float4(saved, transported ? ageNow : 0.0);
    result.context = float4(transported ? birth : proxy.rgb, z);
    result.weight = weight;
    result.reason = DhPeak(abs(answer - fresh.rgb)) > 1e-5 ? 0u : 12u;
    return result;
}

// Independent, opt-in residual filter. Geometry confidence may become zero;
// the explicit floor still admits a bounded correction. It never averages the
// old displayed frame or feeds a filtered answer into long-detail ancestry.
// A bad guide uses screen-space residuals and is labelled as such, not a
// fabricated motion vector. Source identity/completion remain host authority.
DhResult DhEvaluate(uint2 p)
{
    DhResult result = DhEvaluateDetail(p);
    const float4 raw = t0.Load(int3(p, 0));
    const float4 fresh = t1.Load(int3(p, 0));
    const bool finite = DhFinite4(raw) && DhFinite4(fresh) && DhFinite4(result.answer);
    const float3 current = finite ? result.answer.rgb - raw.rgb : 0.0;
    result.stabilized = float4(DhFinite3(current) ? current : 0.0, finite ? 0.0 : -1.0);
    [branch] if (!DhFinite(feedbackStabilitySeconds) || feedbackStabilitySeconds <= 0.0) return result;
    [branch] if (!finite || feedbackHistoryValid == 0u || !DhFinite3(current) ||
        !DhFinite(feedbackDeltaSeconds) || feedbackDeltaSeconds <= 0.0 ||
        feedbackDeltaSeconds >= feedbackStabilitySeconds ||
        !DhFinite(feedbackStabilityFloor) || !DhFinite(feedbackStabilityClamp) || feedbackStabilityClamp <= 0.0 ||
        !DhFinite2(float2(feedbackRasterOffsetUvX, feedbackRasterOffsetUvY))) return result;
    if (!DhWorkTexture(t6) || !DhWorkTexture(t3)) return result;
    float2 mv;
    bool guided = DhMotion(WorkUv(p), mv);
    float2 previousUv = WorkUv(p) + (guided ? mv : float2(0.0, 0.0)) +
                        float2(feedbackRasterOffsetUvX, feedbackRasterOffsetUvY);
    if (!DhFinite2(previousUv) || any(previousUv < 0.0) || any(previousUv >= 1.0))
    {
        // A wild/out-of-frame vector grants no permission to clamp onto an edge.
        // Screen-space stabilization is a separately selected low-trust fallback.
        guided = false;
        previousUv = WorkUv(p) + float2(feedbackRasterOffsetUvX, feedbackRasterOffsetUvY);
    }
    int2 base; float2 fraction;
    DhFootprint(previousUv, base, fraction);
    float3 old = 0.0, oldRaw = 0.0;
    float oldLag = 0.0, depthTrust = 1.0;
    float z;
    const bool validDepth = DhDepth(t5, WorkUv(p), z);
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        const float w = DhTapWeight(i, fraction);
        if (w <= 0.0) continue;
        const int2 q = DhTap(base, i);
        if (!DhInside(q)) return result;
        const float4 saved = t6.Load(int3(q, 0));
        const float4 context = t3.Load(int3(q, 0));
        if (!DhFinite4(saved) || saved.a < 0.0 || !DhFinite3(context.rgb)) return result;
        old += w * saved.rgb;
        oldRaw += w * context.rgb;
        oldLag = max(oldLag, saved.a);
        if (!validDepth || !DhFinite(context.a) || context.a < 0.0 || context.a > 1.0)
            depthTrust = 0.0;
        else
        {
            const float relative = abs(z - context.a) / max(max(z, context.a), 1e-6);
            const float tolerance = max(depthTolerance, 1e-6);
            depthTrust = min(depthTrust, depthTolerance >= 1.0 ? 1.0 :
                1.0 - smoothstep(0.5 * tolerance, tolerance, relative));
        }
    }
    if (!DhFinite3(old) || !DhFinite3(oldRaw) || !DhFinite(oldLag + feedbackDeltaSeconds)) return result;
    // Raw scene changes are kept current. Only the NR edit is filtered. A
    // current-appearance gate limits silhouettes/cuts; depth and motion alone
    // never veto the user's positive low-trust floor.
    const float appearanceLimit = max(0.03, 2.0 * feedbackStabilityClamp);
    const float appearance = 1.0 - smoothstep(0.5 * appearanceLimit, appearanceLimit, DhPeak(abs(raw.rgb - oldRaw)));
    const bool reliable = guided && result.reason != 5u && result.reason != 10u && depthTrust > 0.0;
    const float guideTrust = reliable ? depthTrust : 0.0;
    const float trust = lerp(saturate(feedbackStabilityFloor), 1.0, guideTrust);
    float weight = min(0.9999, exp(-feedbackDeltaSeconds / feedbackStabilitySeconds)) * trust * appearance;
    // Bound mean temporal lag without periodic whole-screen reset/reseed pulses.
    weight = min(weight, feedbackStabilitySeconds / max(oldLag + feedbackDeltaSeconds, 1e-6));
    if (!DhFinite(weight) || weight <= 0.0) return result;
    const float3 correction = clamp(old - current, -feedbackStabilityClamp, feedbackStabilityClamp);
    const float3 stable = current + weight * correction;
    const float3 answer = raw.rgb + stable;
    if (!DhFinite3(stable) || !DhFinite3(answer)) return result;
    result.stabilized = float4(stable, weight * (oldLag + feedbackDeltaSeconds));
    result.stabilityWeight = weight;
    result.answer = float4(answer, fresh.a);
    if (DhPeak(abs(answer - fresh.rgb)) > 1e-5) result.reason = reliable ? 0u : 14u;
    else result.reason = 12u;
    return result;
}

void DetailHistoryResolve(uint2 p)
{
    const DhResult result = DhEvaluate(p);
    u0[p] = result.answer;
    u1[p] = result.detail;
    u2[p] = result.context;
    if (feedbackStabilitySeconds > 0.0) u3[p] = result.stabilized;
}

float3 DhReasonColour(uint reason)
{
    if (reason == 14u) return float3(0.55, 0.9, 0.15); // low-guide fallback contributes
    if (reason == 0u) return float3(0.1, 0.9, 0.2); // actual nonzero contribution
    if (reason == 12u) return float3(0.1, 0.25, 0.75); // valid but fresh wins
    if (reason == 1u) return 0.22; // no admitted adjacent frame
    if (reason == 8u) return float3(1.0, 0.9, 0.0); // expired
    if (reason == 9u || reason == 3u) return float3(1.0, 0.35, 0.0); // depth
    if (reason == 5u) return float3(1.0, 0.0, 1.0); // motion
    if (reason == 6u) return float3(1.0, 0.05, 0.05); // outside
    if (reason == 10u) return float3(0.0, 0.85, 0.9); // birth/structure mismatch
    return float3(0.65, 0.0, 0.15); // invalid parameter/resource/history
}

// A separate, demand-only dispatch on the SAME source and old-history bindings.
// It shares the exact evaluator and never writes algorithm/history outputs.
void DetailHistoryInspect(uint2 p)
{
    const DhResult result = DhEvaluate(p);
    float3 rgb;
    if (debugView == 1u) rgb = DhReasonColour(result.reason);
    else if (debugView == 2u)
    {
        const float age = saturate(result.detail.a / max(feedbackMaxAgeSeconds, 1e-6));
        rgb = result.reason == 0u || result.reason == 12u || result.reason == 14u ? float3(age, 1.0 - age, 0.1) : 0.12;
    }
    else if (debugView == 3u) rgb = saturate(result.reprojected);
    else if (debugView == 4u) rgb = saturate(t1.Load(int3(p, 0)).rgb);
    else if (debugView == 6u) rgb = float3(result.stabilityWeight, 0.15, 1.0 - result.stabilityWeight);
    else rgb = saturate(result.answer.rgb);
    u0[p] = float4(rgb, 1.0);
}

#endif
