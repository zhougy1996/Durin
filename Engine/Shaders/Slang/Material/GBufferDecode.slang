module GBufferDecode;

public static const uint GBufferStandardLitFlag = 1u;

public struct FGBufferRecord
{
    public float3 baseColor;
    public float3 shadingNormal;
    public float3 geometricNormal;
    public float3 emissive;
    public float metallic;
    public float roughness;
    public float ambientOcclusion;
    public float effectiveOpacity;
    public uint flags;
};

public float3 DecodeOctahedralNormal(float2 encoded)
{
    float2 folded = encoded * 2.0 - 1.0;
    float3 normal = float3(
        folded, 1.0 - abs(folded.x) - abs(folded.y));
    float correction = saturate(-normal.z);
    normal.xy += float2(
        normal.x >= 0.0 ? -correction : correction,
        normal.y >= 0.0 ? -correction : correction);
    float lengthSquared = dot(normal, normal);
    return lengthSquared > 1.0e-8
        ? normal * rsqrt(lengthSquared) : float3(0.0, 0.0, 1.0);
}

public FGBufferRecord DecodeGBuffer(
    float4 material,
    float4 normals,
    float4 surface,
    float3 emissive)
{
    FGBufferRecord result;
    result.baseColor = material.rgb;
    result.shadingNormal = DecodeOctahedralNormal(normals.rg);
    result.geometricNormal = DecodeOctahedralNormal(normals.ba);
    result.emissive = emissive;
    result.metallic = material.a;
    result.roughness = surface.r;
    result.ambientOcclusion = surface.g;
    result.effectiveOpacity = surface.b;
    result.flags = (uint)round(saturate(surface.a) * 255.0);
    return result;
}

public bool ReconstructViewPositionAnalytic(
    float4 projectionRow0,
    float4 projectionRow1,
    float4 projectionRow2,
    float4 projectionRow3,
    float2 ndc,
    float deviceDepth,
    out float3 viewPosition)
{
    float3 device = float3(ndc, deviceDepth);
    float3 row0 = projectionRow0.xyz - device.x * projectionRow3.xyz;
    float3 row1 = projectionRow1.xyz - device.y * projectionRow3.xyz;
    float3 row2 = projectionRow2.xyz - device.z * projectionRow3.xyz;
    float3 constants = -(float3(
        projectionRow0.w, projectionRow1.w, projectionRow2.w)
        - device * projectionRow3.w);
    float denominator = dot(row0, cross(row1, row2));
    if (abs(denominator) <= 1.0e-30)
    {
        viewPosition = 0.0;
        return false;
    }
    viewPosition = float3(
        dot(float3(constants.x, row0.yz),
            cross(float3(constants.y, row1.yz),
                float3(constants.z, row2.yz))),
        dot(float3(row0.x, constants.x, row0.z),
            cross(float3(row1.x, constants.y, row1.z),
                float3(row2.x, constants.z, row2.z))),
        dot(float3(row0.xy, constants.x),
            cross(float3(row1.xy, constants.y),
                float3(row2.xy, constants.z)))) / denominator;
    return all(isfinite(viewPosition));
}
