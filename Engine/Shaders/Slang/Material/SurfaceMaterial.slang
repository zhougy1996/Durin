module SurfaceMaterial;

internal static const float MinVectorLengthSquared = 1.0e-8;

public struct FMaterialSampleSet
{
    public float4 baseColor;
    public float3 normal;
    public float metallic;
    public float roughness;
    public float ambientOcclusion;
    public float3 emissive;
    public float opacity;
    public float opacityMask;
};

public struct FMaterialSurface
{
    public float3 baseColor;
    public float3 tangentNormal;
    public float metallic;
    public float roughness;
    public float ambientOcclusion;
    public float3 emissive;
    public float opacity;
    public float opacityMask;
};

public struct FMaterialNormalFrame
{
    public float3 geometricNormal;
    public float3 shadingNormal;
};

public float3 SafeSurfaceNormal(float3 value)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > MinVectorLengthSquared
        ? value * rsqrt(lengthSquared)
        : float3(0.0, 0.0, 1.0);
}

public float3 DecodeTextureNormal(float2 encoded)
{
    float2 xy = encoded * 2.0 - 1.0;
    float xyLengthSquared = dot(xy, xy);
    if (xyLengthSquared > 1.0)
        xy *= rsqrt(xyLengthSquared);
    return float3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

public float3 BlendSurfaceNormalsRNM(float3 baseNormal, float3 detailNormal)
{
    float3 first = SafeSurfaceNormal(baseNormal)
        + float3(0.0, 0.0, 1.0);
    float3 second = SafeSurfaceNormal(detailNormal)
        * float3(-1.0, -1.0, 1.0);
    return SafeSurfaceNormal(
        first * dot(first, second) / max(first.z, 1.0e-5) - second);
}

public FMaterialSurface EvaluateMaterialSurface(
    float4 constantBaseColor,
    float4 constantEmissiveMetallic,
    float4 constantNormalRoughness,
    float4 constantSurfaceParams,
    float4 vertexColor,
    FMaterialSampleSet samples)
{
    FMaterialSurface result;
    result.baseColor = saturate(constantBaseColor.rgb)
        * samples.baseColor.rgb * vertexColor.rgb;
    result.opacity = saturate(constantBaseColor.a)
        * saturate(samples.opacity) * saturate(vertexColor.a);
    result.ambientOcclusion = saturate(constantSurfaceParams.x)
        * saturate(samples.ambientOcclusion);
    result.opacityMask = saturate(
        constantSurfaceParams.y * samples.opacityMask);
    // constantSurfaceParams.w is a zero-valued runtime carry that keeps the
    // fixed material descriptor ABI reflected across static shader variants.
    result.opacity = saturate(result.opacity + constantSurfaceParams.w
        * (result.ambientOcclusion + result.opacityMask));
    result.emissive = max(constantEmissiveMetallic.xyz, 0.0)
        + max(samples.emissive, 0.0);
    result.tangentNormal = BlendSurfaceNormalsRNM(
        constantNormalRoughness.xyz, samples.normal);
    result.metallic = saturate(constantEmissiveMetallic.w)
        * saturate(samples.metallic);
    result.roughness = clamp(saturate(constantNormalRoughness.w)
        * saturate(samples.roughness), 0.045, 1.0);
    return result;
}

public bool RejectMaterialMask(float opacityMask, float threshold)
{
    return opacityMask < saturate(threshold);
}

public FMaterialNormalFrame EvaluateMaterialNormalFrame(
    float3 worldNormal,
    float4 worldTangent,
    float3 tangentNormal,
    bool isFrontFace)
{
    FMaterialNormalFrame result;
    result.geometricNormal = SafeSurfaceNormal(worldNormal);
    if (!isFrontFace)
        result.geometricNormal = -result.geometricNormal;
    float3 tangent = worldTangent.xyz - result.geometricNormal
        * dot(result.geometricNormal, worldTangent.xyz);
    float tangentLengthSquared = dot(tangent, tangent);
    bool hasValidTangentBasis = tangentLengthSquared
            > MinVectorLengthSquared
        && abs(worldTangent.w) > 0.5;
    float3 tangentFallbackAxis = abs(result.geometricNormal.z) < 0.999
        ? float3(0.0, 0.0, 1.0)
        : float3(0.0, 1.0, 0.0);
    float3 tangentFallback = cross(
        tangentFallbackAxis, result.geometricNormal);
    tangentFallback *= rsqrt(max(
        dot(tangentFallback, tangentFallback), MinVectorLengthSquared));
    tangent = tangentLengthSquared > MinVectorLengthSquared
        ? tangent * rsqrt(tangentLengthSquared)
        : tangentFallback;
    float3 bitangent = cross(result.geometricNormal, tangent)
        * worldTangent.w;
    float3 mappedNormal = tangent * tangentNormal.x
        + bitangent * tangentNormal.y
        + result.geometricNormal * tangentNormal.z;
    float mappedNormalLengthSquared = dot(mappedNormal, mappedNormal);
    result.shadingNormal = hasValidTangentBasis
        && mappedNormalLengthSquared > MinVectorLengthSquared
        ? mappedNormal * rsqrt(mappedNormalLengthSquared)
        : result.geometricNormal;
    return result;
}

public float2 EncodeOctahedralNormal(float3 value)
{
    float3 normal = SafeSurfaceNormal(value);
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    float2 encoded = normal.xy;
    if (normal.z < 0.0)
    {
        encoded = (1.0 - abs(encoded.yx))
            * float2(encoded.x >= 0.0 ? 1.0 : -1.0,
                encoded.y >= 0.0 ? 1.0 : -1.0);
    }
    return encoded * 0.5 + 0.5;
}
