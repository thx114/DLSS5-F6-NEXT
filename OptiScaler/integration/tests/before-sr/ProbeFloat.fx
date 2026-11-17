texture BackBuffer : COLOR;
sampler BackSampler { Texture = BackBuffer; };
void VS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(BackSampler, uv);
    return float4(color.rgb * 0.25 + float3(0.125, 0, 0), color.a);
}
technique ProbeFloat { pass { VertexShader = VS; PixelShader = PS; } }
