[[vk::binding(0, 0)]] Texture2D<float4> Inputs[2];
[[vk::binding(1, 0)]] SamplerState InputSamplers[2];
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(2, 1, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    const float4 sampled = Inputs[index].SampleLevel(
        InputSamplers[index], float2(0.5, 0.5), 0.0);
    const uint red = uint(round(sampled.r * 255.0));
    const uint green = uint(round(sampled.g * 255.0));
    Output[index] = red | (green << 8);
}
