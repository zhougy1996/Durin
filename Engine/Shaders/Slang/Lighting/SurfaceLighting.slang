module SurfaceLighting;

import Lighting.PBRLighting;
import Lighting.ViewLightingUniform;

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
    float specular,
    float roughness,
    FSurfaceLightingFrame frame,
    float3 toLight,
    float3 radiance,
    float shadowAttenuation, float clearCoat = 0.0, float clearCoatRoughness = 0.1)
{
    return EvaluatePBRDirectLighting(
        baseColor, metallic, specular, roughness, frame.normal,
        toLight, frame.toView, radiance, clearCoat, clearCoatRoughness) * shadowAttenuation;
}

public float3 EvaluateSurfaceLocalLighting(
    float3 baseColor,
    float metallic,
    float specular,
    float roughness,
    float3 worldPosition,
    FSurfaceLightingFrame frame,
    FForwardLocalLightUniform light, float clearCoat = 0.0, float clearCoatRoughness = 0.1)
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
        baseColor, metallic, specular, roughness, frame.normal,
        toLight, frame.toView, radiance, clearCoat, clearCoatRoughness);
}

public float3 AccumulateViewLocalLights(float3 result, float3 baseColor, float metallic, float specular, float roughness,
    float3 worldPosition, FSurfaceLightingFrame frame, StructuredBuffer<FForwardLocalLightUniform> records, uint count, float clearCoat = 0.0, float clearCoatRoughness = 0.1)
{
    for (uint i = 0; i < count; ++i)
        result += EvaluateSurfaceLocalLighting(baseColor, metallic, specular, roughness, worldPosition, frame, records[i], clearCoat, clearCoatRoughness);
    return result;
}

public float3 EvaluateSurfaceEnvironmentLighting(
    float3 baseColor,
    float metallic,
    float specular,
    float roughness,
    float ambientOcclusion,
    FSurfaceLightingFrame frame,
    TextureCube<float4> irradianceTexture,
    TextureCube<float4> prefilteredTexture,
    Texture2D<float4> brdfLutTexture,
    SamplerState environmentSampler,
    float4 environmentRotation, float4 environmentControl, float clearCoat = 0.0, float clearCoatRoughness = 0.1)
{
    float3 normal = frame.normal + 2.0 * cross(environmentRotation.xyz,
        cross(environmentRotation.xyz, frame.normal) + environmentRotation.w * frame.normal);
    float3 reflection = frame.reflectionDirection + 2.0 * cross(environmentRotation.xyz,
        cross(environmentRotation.xyz, frame.reflectionDirection) + environmentRotation.w * frame.reflectionDirection);
    float3 irradiance = irradianceTexture.Sample(
        environmentSampler, normal).rgb * environmentControl.x;
    float3 prefiltered = prefilteredTexture.SampleLevel(
        environmentSampler, reflection,
        roughness * environmentControl.y).rgb * environmentControl.x;
    float2 environmentBrdf = brdfLutTexture.Sample(
        environmentSampler, float2(frame.NoV, roughness)).rg;
    float3 base = EvaluatePBREnvironmentLighting(
        baseColor, metallic, specular, roughness, ambientOcclusion,
        frame.NoV, irradiance, prefiltered, environmentBrdf);
    float c = saturate(clearCoat);
    if (c == 0.0) return base;
    float rc = clamp(clearCoatRoughness, 0.045, 1.0);
    float3 coatPrefiltered = prefilteredTexture.SampleLevel(environmentSampler, reflection,
        rc * environmentControl.y).rgb * environmentControl.x;
    float2 coatBrdf = brdfLutTexture.Sample(environmentSampler, float2(frame.NoV, rc)).rg;
    float Fc = 0.04 + 0.96 * pow(1.0 - frame.NoV, 5.0);
    float attenuation = 1.0 - c * Fc;
    return attenuation * attenuation * base
        + c * coatPrefiltered * (0.04 * coatBrdf.x + coatBrdf.y) * ambientOcclusion;
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
