cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint offset;
    uint velocity;
    uint rowFlip;
    uint negateMotionY;
};

Texture2D<float3> SourceTexture : register(t0);
RWTexture2D<float3> DestinationTexture : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint inputWidth, inputHeight, outputWidth, outputHeight;
    SourceTexture.GetDimensions(inputWidth, inputHeight);
    DestinationTexture.GetDimensions(outputWidth, outputHeight);
    // Reject the entire invalid region before unsigned subtraction/addition.
    if (width == 0 || height == 0 || width > inputWidth || width > outputWidth ||
        height > inputHeight || height > outputHeight)
        return;
    if (offset > inputHeight - height)
        return;
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    uint2 source = uint2(dispatchThreadID.x, dispatchThreadID.y + offset);
    uint2 destination = uint2(dispatchThreadID.x,
                             rowFlip != 0 ? height - 1 - dispatchThreadID.y : dispatchThreadID.y);
    float3 value = SourceTexture.Load(int3(source, 0));
    if (velocity != 0)
        value = float3(value.x, negateMotionY != 0 ? -value.y : value.y, 0);
    DestinationTexture[destination] = value;
}
