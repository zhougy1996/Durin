module PBRLighting;

internal static const float Pi = 3.14159265358979323846;
internal static const float MinVectorLengthSquared = 1.0e-8;
internal static const float MinBRDFDivisor = 1.0e-5;
internal static const float MinPerceptualRoughness = 0.045;

internal float3 SafeNormalize(float3 value)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > MinVectorLengthSquared
        ? value * rsqrt(lengthSquared)
        : float3(0.0);
}

internal float3 ComputeF0(float3 baseColor, float metallic, float specular)
{
    return lerp(float3(0.08 * saturate(specular)), baseColor, metallic);
}

public float3 EvaluatePBRDirectLighting(
    float3 baseColor,
    float metallic,
    float specular,
    float roughness,
    float3 normal,
    float3 toLight,
    float3 toView,
    float3 lightRadiance, float clearCoat = 0.0, float clearCoatRoughness = 0.1)
{
    float3 halfVector = SafeNormalize(toLight + toView);
    float NoL = saturate(dot(normal, toLight));
    float NoV = saturate(dot(normal, toView));
    float NoH = saturate(dot(normal, halfVector));
    float VoH = saturate(dot(toView, halfVector));
    float evaluatedRoughness = clamp(
        roughness, MinPerceptualRoughness, 1.0);
    float alpha = evaluatedRoughness * evaluatedRoughness;
    float alpha2 = alpha * alpha;
    float NoH2 = NoH * NoH;
    float distributionTerm = (1.0 - NoH2) + NoH2 * alpha2;
    float distribution = alpha2
        / (Pi * distributionTerm * distributionTerm);
    float visibility = 0.5 / max(
        NoL * sqrt(NoV * NoV * (1.0 - alpha2) + alpha2)
            + NoV * sqrt(NoL * NoL * (1.0 - alpha2) + alpha2),
        MinBRDFDivisor);
    float3 f0 = ComputeF0(baseColor, metallic, specular);
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic)
        * baseColor / Pi;
    float3 base = (diffuse + distribution * visibility * fresnel) * lightRadiance * NoL;
    float c = saturate(clearCoat);
    if (c == 0.0) return base;
    float rc = clamp(clearCoatRoughness, 0.045, 1.0);
    float ac2 = rc * rc * rc * rc;
    float term = (1.0 - NoH2) + NoH2 * ac2;
    float Dc = ac2 / (Pi * term * term);
    float Vc = 0.5 / max(NoL * sqrt(NoV * NoV * (1.0 - ac2) + ac2)
        + NoV * sqrt(NoL * NoL * (1.0 - ac2) + ac2), MinBRDFDivisor);
    float Fv = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
    float Fl = 0.04 + 0.96 * pow(1.0 - NoL, 5.0);
    float Fh = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
    return (1.0 - c * Fv) * (1.0 - c * Fl) * base
        + c * Dc * Vc * Fh * lightRadiance * NoL;
}

public float3 EvaluatePBREnvironmentLighting(
    float3 baseColor,
    float metallic,
    float specular,
    float roughness,
    float ambientOcclusion,
    float NoV,
    float3 irradiance,
    float3 prefilteredRadiance,
    float2 environmentBrdf)
{
    float3 f0 = ComputeF0(baseColor, metallic, specular);
    float3 fresnel = f0
        + (max(float3(1.0 - roughness), f0) - f0)
            * pow(1.0 - NoV, 5.0);
    float3 diffuse = irradiance * baseColor / Pi
        * (1.0 - fresnel) * (1.0 - metallic);
    float3 specular = prefilteredRadiance
        * (f0 * environmentBrdf.x + environmentBrdf.y);
    return (diffuse + specular) * ambientOcclusion;
}
