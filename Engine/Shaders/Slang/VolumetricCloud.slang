module VolumetricCloud;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

struct VolumetricCloudUniform
{
    float4x4 InverseViewProjection;
    float4 Layer;       // min Z, max Z, max distance, extinction
    float4 Density;     // coverage, erosion, light extinction, ambient
    float4 Sampling;    // primary samples, light samples, cutoff, reversed Z
    float4 BaseFrequency;
    float4 DetailFrequency;
    float4 WindOffset;
    float4 Weather;     // frequency.xy, offset.zw
    float4 LightDirection;
    float4 LightColor;
    float4 CameraPosition;
    float4 Viewport;    // inverse cloud viewport extent.xy, cloud origin.zw
    float4 OutputViewport; // full-resolution origin.xy and extent.zw
    float4 Target;      // cloud extent.xy, output extent.zw
    float4 Jitter;      // subpixel cloud-target jitter.xy
};

[[vk::binding(0, 0)]] Texture3D<float> BaseDensity;
[[vk::binding(1, 0)]] Texture3D<float> DetailDensity;
[[vk::binding(2, 0)]] Texture2D<float> WeatherTexture;
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(4, 0)]] SamplerState DensitySampler;
[[vk::binding(5, 0)]] ConstantBuffer<VolumetricCloudUniform> Params;
[[vk::binding(6, 0), vk::image_format("rgba16f")]]
RWTexture2D<float4> CloudOutput;

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

bool IntersectLayer(float3 origin, float3 direction,
    out float nearDistance, out float farDistance)
{
    nearDistance = 0.0;
    farDistance = Params.Layer.z;
    if (abs(direction.z) <= 1.0e-7)
        return origin.z >= Params.Layer.x && origin.z <= Params.Layer.y;
    float first = (Params.Layer.x - origin.z) / direction.z;
    float second = (Params.Layer.y - origin.z) / direction.z;
    nearDistance = max(0.0, min(first, second));
    farDistance = min(Params.Layer.z, max(first, second));
    return farDistance > nearDistance;
}

float SampleBase(float3 position)
{
    return saturate(BaseDensity.SampleLevel(DensitySampler,
        frac(position * Params.BaseFrequency.xyz + Params.WindOffset.xyz), 0.0));
}

float SampleDensity(float3 position)
{
    float layerHeight = max(Params.Layer.y - Params.Layer.x, 1.0e-4);
    float height = saturate((position.z - Params.Layer.x) / layerHeight);
    float heightProfile = saturate(min(height / 0.15,
        (1.0 - height) / 0.20));
    float base = SampleBase(position);
    float detail = saturate(DetailDensity.SampleLevel(DensitySampler,
        frac(position * Params.DetailFrequency.xyz
            + Params.WindOffset.xyz * 3.7), 0.0));
    float weather = saturate(WeatherTexture.SampleLevel(DensitySampler,
        frac(position.xy * Params.Weather.xy + Params.Weather.zw), 0.0));
    float coverage = saturate(Params.Density.x * weather);
    float shape = coverage > 0.0
        ? saturate((base - (1.0 - coverage)) / max(coverage, 1.0e-4))
        : 0.0;
    return saturate(shape - detail * Params.Density.y) * heightProfile;
}

float4 IntegrateCloud(int2 pixel)
{
    int2 viewportOrigin = int2(Params.Viewport.zw);
    int2 viewportSize = max(int2(round(rcp(Params.Viewport.xy))), int2(1));
    float2 localUV = (float2(pixel) + 0.5 + Params.Jitter.xy
        - Params.Viewport.zw)
        * Params.Viewport.xy;
    float nearDeviceDepth = Params.Sampling.w > 0.5 ? 1.0 : 0.0;
    float farDeviceDepth = 1.0 - nearDeviceDepth;
    float3 nearWorld = ReconstructWorld(localUV, nearDeviceDepth);
    float3 farWorld = ReconstructWorld(localUV, farDeviceDepth);
    float3 origin = Params.CameraPosition.xyz;
    float3 direction = normalize(farWorld - nearWorld);
    float nearDistance;
    float farDistance;
    if (!IntersectLayer(origin, direction, nearDistance, farDistance))
        return float4(0.0, 0.0, 0.0, 1.0);

    int2 outputPixel = int2(clamp(
        floor(Params.OutputViewport.xy + localUV * Params.OutputViewport.zw),
        float2(0.0), Params.Target.zw - 1.0));
    float deviceDepth = SceneDepth.Load(int3(outputPixel, 0));
    bool sceneValid = Params.Sampling.w > 0.5
        ? deviceDepth > 0.001 : deviceDepth < 0.999;
    if (sceneValid)
    {
        float3 opaqueWorld = ReconstructWorld(localUV, deviceDepth);
        farDistance = min(farDistance, length(opaqueWorld - origin));
    }
    if (farDistance <= nearDistance)
        return float4(0.0, 0.0, 0.0, 1.0);

    int primaryCount = clamp((int)round(Params.Sampling.x), 1, 32);
    int lightCount = clamp((int)round(Params.Sampling.y), 0, 4);
    float stepDistance = (farDistance - nearDistance) / (float)primaryCount;
    float lightStep = (Params.Layer.y - Params.Layer.x)
        / (float)max(lightCount, 1);
    float3 toLight = normalize(Params.LightDirection.xyz);
    float3 radiance = 0.0;
    float transmittance = 1.0;
    for (int sampleIndex = 0; sampleIndex < primaryCount; ++sampleIndex)
    {
        float distance = nearDistance
            + ((float)sampleIndex + 0.5) * stepDistance;
        float3 position = origin + direction * distance;
        float density = SampleDensity(position);
        if (density <= 0.0)
            continue;
        float lightOpticalDepth = 0.0;
        for (int lightIndex = 0; lightIndex < lightCount; ++lightIndex)
        {
            float3 lightPosition = position + toLight
                * ((float)lightIndex + 0.5) * lightStep;
            lightOpticalDepth += SampleBase(lightPosition)
                * Params.Density.z * lightStep;
        }
        float lightTransmittance = exp(-lightOpticalDepth);
        float stepTransmittance = exp(
            -density * Params.Layer.w * stepDistance);
        float scattered = 1.0 - stepTransmittance;
        radiance += transmittance * scattered
            * (Params.Density.w + lightTransmittance)
            * Params.LightColor.xyz;
        transmittance *= stepTransmittance;
        if (transmittance <= Params.Sampling.z)
        {
            transmittance = 0.0;
            break;
        }
    }
    return float4(radiance, transmittance);
}

[shader("fragment")]
float4 CloudFragmentMain(VSOutput input) : SV_Target0
{
    return IntegrateCloud(int2(input.pos.xy));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CloudComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint targetWidth;
    uint targetHeight;
    CloudOutput.GetDimensions(targetWidth, targetHeight);
    if (dispatchThreadId.x >= targetWidth || dispatchThreadId.y >= targetHeight)
        return;
    int2 pixel = int2(dispatchThreadId.xy);
    int2 viewportOrigin = int2(Params.Viewport.zw);
    int2 viewportSize = max(int2(round(rcp(Params.Viewport.xy))), int2(1));
    bool insideViewport = all(pixel >= viewportOrigin)
        && all(pixel < viewportOrigin + viewportSize);
    CloudOutput[pixel] = insideViewport
        ? IntegrateCloud(pixel) : float4(0.0, 0.0, 0.0, 1.0);
}
