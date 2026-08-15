module SurfaceLighting;

import Lighting.PBRLighting;
import Lighting.ForwardLightingUniform;

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

public float3 EvaluateSurfaceLocalLighting(
    float3 baseColor,
    float metallic,
    float roughness,
    float3 worldPosition,
    FSurfaceLightingFrame frame,
    FForwardLocalLightUniform light)
{
    float3 lightVector = light.PositionInverseRange.xyz - worldPosition;
    float distanceSquared = dot(lightVector, lightVector);
    float3 toLight = distanceSquared > 1.0e-8
        ? lightVector * rsqrt(distanceSquared) : float3(0.0);
    float normalizedDistance = sqrt(max(distanceSquared, 0.0))
        * light.PositionInverseRange.w;
    float rangeBase = 1.0 - saturate(
        normalizedDistance * normalizedDistance);
    float rangeAttenuation = rangeBase * rangeBase;
    float inverseSquare = 1.0 / max(distanceSquared, 0.0025);
    float coneAttenuation = 1.0;
    if (light.DirectionType.w > 0.5)
    {
        float angleCosine = dot(light.DirectionType.xyz, -toLight);
        if (light.SpotCone.w > 0.5)
            coneAttenuation = angleCosine > light.SpotCone.y ? 1.0 : 0.0;
        else
        {
            float coneValue = saturate(
                (angleCosine - light.SpotCone.y) * light.SpotCone.z);
            coneAttenuation = coneValue * coneValue
                * (3.0 - 2.0 * coneValue);
        }
    }
    float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a
        * inverseSquare * rangeAttenuation * coneAttenuation;
    return EvaluatePBRDirectLighting(
        baseColor, metallic, roughness, frame.normal,
        toLight, frame.toView, radiance);
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
