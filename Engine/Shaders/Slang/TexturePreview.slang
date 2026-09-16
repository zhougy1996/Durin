// Offscreen channel filtering for Texture Editor previews.

import Material.SurfaceMaterial;

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
    uint DecodeNormal;
    uint InputSRGB;
    uint Padding;
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
    // Recover stored values for explicit inspection before interpretation.
    if (PreviewSettings.InputSRGB != 0)
        sampled.rgb = select(sampled.rgb <= 0.0031308, sampled.rgb * 12.92,
            1.055 * pow(max(sampled.rgb, 0.0), 1.0 / 2.4) - 0.055);
    if (PreviewSettings.DecodeNormal != 0)
    {
        // Match material sampling, then map the signed normal to display RGB.
        sampled = float4(DecodeTextureNormal(sampled.rg) * 0.5 + 0.5, 1.0);
    }
    if (PreviewSettings.Channel != 0)
    {
        float value = sampled[min(PreviewSettings.Channel - 1, 3)];
        sampled = float4(value, value, value, 1.0);
    }
    // The shared output is sRGB, like cached thumbnails and other UI images.
    // Convert display values to linear here; the render target encodes them once.
    float3 linear = select(sampled.rgb <= 0.04045, sampled.rgb / 12.92,
        pow(max((sampled.rgb + 0.055) / 1.055, 0.0), 2.4));
    return float4(linear, sampled.a);
}
