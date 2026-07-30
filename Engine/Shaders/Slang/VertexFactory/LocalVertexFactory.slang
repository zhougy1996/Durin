module LocalVertexFactory;

// Public contract consumed by mesh-pass entry-point modules. Vulkan vertex
// fetch expands the normalized integer tangent/color streams to float values
// before this module receives them.
public struct FLocalVertexFactoryInput
{
    public float3 position : POSITION;
    public float4 packedNormal : NORMAL;
    public float4 packedTangent : TANGENT;
    public float2 texCoord0 : TEXCOORD0;
    public float2 texCoord1 : TEXCOORD1;
    public float2 texCoord2 : TEXCOORD2;
    public float2 texCoord3 : TEXCOORD3;
    public float4 packedColor : COLOR;
};

public struct FLocalVertexFactoryIntermediates
{
    public float3 localPosition;
    public float3 localNormal;
    public float4 localTangent;
    public float2 texCoords[4];
    public float4 color;
};

internal float3 DecodeDirection(float4 packedDirection)
{
    return packedDirection.xyz;
}

internal float4 DecodeTangent(float4 packedTangent)
{
    return float4(DecodeDirection(packedTangent), packedTangent.w);
}

internal float4 DecodeColor(float4 packedColor)
{
    return packedColor;
}

public FLocalVertexFactoryIntermediates GetLocalVertexFactoryIntermediates(
    FLocalVertexFactoryInput input)
{
    FLocalVertexFactoryIntermediates result;
    result.localPosition = input.position;
    result.localNormal = DecodeDirection(input.packedNormal);
    result.localTangent = DecodeTangent(input.packedTangent);
    result.texCoords[0] = input.texCoord0;
    result.texCoords[1] = input.texCoord1;
    result.texCoords[2] = input.texCoord2;
    result.texCoords[3] = input.texCoord3;
    result.color = DecodeColor(input.packedColor);
    return result;
}
