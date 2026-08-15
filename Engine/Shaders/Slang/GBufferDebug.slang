module GBufferDebug;

import Material.GBufferDecode;

struct VSInput
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
};

struct GBufferDebugUniform
{
    float4 ProjectionRow0;
    float4 ProjectionRow1;
    float4 ProjectionRow2;
    float4 ProjectionRow3;
    float4x4 InverseProjection;
    float4 Params; // x = mode, yz = inverse viewport, w = position scale
};

[[vk::binding(0, 0)]] Texture2D<float4> GBufferMaterial;
[[vk::binding(1, 0)]] Texture2D<float4> GBufferNormals;
[[vk::binding(2, 0)]] Texture2D<float4> GBufferSurface;
[[vk::binding(3, 0)]] Texture2D<float3> GBufferEmissive;
[[vk::binding(4, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(5, 0)]] ConstantBuffer<GBufferDebugUniform> Params;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 DebugFragmentMain(VSOutput input) : SV_Target
{
    int2 pixel = int2(input.pos.xy);
    float4 material = GBufferMaterial.Load(int3(pixel, 0));
    float4 normals = GBufferNormals.Load(int3(pixel, 0));
    float4 surface = GBufferSurface.Load(int3(pixel, 0));
    float3 emissive = GBufferEmissive.Load(int3(pixel, 0));
    FGBufferRecord record = DecodeGBuffer(
        material, normals, surface, emissive);
    if ((record.flags & GBufferStandardLitFlag) == 0u)
        return float4(0.0, 0.0, 0.0, 1.0);

    uint mode = (uint)round(Params.Params.x);
    if (mode == 1u)
        return float4(record.baseColor, record.metallic);
    if (mode == 2u)
        return float4(record.shadingNormal * 0.5 + 0.5, 1.0);
    if (mode == 3u)
        return float4(record.geometricNormal * 0.5 + 0.5, 1.0);
    if (mode == 4u)
        return float4(record.roughness, record.ambientOcclusion,
            record.effectiveOpacity, 1.0);
    if (mode == 5u)
        return float4(record.emissive, 1.0);
    if (mode == 6u)
        return float4(
            (record.flags & 1u) != 0u ? 1.0 : 0.0,
            (record.flags & 2u) != 0u ? 1.0 : 0.0,
            (record.flags & 4u) != 0u ? 1.0 : 0.0, 1.0);
    if (mode == 10u)
        return float4(record.baseColor + record.emissive,
            record.effectiveOpacity);

    float depth = SceneDepth.Load(int3(pixel, 0));
    float2 uv = (float2(pixel) + 0.5) * Params.Params.yz;
    float2 ndc = uv * 2.0 - 1.0;
    float3 analyticPosition;
    bool valid = ReconstructViewPositionAnalytic(
        Params.ProjectionRow0,
        Params.ProjectionRow1,
        Params.ProjectionRow2,
        Params.ProjectionRow3,
        ndc, depth, analyticPosition);
    if (!valid)
        return float4(1.0, 0.0, 1.0, 1.0);
    if (mode == 7u)
        return float4(depth, depth, depth, 1.0);
    if (mode == 8u)
        return float4(saturate(abs(analyticPosition) * Params.Params.w), 1.0);

    float4 referenceH = mul(
        Params.InverseProjection, float4(ndc, depth, 1.0));
    float3 referencePosition = referenceH.xyz / referenceH.w;
    float error = length(analyticPosition - referencePosition);
    float tolerance = max(0.002, 3.0e-5 * length(referencePosition));
    float normalizedError = error / max(tolerance, 1.0e-8);
    return float4(saturate(normalizedError),
        saturate(1.0 - normalizedError), 0.0, 1.0);
}
