module SkeletalMeshVertexFactory;

public struct FSkeletalMeshVertexFactoryInput
{
    public float3 position : POSITION;
    public float4 packedNormal : NORMAL;
    public float4 packedTangent : TANGENT;
    public float2 texCoord0 : TEXCOORD0;
    public float2 texCoord1 : TEXCOORD1;
    public float2 texCoord2 : TEXCOORD2;
    public float2 texCoord3 : TEXCOORD3;
    public float4 packedColor : COLOR;
    public uint4 jointIndices : BLENDINDICES;
    public float4 jointWeights : BLENDWEIGHT;
};

public struct FSkeletalMeshVertexFactoryIntermediates
{
    public float3 localPosition;
    public float3 localNormal;
    public float4 localTangent;
    public float2 texCoords[4];
    public float4 color;
};

internal float3 NormalizeSkinnedDirection(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8
        ? value * rsqrt(lengthSquared)
        : fallback;
}

public FSkeletalMeshVertexFactoryIntermediates
GetSkeletalMeshVertexFactoryIntermediates(
    FSkeletalMeshVertexFactoryInput input,
    StructuredBuffer<float4x4> palette)
{
    float4 bindPosition = float4(input.position, 1.0);
    float4 bindNormal = float4(input.packedNormal.xyz, 0.0);
    float4 bindTangent = float4(input.packedTangent.xyz, 0.0);
    float4 skinnedPosition = float4(0.0);
    float3 skinnedNormal = float3(0.0);
    float3 skinnedTangent = float3(0.0);
    [unroll]
    for (uint influence = 0; influence < 4; ++influence)
    {
        float weight = input.jointWeights[influence];
        float4x4 skinMatrix = palette[input.jointIndices[influence]];
        skinnedPosition += mul(skinMatrix, bindPosition) * weight;
        skinnedNormal += mul(skinMatrix, bindNormal).xyz * weight;
        skinnedTangent += mul(skinMatrix, bindTangent).xyz * weight;
    }

    FSkeletalMeshVertexFactoryIntermediates result;
    result.localPosition = skinnedPosition.xyz;
    result.localNormal = NormalizeSkinnedDirection(
        skinnedNormal, float3(0.0, 0.0, 1.0));
    result.localTangent = float4(NormalizeSkinnedDirection(
        skinnedTangent, float3(1.0, 0.0, 0.0)), input.packedTangent.w);
    result.texCoords[0] = input.texCoord0;
    result.texCoords[1] = input.texCoord1;
    result.texCoords[2] = input.texCoord2;
    result.texCoords[3] = input.texCoord3;
    result.color = input.packedColor;
    return result;
}
