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
    // Retain the 192-byte upload ABI; per-pixel motion validation does not
    // assume that object/bone positions follow these camera-only matrices.
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
    // Preserve the producer's previous-surface lookup, including object/bone
    // motion. Camera-only previous Z assumes a static world point and cannot
    // validate this lookup for animated geometry (even along the same ray).
    const float2 nativeUv=(float2(selected)+0.5)/float2(producerSize);
    // Depth and motion were selected at the nearest-depth donor, not at
    // sourceCenter. Reproject THAT same native texel for history validity.
    // Using the output center here compares a different previous surface
    // on sloped geometry/edges and falsely invalidates correct game motion.
    const float2 previousNative=nativeUv+nativeMotion;
    const int2 previousAt=int2(floor(previousNative*float2(producerSize)));
    if(any(previousAt<0)||any(previousAt>=int2(producerSize)))return;
    const float previousZ=PreviousDepth.Load(int3(previousAt,0));
    if(!ValidDepth(previousZ))return;
    // Valid previous depth alone does not prove surface identity. A depth
    // mismatch may be animation OR static disocclusion; leave that distinction
    // to DLSSG with the unchanged device-Z guide and actual game motion.
    // Match the generic DLSSG VelocityFlip consumer adaptation: R847 already
    // reflected rows above; RF.hlsl also negates vector Y. History/depth
    // validation above stays entirely in producer coordinates.
    GameMotion[id.xy]=float4(pixels.x,-pixels.y,0,0);
}
)HLSL";
} // namespace GenshinFgGameMotionHlsl
