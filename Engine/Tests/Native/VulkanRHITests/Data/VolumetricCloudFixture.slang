[[vk::binding(0, 0)]] Texture3D<float> BaseDensity;
[[vk::binding(1, 0)]] Texture3D<float> DetailDensity;
[[vk::binding(2, 0)]] Texture2D<float> WeatherTexture;
[[vk::binding(3, 0)]] SamplerState DensitySampler;
[[vk::binding(4, 0)]] RWTexture2D<float4> OutputImage;

[numthreads(1, 1, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    float coordinate = ((float)index + 0.5) / 2.0;
    float base = BaseDensity.SampleLevel(
        DensitySampler, float3(0.5, 0.5, coordinate), 0.0);
    float detail = DetailDensity.SampleLevel(
        DensitySampler, float3(0.5, 0.5, coordinate), 0.0);
    float weather = WeatherTexture.SampleLevel(
        DensitySampler, float2(coordinate, 0.5), 0.0);
    OutputImage[int2(index, 0)] = float4(base, detail, weather, 1.0);
}
