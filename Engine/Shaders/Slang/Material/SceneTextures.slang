// Logical material scene inputs are supplied by the fullscreen pass adapter.
// Texture UV conversion stays here; backend orientation is already in pass coordinates.
struct FMaterialSceneTextureUniform
{
    float4 Input0Viewport;
    float4 Input0TextureSize;
    float4 DepthViewport;
    float4 DepthTextureSize;
    float4 ProjectionRow0;
    float4 ProjectionRow1;
    float4 ProjectionRow2;
    float4 ProjectionRow3;
    float4 DepthParameters; // x: clear depth
};
[[vk::binding(40, 0)]] ConstantBuffer<FMaterialSceneTextureUniform> MaterialSceneTextures;
[[vk::binding(41, 0)]] Texture2D<float4> PostProcessInput0;
[[vk::binding(42, 0)]] Texture2D<float> MaterialSceneDepth;
[[vk::binding(43, 0)]] SamplerState MaterialScenePointSampler;
[[vk::binding(44, 0)]] SamplerState MaterialSceneLinearSampler;

float2 MaterialSceneTextureUV(float2 uv, float4 viewport, float4 size)
{
    float2 pixel = viewport.xy + saturate(uv) * viewport.zw;
    return clamp(pixel, viewport.xy + 0.5, viewport.xy + viewport.zw - 0.5) * size.zw;
}
float4 ReadMaterialInput0Linear(float2 uv)
{
    float2 textureUV = MaterialSceneTextureUV(uv, MaterialSceneTextures.Input0Viewport, MaterialSceneTextures.Input0TextureSize);
    float2 pixel = textureUV * MaterialSceneTextures.Input0TextureSize.xy;
    float2 center = floor(pixel) + 0.5;
    if (all(abs(pixel - center) < 1.0e-4))
        return PostProcessInput0.Load(int3(int2(floor(pixel)), 0));
    return PostProcessInput0.SampleLevel(MaterialSceneLinearSampler, textureUV, 0);
}
float4 ReadMaterialInput0Point(float2 uv)
{
    float2 textureUV = MaterialSceneTextureUV(uv, MaterialSceneTextures.Input0Viewport, MaterialSceneTextures.Input0TextureSize);
    float2 centerUV = (floor(textureUV * MaterialSceneTextures.Input0TextureSize.xy) + 0.5) * MaterialSceneTextures.Input0TextureSize.zw;
    return PostProcessInput0.SampleLevel(MaterialScenePointSampler, centerUV, 0);
}
float ReadMaterialDeviceDepth(float2 uv)
{
    float2 textureUV = MaterialSceneTextureUV(uv, MaterialSceneTextures.DepthViewport, MaterialSceneTextures.DepthTextureSize);
    float2 centerUV = (floor(textureUV * MaterialSceneTextures.DepthTextureSize.xy) + 0.5) * MaterialSceneTextures.DepthTextureSize.zw;
    return MaterialSceneDepth.SampleLevel(MaterialScenePointSampler, centerUV, 0);
}
float3 ReadMaterialSceneDepth(float2 uv)
{
    float deviceDepth = ReadMaterialDeviceDepth(uv);
    if (!isfinite(deviceDepth) || abs(deviceDepth - MaterialSceneTextures.DepthParameters.x) <= 1.0e-7)
        return float3(deviceDepth, 0, 0);
    float2 textureUV = MaterialSceneTextureUV(uv, MaterialSceneTextures.DepthViewport, MaterialSceneTextures.DepthTextureSize);
    float2 pixelCenter = floor(textureUV * MaterialSceneTextures.DepthTextureSize.xy) + 0.5;
    float2 sampleUV = (pixelCenter - MaterialSceneTextures.DepthViewport.xy) / MaterialSceneTextures.DepthViewport.zw;
    float3 viewPosition;
    bool valid = ReconstructViewPositionAnalytic(MaterialSceneTextures.ProjectionRow0,
        MaterialSceneTextures.ProjectionRow1, MaterialSceneTextures.ProjectionRow2,
        MaterialSceneTextures.ProjectionRow3, sampleUV * 2.0 - 1.0, deviceDepth, viewPosition);
    float depth = valid ? abs(viewPosition.z) : 0;
    valid = valid && isfinite(depth) && depth > 0;
    return float3(deviceDepth, valid ? depth : 0, valid ? 1 : 0);
}
