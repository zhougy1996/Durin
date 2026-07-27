// Offscreen channel filtering for Texture Editor previews.

struct VSInput
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

struct PreviewSettingsUniform
{
    uint Channel;
    uint3 Padding;
};

[[vk::binding(0, 0)]] Texture2D<float4> PreviewTexture;
[[vk::binding(1, 0)]] SamplerState PreviewSampler;
[[vk::binding(2, 0)]] ConstantBuffer<PreviewSettingsUniform> PreviewSettings;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    float4 sampled = PreviewTexture.Sample(PreviewSampler, input.uv);
    uint channelIndex = PreviewSettings.Channel > 0
        ? min(PreviewSettings.Channel - 1, 3)
        : 0;
    float channelValue = sampled[channelIndex];
    return float4(channelValue, channelValue, channelValue, 1.0);
}
