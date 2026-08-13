module DirectionalShadow;

public float SampleDirectionalShadow(
    float3 worldPosition,
    float4x4 worldToShadow,
    float4 shadowParams,
    Texture2D<float> shadowTexture,
    SamplerComparisonState shadowSampler)
{
    if (shadowParams.x < 0.5)
        return 1.0;
    float4 projected = mul(worldToShadow, float4(worldPosition, 1.0));
    if (!all(isfinite(projected)) || abs(projected.w) <= 1.0e-8)
        return 1.0;
    float3 coordinate = projected.xyz / projected.w;
    if (!all(isfinite(coordinate))
        || any(coordinate.xy < float2(0.0))
        || any(coordinate.xy > float2(1.0))
        || coordinate.z < 0.0 || coordinate.z > 1.0)
        return 1.0;
    return shadowTexture.SampleCmpLevelZero(
        shadowSampler, coordinate.xy, coordinate.z - shadowParams.y);
}
