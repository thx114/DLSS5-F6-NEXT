// Independently written separable mapping. Not a full codec or runtime owner.
// Host validates finite parameters, Gmax>0, center in [0,1], and enabled axes
// with positive slope. B may be zero: inner/outer branches cover the full axis.
// No float32 numerical parity is claimed by CPU tests.
#ifndef ENCORE_F7_NR_WARP_REFERENCE
#define ENCORE_F7_NR_WARP_REFERENCE
float3 UnpackAxis(uint packed)
{
    return float3(packed & 4095u, (packed >> 12u) & 4095u, (packed >> 24u) & 127u)
         * float3(asfloat(0x39800801), 1.0/2048.0, 1.0/128.0);
}

float WarpCurve(float d, float3 p)
{
    float result = d;
    if (d > p.x)
    {
        float k = 1.0 - p.z;
        if (d >= p.x + p.y) result = p.x + p.y*(1.0-k*0.5) + p.z*(d-p.x-p.y);
        else
        {
            float t = (d-p.x)/p.y;
            result = p.x + p.y*(t-k*(t*t*t-0.5*t*t*t*t));
        }
    }
    return result;
}

float WarpInverseCurve4(float d, float3 p)
{
    float result = d;
    if (d > p.x)
    {
        float k = 1.0 - p.z;
        float edge = p.x + p.y*(1.0-k*0.5);
        if (d >= edge) result = p.x+p.y+(d-edge)/p.z;
        else
        {
            float q = (d-p.x)/p.y;
            float t = q;
            [unroll] for (uint i=0u; i<4u; ++i)
            {
                float f = t-k*(t*t*t-0.5*t*t*t*t)-q;
                float derivative = 1.0-k*(3.0*t*t-2.0*t*t*t);
                t = saturate(t-f/derivative);
            }
            result = p.x+p.y*t;
        }
    }
    return result;
}

float WarpForward(float u, uint packed, float gmax, float center)
{
    float result = u;
    if ((packed & 0x80000000u)!=0u)
    {
        float3 p = UnpackAxis(packed);
        if (center==0.5)
        {
            float d = 2.0*u-1.0;
            result = 0.5 + 0.5*sign(d)*WarpCurve(abs(d),p)/gmax;
        }
        else
        {
            float d = u-center;
            result = (WarpCurve(2.0*center,p)+sign(d)*WarpCurve(2.0*abs(d),p))/(2.0*gmax);
        }
    }
    return result;
}

float WarpInverse4(float v, uint packed, float gmax, float center)
{
    float result = v;
    if ((packed & 0x80000000u)!=0u)
    {
        float3 p = UnpackAxis(packed);
        if (center==0.5)
        {
            float d = 2.0*v-1.0;
            result = 0.5 + 0.5*sign(d)*WarpInverseCurve4(abs(d)*gmax,p);
        }
        else
        {
            float d = 2.0*v*gmax-WarpCurve(2.0*center,p);
            result = center + 0.5*sign(d)*WarpInverseCurve4(abs(d),p);
        }
    }
    return result;
}

float CodecForward(float u, uint outputExtent, uint workExtent, uint packed, float gmax, float center)
{
    return outputExtent==workExtent ? u : WarpForward(u,packed,gmax,center);
}
float CodecInverse4(float v, uint outputExtent, uint workExtent, uint packed, float gmax, float center)
{
    return outputExtent==workExtent ? v : WarpInverse4(v,packed,gmax,center);
}
#endif
