texture DepthInput : DEPTH;
uniform bool DepthReady < source = "bufready_depth"; >;
uniform bool DepthReversed < source = "hoyoshade_depth_reversed"; >;
sampler DepthSampler { Texture = DepthInput; MinFilter = POINT; MagFilter = POINT; };
void VS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float z = tex2D(DepthSampler, uv).x;
#if RESHADE_DEPTH_INPUT_IS_REVERSED
    z = 1.0 - z;
    if (!DepthReversed) return 0;
#else
    if (DepthReversed) return 0;
#endif
    return DepthReady ? float4(z.xxx, 1) : 0;
}
technique DepthProbe { pass { VertexShader = VS; PixelShader = PS; } }
