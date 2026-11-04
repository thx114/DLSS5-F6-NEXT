// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// New BridgeDepth bytecode entries for Luna's generation lane. No runtime HLSL
// compilation. PS entries execute only into private mapping surfaces.
namespace GenshinDepthMappingHlsl {
inline constexpr const char* Raster=R"HLSL(
Texture2D<float4> PreviousMap : register(t1);
cbuffer MappingControl : register(b0) { uint linearSample; uint3 pad; };
float4 InvalidUv(){return float4(-1,-1,0,0);}
bool ValidUv(float2 uv){return all(isfinite(uv)) && all(uv>=0) && all(uv<1);}
// Match the two real PS input signatures, including register packing.
struct SceneInput {float4 position:SV_Position;float2 uv:TEXCOORD0;float2 unused:TEXCOORD1;};
struct PictureInput {float2 uv:TEXCOORD0;float4 position:SV_Position;};
float4 SceneUv(SceneInput input):SV_Target {
    float2 uv=input.uv;
    if(!ValidUv(uv))return InvalidUv();
    return float4(uv,1,1);
}
float4 ComposeUv(PictureInput input):SV_Target {
    float4 output=InvalidUv();
    float2 uv=input.uv;
    if(ValidUv(uv)){
        uint w=0,h=0;PreviousMap.GetDimensions(w,h);
        if(w!=0 && h!=0){
            float2 result=float2(0,0);
            bool valid=true;
            if(linearSample==0){
                int2 at=int2(uv*float2(w,h));
                result=PreviousMap.Load(int3(at,0)).xy;
                valid=ValidUv(result);
            }else{
                float2 at=uv*float2(w,h)-0.5;int2 lo=int2(floor(at));float2 f=frac(at);
                [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
                    float weight=(x?f.x:1-f.x)*(y?f.y:1-f.y);
                    // Zero-weight taps do not require bounds or validity.
                    if(weight!=0){
                        int2 p=lo+int2(x,y);
                        // Reject an unsupported boundary footprint instead of inventing wrap,
                        // mirror, border, or validity at pixels not written by the scene.
                        if(any(p<0) || any(p>=int2(w,h))){
                            valid=false;
                        }else{
                            float2 m=PreviousMap.Load(int3(p,0)).xy;
                            if(ValidUv(m))result+=weight*m;
                            else valid=false;
                        }
                    }
                }
            }
            // Keep invalidity sticky; every path assigns all output components.
            if(valid && ValidUv(result))output=float4(result,1,1);
        }
    }
    return output;
}
float4 InvalidateUv(float4 position:SV_Position):SV_Target {return InvalidUv();}
)HLSL";
inline constexpr const char* Resolve=R"HLSL(
Texture2D<float> ProducerDepth:register(t0);
Texture2D<float4> PictureMap:register(t1);
RWTexture2D<float4> MappedDepth:register(u0); // x=device Z; y=scene-valid mask
cbuffer DepthMappingControl:register(b0){
    uint2 pictureSize;uint2 producerSize;
    float2 jitterUv;float clearValue;uint reversed;
};
[numthreads(8,8,1)]void ResolveDepth(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=pictureSize))return;
    // The paired mask has a real per-pixel writer. No receipt-wide coverage
    // boolean fabricates valid samples for HUD, sky, clipping or missing Z.
    MappedDepth[id.xy]=float4(clearValue,0,0,0);
    float2 m=PictureMap.Load(int3(id.xy,0)).xy;
    if(!all(isfinite(m)) || any(m<0) || any(m>=1))return;
    float2 uv=m+jitterUv;
    if(!all(isfinite(uv)) || any(uv<0) || any(uv>=1))return;
    uint2 p=uint2(uv*float2(producerSize));
    if(any(p>=producerSize))return;
    float d=ProducerDepth.Load(int3(p,0));
    if(!isfinite(d) || d<0 || d>1 || d==clearValue)return;
    MappedDepth[id.xy]=float4(d,1,0,0);
}
)HLSL";
}
