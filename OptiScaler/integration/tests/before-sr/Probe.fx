texture BackBuffer : COLOR;
sampler BackSampler { Texture = BackBuffer; };
sampler SrgbSampler { Texture = BackBuffer; SRGBTexture = true; };
void VS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(BackSampler, uv) + float4(32.0 / 255.0, 0, 0, 0);
}
technique Probe { pass { VertexShader = VS; PixelShader = PS; } }
float4 SrgbIdentity(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(SrgbSampler, uv);
}
technique SrgbRoundtrip { pass { VertexShader = VS; PixelShader = SrgbIdentity; SRGBWriteEnable = true; } }
