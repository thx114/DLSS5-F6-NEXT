// Independent reference for the identified Guides shader branch.
// Host validates nonzero source/work rectangles, capacities, finite MV/parameters,
// states, matching frame tickets, and actual queue/fence retirement.
// This 80B cbuffer reproduces the recovered Guides field order, not NR Color's ABI.
#include "nr_warp_reference.hlsli"
cbuffer GuidesConstants : register(b0)
{
    uint2 WorkSize;
    uint2 MotionSize;
    uint2 MotionBase;
    uint2 DepthSize;
    uint2 DepthBase;
    float2 UvScale;
    uint2 Axes;
    float2 Gmax;
    float2 Center;
    float2 Reserved;
};
Texture2D<float> Depth : register(t0);
Texture2D<float2> Motion : register(t1);
RWTexture2D<float> DepthOut : register(u0);
RWTexture2D<float2> MotionOut : register(u1);

[numthreads(8,8,1)]
void GuidesReference(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= WorkSize)) return;
    float2 v = (float2(id.xy)+0.5)/float2(WorkSize);
    float2 p = float2(WarpInverse4(v.x,Axes.x,Gmax.x,Center.x),
                      WarpInverse4(v.y,Axes.y,Gmax.y,Center.y));
    uint2 depthPixel = DepthBase + min(uint2(saturate(p)*float2(DepthSize)),DepthSize-1u);
    uint2 motionPixel = MotionBase + min(uint2(saturate(p)*float2(MotionSize)),MotionSize-1u);
    DepthOut[id.xy] = Depth.Load(int3(depthPixel,0));
    // Sampling clamps p; the displacement endpoint uses the unclamped p.
    float2 endpoint = p + Motion.Load(int3(motionPixel,0))*UvScale;
    float2 mapped = float2(WarpForward(endpoint.x,Axes.x,Gmax.x,Center.x),
                           WarpForward(endpoint.y,Axes.y,Gmax.y,Center.y));
    MotionOut[id.xy] = (mapped-v)*float2(WorkSize);
}
