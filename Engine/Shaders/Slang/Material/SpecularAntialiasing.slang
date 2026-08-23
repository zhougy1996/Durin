module SpecularAntialiasing;

internal static const float MinPerceptualRoughness = 0.045;
internal static const float SpecularAAVarianceScale = 0.15;
internal static const float SpecularAAKernelThreshold = 0.20;

public float EstimateNormalVariance(float3 shadingNormal)
{
    float3 normalDx = ddx(shadingNormal);
    float3 normalDy = ddy(shadingNormal);
    return dot(normalDx, normalDx) + dot(normalDy, normalDy);
}

public float FilterSpecularRoughnessFromVariance(
    float authoredRoughness,
    float normalVariance)
{
    float roughness = isfinite(authoredRoughness)
        ? clamp(authoredRoughness, MinPerceptualRoughness, 1.0)
        : 0.5;
    if (!isfinite(normalVariance))
        return roughness;
    float kernelRoughnessSquared = min(
        2.0 * SpecularAAVarianceScale * max(normalVariance, 0.0),
        SpecularAAKernelThreshold);
    return clamp(
        sqrt(roughness * roughness + kernelRoughnessSquared),
        MinPerceptualRoughness,
        1.0);
}

public float FilterSpecularRoughness(
    float authoredRoughness,
    float3 shadingNormal,
    bool enabled)
{
    if (!enabled)
    {
        return FilterSpecularRoughnessFromVariance(
            authoredRoughness, 0.0);
    }
    return FilterSpecularRoughnessFromVariance(
        authoredRoughness,
        EstimateNormalVariance(shadingNormal));
}
