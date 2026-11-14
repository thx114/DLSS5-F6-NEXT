texture BackBuffer : COLOR;
sampler BackSampler { Texture = BackBuffer; };
uniform float AddRed = 32.0 / 255.0;
void VS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 AddPS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(BackSampler, uv) + float4(AddRed, 0, 0, 0);
}
float4 ScalePS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(BackSampler, uv);
    color.r *= 2;
    return color;
}
technique Add { pass { VertexShader = VS; PixelShader = AddPS; } }
technique Scale { pass { VertexShader = VS; PixelShader = ScalePS; } }
