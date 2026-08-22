module VolumetricCloudShadow;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

struct CloudShadowUniform
{
    float4x4 InverseViewProjection;
    float4 Layer;       // min Z, max Z, light extinction, reversed Z
    float4 Density;     // coverage, erosion, sample count, unused
    float4 BaseFrequency;
    float4 DetailFrequency;
    float4 WindOffset;
    float4 Weather;
    float4 LightDirection;
    float4 Viewport;    // inverse local extent.xy, absolute origin.zw
};

[[vk::binding(0, 0)]] Texture3D<float> BaseDensity;
[[vk::binding(1, 0)]] Texture3D<float> DetailDensity;
[[vk::binding(2, 0)]] Texture2D<float> WeatherTexture;
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(4, 0)]] SamplerState DensitySampler;
[[vk::binding(5, 0)]] ConstantBuffer<CloudShadowUniform> Params;
[[vk::binding(6, 0)]] RWTexture2D<unorm float> CloudVisibilityOutput;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

float3 ReconstructWorld(float2 localUV, float deviceDepth)
{
    float4 worldH = mul(Params.InverseViewProjection,
        float4(localUV * 2.0 - 1.0, deviceDepth, 1.0));
    return worldH.xyz / worldH.w;
}

float SampleDensity(float3 position)
{
    float layerHeight = max(Params.Layer.y - Params.Layer.x, 1.0e-4);
    float height = saturate((position.z - Params.Layer.x) / layerHeight);
    float heightProfile = saturate(min(height / 0.15, (1.0 - height) / 0.20));
    float base = saturate(BaseDensity.SampleLevel(DensitySampler,
        frac(position * Params.BaseFrequency.xyz + Params.WindOffset.xyz), 0.0));
    float detail = saturate(DetailDensity.SampleLevel(DensitySampler,
        frac(position * Params.DetailFrequency.xyz
            + Params.WindOffset.xyz * 3.7), 0.0));
    float weather = saturate(WeatherTexture.SampleLevel(DensitySampler,
        frac(position.xy * Params.Weather.xy + Params.Weather.zw), 0.0));
    float coverage = saturate(Params.Density.x * weather);
    float shape = coverage > 0.0
        ? saturate((base - (1.0 - coverage)) / max(coverage, 1.0e-4)) : 0.0;
    return saturate(shape - detail * Params.Density.y) * heightProfile;
}

bool IntersectLayer(float3 origin, float3 direction,
    out float nearDistance, out float farDistance)
{
    nearDistance = 0.0;
    farDistance = 0.0;
    if (abs(direction.z) <= 1.0e-7)
        return false;
    float first = (Params.Layer.x - origin.z) / direction.z;
    float second = (Params.Layer.y - origin.z) / direction.z;
    nearDistance = max(0.0, min(first, second));
    farDistance = max(first, second);
    return farDistance > nearDistance;
}

float CalculateCloudVisibility(int2 pixel)
{
    float deviceDepth = SceneDepth.Load(int3(pixel, 0));
    bool sceneValid = Params.Layer.w > 0.5
        ? deviceDepth > 0.001 : deviceDepth < 0.999;
    if (!sceneValid)
        return 1.0;
    float2 localUV = (float2(pixel) + 0.5 - Params.Viewport.zw)
        * Params.Viewport.xy;
    float3 receiver = ReconstructWorld(localUV, deviceDepth);
    float3 toLight = normalize(Params.LightDirection.xyz);
    float nearDistance;
    float farDistance;
    if (!IntersectLayer(receiver, toLight, nearDistance, farDistance))
        return 1.0;
    int sampleCount = clamp((int)round(Params.Density.z), 1, 8);
    float stepDistance = (farDistance - nearDistance) / (float)sampleCount;
    float opticalDepth = 0.0;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        float distance = nearDistance + ((float)sampleIndex + 0.5) * stepDistance;
        opticalDepth += SampleDensity(receiver + toLight * distance) * stepDistance;
    }
    return saturate(exp(-opticalDepth * Params.Layer.z));
}

[shader("fragment")]
float CloudVisibilityFragmentMain(VSOutput input) : SV_Target0
{
    return CalculateCloudVisibility(int2(input.pos.xy));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CloudVisibilityComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    CloudVisibilityOutput.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;
    int2 pixel = int2(dispatchThreadId.xy);
    int2 origin = int2(Params.Viewport.zw);
    int2 extent = max(int2(round(rcp(Params.Viewport.xy))), int2(1));
    bool inside = all(pixel >= origin) && all(pixel < origin + extent);
    CloudVisibilityOutput[pixel] = inside ? CalculateCloudVisibility(pixel) : 1.0;
}
