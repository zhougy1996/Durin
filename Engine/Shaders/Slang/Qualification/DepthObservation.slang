// Qualification-only bit-preserving observation; no depth transfer API extension.
module DepthObservation;
[[vk::binding(0, 0)]] Texture2D<float> Depth;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> Output;
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Depth.GetDimensions(width, height);
    if (id.x < width && id.y < height)
        Output[id.y * width + id.x] = asuint(Depth.Load(int3(id.xy, 0)));
}
