module QueueProducer;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> Values;
[[vk::binding(1, 0), vk::image_format("rgba8")]] RWTexture2D<float4> Image;
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    uint index = id.y * 8 + id.x;
    Values[index] = (index + 1) * 0x01010101u;
    Image[id.xy] = float4(0.0, 1.0, 0.0, 1.0);
}
