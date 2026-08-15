module SurfaceLighting;

import Lighting.PBRLighting;

public struct FSurfaceLightingFrame
{
    public float3 normal;
    public float3 toView;
    public float NoV;
    public float3 reflectionDirection;
};

public FSurfaceLightingFrame BuildSurfaceLightingFrame(
    float3 worldPosition,
    float3 shadingNormal,
    float3 viewPosition)
{
    FSurfaceLightingFrame result;
    // SurfaceMaterial already publishes a finite normalized shading normal.
    // Preserve that exact value so extraction does not perturb forward output.
    result.normal = shadingNormal;
    float3 viewVector = viewPosition - worldPosition;
    float viewLengthSquared = dot(viewVector, viewVector);
    result.toView = viewLengthSquared > 1.0e-8
        ? viewVector * rsqrt(viewLengthSquared)
        : float3(0.0);
    result.NoV = saturate(dot(result.normal, result.toView));
    result.reflectionDirection = reflect(-result.toView, result.normal);
    return result;
}

public float3 EvaluateSurfaceDirectionalLighting(
    float3 baseColor,
    float metallic,
    float roughness,
    FSurfaceLightingFrame frame,
    float3 toLight,
    float3 radiance,
    float shadowAttenuation)
{
    return EvaluatePBRDirectLighting(
        baseColor, metallic, roughness, frame.normal,
        toLight, frame.toView, radiance) * shadowAttenuation;
}

public float3 EvaluateSurfaceEnvironmentLighting(
    float3 baseColor,
    float metallic,
    float roughness,
    float ambientOcclusion,
    FSurfaceLightingFrame frame,
    TextureCube<float4> irradianceTexture,
    TextureCube<float4> prefilteredTexture,
    Texture2D<float4> brdfLutTexture,
    SamplerState environmentSampler)
{
    float3 irradiance = irradianceTexture.Sample(
        environmentSampler, frame.normal).rgb;
    float3 prefiltered = prefilteredTexture.SampleLevel(
        environmentSampler, frame.reflectionDirection,
        roughness * 6.0).rgb;
    float2 environmentBrdf = brdfLutTexture.Sample(
        environmentSampler, float2(frame.NoV, roughness)).rg;
    return EvaluatePBREnvironmentLighting(
        baseColor, metallic, roughness, ambientOcclusion,
        frame.NoV, irradiance, prefiltered, environmentBrdf);
}

public float4 ComposeSurfaceLighting(
    float3 direct,
    float3 environment,
    float3 emissive,
    float effectiveOpacity)
{
    return float4(
        direct + environment + emissive,
        effectiveOpacity);
}
