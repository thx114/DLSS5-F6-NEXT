// Dedicated DX12 diagnostic pass. Does not alter model inputs, motion signs or depth convention.
cbuffer Params : register(b0)
{
    uint gWidth, gHeight, gMode, gAvailable;
    uint gBaseX, gBaseY, gRegionWidth, gRegionHeight;
    float gMotionScaleX, gMotionScaleY, gMotionGain, gColourWhite;
    uint gFlipY, gRawResource, gColourHdr, gDepthInverted;
    uint4 gPadding[12];
};
Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gTarget : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;
    // Missing guide: no resource read, and every output texel is overwritten this frame.
    if (!gAvailable || !gRegionWidth || !gRegionHeight)
    {
        float c = ((id.x / 24 + id.y / 24) & 1) ? 0.18 : 0.07;
        gTarget[id.xy] = float4(c, c, c, 1);
        return;
    }
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    if (gFlipY) uv.y = 1.0 - uv.y; // lookup only; NEVER negate the vector or invert Z
    uint2 p = uint2(gBaseX, gBaseY) + min(uint2(uv * float2(gRegionWidth, gRegionHeight)),
                                                       uint2(gRegionWidth, gRegionHeight) - 1);
    float4 v = gSource.Load(int3(p, 0)); // point sample: inspect exact rows, signs and subrect
    float3 rgb;
    if (gMode == 3)
    {
        float z = saturate(v.x);
        if (gRawResource)
            rgb = z.xxx; // exact Z grayscale remains available for numeric/orientation checks
        else
        {
            // Display only: nonlinear device Z often occupies a tiny range near 0 or 1.
            // Use the declared convention for near-bright/far-dark contrast, not metric depth.
            // No model resource, sample row, vector sign or temporal state is modified.
            float proximity = gDepthInverted ? z : 1.0 - z;
            float visible = log2(1.0 + 65535.0 * proximity) / 16.0;
            rgb = visible.xxx;
        }
    }
    else if (gMode == 4)
    {
        float2 mv = v.xy * float2(gMotionScaleX, gMotionScaleY);
        rgb = float3(saturate(0.5 + mv * gMotionGain), saturate(length(mv) * gMotionGain));
    }
    else
    {
        rgb = max(v.rgb, 0);
        // Original HDR numeric values need display mapping; proxy is already display referred.
        if (gColourHdr) rgb = rgb / (max(gColourWhite, 0.0001) + rgb);
        rgb = saturate(rgb);
    }
    gTarget[id.xy] = float4(rgb, 1);
}
