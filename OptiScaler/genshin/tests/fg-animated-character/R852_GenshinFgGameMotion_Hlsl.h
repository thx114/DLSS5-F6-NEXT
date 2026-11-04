#pragma once
namespace GenshinFgGameMotionHlsl {
inline constexpr const char* Source=R"HLSL(
// Native-accumulate -> display-resolution DLSSG guides. One pass, no CPU map.
cbuffer Control : register(b0) {
    uint2 pictureSize;
    uint2 producerSize;
    float2 currentCorrection;
    float2 previousCorrection;
    float clearValue;
    uint reversedZ;
    uint2 reserved;
    float2 inversePictureSize;
    float2 reserved2;
    row_major float4x4 currentClipToWorld;
    row_major float4x4 previousWorldToClip;
};
Texture2D<float2> PreparedMotion : register(t0);
Texture2D<float> PreparedDepth : register(t1);
Texture2D<float> PreviousDepth : register(t2);
RWTexture2D<float4> GameMotion : register(u0);
RWTexture2D<float> GameDepth : register(u1);

bool FiniteBits(float x) { return (asuint(x)&0x7f800000)!=0x7f800000; }
bool ValidDepth(float z) { return FiniteBits(z) && z>=0 && z<=1 && z!=clearValue; }

[numthreads(8,8,1)] void NormalizeNative(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=pictureSize))return;
    GameMotion[id.xy]=float4(65504,65504,0,0);
    GameDepth[id.xy]=clearValue;
    const float2 center=(float2(id.xy)+0.5)*inversePictureSize;
    // DLSSG and native prepared/history rows use opposite vertical bases.
    // Reflect only producer lookups; selected/raster camera coordinates stay native.
    const float2 sourceCenter=float2(center.x,1-center.y);
    const float2 nativeCenter=sourceCenter+currentCorrection;
    const int2 base=int2(floor(nativeCenter*float2(producerSize)));
    // The accumulate recipe's five-candidate nearest-depth cross. No filtering
    // or optical substitute is introduced at depth discontinuities.
    const int2 offsets[5]={int2(0,0),int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
    float nearest=0;float2 nativeMotion=0;int2 selected=base;bool found=false;
    [unroll] for(uint i=0;i<5;++i) {
        const int2 at=base+offsets[i];
        if(any(at<0)||any(at>=int2(producerSize)))continue;
        const float z=PreparedDepth.Load(int3(at,0));
        const float2 m=PreparedMotion.Load(int3(at,0));
        if(!ValidDepth(z)||!FiniteBits(m.x)||!FiniteBits(m.y))continue;
        if(!found||(reversedZ!=0?z>nearest:z<nearest)) {nearest=z;nativeMotion=m;selected=at;found=true;}
    }
    if(!found)return;
    GameDepth[id.xy]=nearest;
    // vSR = vNative + deltaCurrent - deltaPrevious. These are the actual
    // independently sealed per-frame corrections, not a common-phase guess.
    const float2 vSr=nativeMotion+currentCorrection-previousCorrection;
    const float2 pixels=vSr*float2(pictureSize);
    if(!FiniteBits(pixels.x)||!FiniteBits(pixels.y)||any(abs(pixels)>=4096))return;
    const float2 previousSource=sourceCenter+vSr;
    if(any(previousSource<0)||any(previousSource>=1))return;
    // Independently reproject the selected current surface into the previous
    // camera. Previous depth must contain that same surface; in-bounds
    // disocclusion keeps the sentinel instead of borrowing foreground motion.
    const float2 nativeUv=(float2(selected)+0.5)/float2(producerSize);
    const float2 rasterUv=nativeUv-currentCorrection;
    float4 world=mul(float4(rasterUv.x*2-1,1-rasterUv.y*2,nearest,1),currentClipToWorld);
    if(abs(world.w)<1e-12)return;world/=world.w;
    const float4 previousClip=mul(world,previousWorldToClip);
    if(abs(previousClip.w)<1e-8)return;
    const float expectedZ=previousClip.z/previousClip.w;
    // Depth and motion were selected at the nearest-depth donor, not at
    // sourceCenter. Reproject THAT same native texel for the depth test.
    // Using the output center here compares a different previous surface
    // on sloped geometry/edges and falsely invalidates correct game motion.
    const float2 previousNative=nativeUv+nativeMotion;
    const int2 previousAt=int2(floor(previousNative*float2(producerSize)));
    if(any(previousAt<0)||any(previousAt>=int2(producerSize))||!FiniteBits(expectedZ))return;
    const float previousZ=PreviousDepth.Load(int3(previousAt,0));
    if(!ValidDepth(previousZ)||abs(previousZ-expectedZ)>(0.0005+0.002*abs(expectedZ)))return;
    // Match the generic DLSSG VelocityFlip consumer adaptation: R847 already
    // reflected rows above; RF.hlsl also negates vector Y. History/depth
    // validation above stays entirely in producer coordinates.
    GameMotion[id.xy]=float4(pixels.x,-pixels.y,0,0);
}
)HLSL";
} // namespace GenshinFgGameMotionHlsl
