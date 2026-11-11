// Independent reference for Encore's rectangular, non-foveated NR codec branch.
// Not a drop-in PSO or complete recovered shader. Host must validate rectangles,
// distinct resources, states, matching tickets, finite inputs/exposure/products,
// and HdrFlags in {0,1,3}. Geometry mapping/foveation is deliberately absent.
// This 32-byte cbuffer matches only the recovered prefix, not its whole 64B ABI.
cbuffer Reference : register(b0)
{
    uint2 OutSize;
    uint2 OutBase;
    uint2 WorkSize;
    uint HdrFlags;
    float Exposure;
};
Texture2D<float4> Original : register(t0);
Texture2D<float4> Proxy : register(t1);
Texture2D<float4> Neural : register(t2);
Texture2D<float> ExposureImage : register(t3);
RWTexture2D<float4> Target : register(u0);

float3 Clean(float3 value)
{
    return isfinite(value) ? max(value, 0.0) : 0.0;
}

float ResolvedExposure()
{
    float sample = 1.0;
    if (HdrFlags & 2u)
    {
        sample = ExposureImage.Load(int3(0, 0, 0));
        if (!isfinite(sample) || sample <= 0.0) sample = 1.0;
    }
    return max(asfloat(0x38d1b717), Exposure * sample);
}

float3 EncodeGamma(float3 value)
{
    value = saturate(value);
    return value <= asfloat(0x3b4d2e1c) ? value * asfloat(0x414eb852) :
        asfloat(0x3f870a3d) * pow(value, asfloat(0x3ed55555)) - asfloat(0x3d6147ae);
}

float3 DecodeGamma(float3 value)
{
    value = saturate(value);
    return value <= asfloat(0x3d25aee6) ? value * asfloat(0x3d9e8391) :
        pow((value + asfloat(0x3d6147ae)) * asfloat(0x3f72a76f), asfloat(0x4019999a));
}

float3 EncodeColor(float3 value, float exposure)
{
    value = Clean(value);
    float3 result = saturate(value);
    if (HdrFlags & 1u)
    {
        value *= exposure;
        float peak = max(value.r, max(value.g, value.b));
        if (peak > 0.75)
        {
            float knee = 0.75 + 0.25 * (1.0 - exp2((peak - 0.75) * asfloat(0xc1b8aa3b)));
            value *= knee / peak;
        }
        result = EncodeGamma(value);
    }
    return result;
}

void AddResidual(int2 pixel, float bilinear, float3 center,
                 inout float3 delta, inout float total)
{
    pixel = clamp(pixel, int2(0, 0), int2(WorkSize) - 1);
    float3 proxy = Proxy.Load(int3(pixel, 0)).rgb;
    float3 difference = proxy - center;
    float weight = bilinear * (exp2(dot(difference, difference) * asfloat(0xc29044fe)) + asfloat(0x3a83126f));
    delta += weight * (Neural.Load(int3(pixel, 0)).rgb - proxy);
    total += weight;
}

[numthreads(8, 8, 1)]
void EncodeReference(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= WorkSize)) return;
    float2 ratio = float2(OutSize) / float2(WorkSize);
    float2 low = float2(thread.xy) * ratio;
    float2 high = float2(thread.xy + 1u) * ratio;
    uint2 begin = uint2(floor(low));
    uint2 end = min(uint2(ceil(high)), OutSize);
    float3 sum = 0.0;
    float area = 0.0;
    [loop] for (uint y = begin.y; y < end.y; ++y)
    {
        [loop] for (uint x = begin.x; x < end.x; ++x)
        {
            float2 overlap = max(0.0, min(high, float2(x + 1u, y + 1u)) - max(low, float2(x, y)));
            float weight = overlap.x * overlap.y;
            sum += weight * Clean(Original.Load(int3(OutBase + uint2(x, y), 0)).rgb);
            area += weight;
        }
    }
    float3 average = area > 0.0 ? sum / area : 0.0;
    Target[thread.xy] = float4(EncodeColor(average, ResolvedExposure()), 1.0);
}

[numthreads(8, 8, 1)]
void DecodeReference(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= OutSize)) return;
    uint2 outputPixel = OutBase + thread.xy;
    float4 original = Original.Load(int3(outputPixel, 0));
    float3 center = EncodeColor(original.rgb, ResolvedExposure());
    float2 position = (float2(thread.xy) + 0.5) * float2(WorkSize) / float2(OutSize) - 0.5;
    int2 base = int2(floor(position));
    float2 fraction = position - floor(position);
    float3 delta = 0.0;
    float total = 0.0;
    AddResidual(base, (1.0 - fraction.x) * (1.0 - fraction.y), center, delta, total);
    AddResidual(base + int2(1, 0), fraction.x * (1.0 - fraction.y), center, delta, total);
    AddResidual(base + int2(0, 1), (1.0 - fraction.x) * fraction.y, center, delta, total);
    AddResidual(base + int2(1, 1), fraction.x * fraction.y, center, delta, total);
    delta = total > 0.0 ? delta / total : 0.0;
    float3 enhanced = saturate(center + delta);
    if (HdrFlags & 1u)
    {
        float3 before = DecodeGamma(center);
        float3 after = DecodeGamma(enhanced);
        float3 gain = clamp(1.0 + (after - before) / max(before, asfloat(0x3a83126f)), 0.25, 4.0);
        enhanced = Clean(original.rgb) * gain;
    }
    Target[outputPixel] = float4(enhanced, original.a);
}
