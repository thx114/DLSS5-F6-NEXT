// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace GuidePreviewShader {
// Extracted from the original NR guide visualization. No NR/model dependency.
struct Constants {
    uint32_t width,height,mode,flags;
    uint32_t rect[4],otherRect[4];
    float scaleX,scaleY,gain,white;
};
static_assert(sizeof(Constants)==64);
inline constexpr char Compute[]=R"(
cbuffer Params:register(b0){uint w,h,mode,flags;uint4 rect;uint4 otherRect;float sx,sy,gain,white;}
Texture2D<float4> source:register(t0);Texture2D<float4> other:register(t1);
RWTexture2D<float4> target:register(u0);
uint2 address(float2 uv,uint4 r){return r.xy+min(uint2(uv*float2(r.zw)),r.zw-1);}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;float2 uv=(float2(id.xy)+.5)/float2(w,h);if(flags&2)uv.y=1-uv.y;
 float4 v=source.Load(int3(address(uv,rect),0));float3 rgb;
 if(!all(isfinite(v)))rgb=float3(1,0,1);
 else if(mode==3){float z=saturate(v.x);float nearZ=(flags&4)?z:1-z;
   rgb=(flags&1)?z.xxx:(log2(1+65535*nearZ)/16).xxx;}
 else if(mode==4){float2 mv=v.xy*float2(sx,sy);
   rgb=float3(saturate(.5+mv*gain),saturate(length(mv)*gain));}
 else if(mode==8){float delta=v.r;if(flags&16){float3 fresh=other.Load(int3(address(uv,otherRect),0)).rgb;
   delta=all(isfinite(fresh))?max(max(abs(v.r-fresh.r),abs(v.g-fresh.g)),abs(v.b-fresh.b)):-1;}
   rgb=delta<0?float3(1,0,1):saturate(delta*gain).xxx;}
 else {rgb=max(v.rgb,0);if(flags&8)rgb=rgb/(max(white,.0001)+rgb);rgb=saturate(rgb);}
 target[id.xy]=float4(rgb,1);
})";
inline constexpr char Display[]=R"(
cbuffer Params:register(b0){float outW,outH,srcW,srcH;}Texture2D<float4> image:register(t0);
float4 vs(uint id:SV_VertexID):SV_Position {return float4((id==2)?3:-1,(id==1)?3:-1,0,1);}
float4 ps(float4 p:SV_Position):SV_Target {
 float scale=min(outW/srcW,outH/srcH);float2 size=float2(srcW,srcH)*scale;
 float2 uv=(p.xy-(float2(outW,outH)-size)*.5)/size;
 if(any(uv<0)||any(uv>=1))return float4(0,0,0,1);
 return image.Load(int3(min(uint2(uv*float2(srcW,srcH)),uint2(srcW,srcH)-1),0));
})";
}
