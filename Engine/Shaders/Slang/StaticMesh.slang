module StaticMesh;

import VertexFactory.LocalVertexFactory;

// Static mesh material foundation shader.

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 worldTangent : TEXCOORD2;
    float2 uv0 : TEXCOORD3;
};

struct TransformUniform
{
    float4x4 LocalToClip;
    float4x4 LocalToWorld;
    float4x4 NormalToWorld;
    // x contains the local-to-world determinant sign for mirrored transforms.
    float4 TransformParams;
};

struct LightingUniform
{
    // LightDirection.w carries the optional editor-preview rim-light intensity.
    float4 LightDirection;
    float4 LightColorIntensity;
    float4 ViewPositionAmbient;
};

struct MaterialUniform
{
    float4 BaseColor;
    // x = specular strength, y = shininess, z = lit mode.
    float4 Params;
};

[[vk::binding(0, 0)]] ConstantBuffer<TransformUniform> Transform;
[[vk::binding(1, 0)]] ConstantBuffer<LightingUniform> Lighting;
[[vk::binding(2, 0)]] ConstantBuffer<MaterialUniform> Material;
[[vk::binding(3, 0)]] Texture2D<float4> BaseColorTexture;
[[vk::binding(4, 0)]] SamplerState BaseColorSampler;

[shader("vertex")]
VSOutput VertexMain(FLocalVertexFactoryInput input)
{
    FLocalVertexFactoryIntermediates vertex =
        GetLocalVertexFactoryIntermediates(input);
    VSOutput output;
    output.pos = mul(Transform.LocalToClip, float4(vertex.localPosition, 1.0));
    output.color = vertex.color;
    output.worldPosition = mul(Transform.LocalToWorld, float4(vertex.localPosition, 1.0)).xyz;
    output.worldNormal = normalize(mul(Transform.NormalToWorld, float4(vertex.localNormal, 0.0)).xyz);
    float3 worldTangent = mul(Transform.LocalToWorld, float4(vertex.localTangent.xyz, 0.0)).xyz;
    worldTangent = normalize(worldTangent - output.worldNormal * dot(output.worldNormal, worldTangent));
    output.worldTangent = float4(worldTangent, vertex.localTangent.w * Transform.TransformParams.x);
    output.uv0 = vertex.texCoords[0];
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target
{
    float4 surfaceColor = Material.BaseColor * BaseColorTexture.Sample(BaseColorSampler, input.uv0) * input.color;
    if (Material.Params.z < 0.5)
        return surfaceColor;

    float3 normal = normalize(input.worldNormal);
    float3 toLight = normalize(-Lighting.LightDirection.xyz);
    float3 toView = normalize(Lighting.ViewPositionAmbient.xyz - input.worldPosition);
    float diffuse = max(dot(normal, toLight), 0.0);
    float3 halfVector = normalize(toLight + toView);
    float specular = diffuse > 0.0 ? pow(max(dot(normal, halfVector), 0.0), Material.Params.y) * Material.Params.x : 0.0;
    float3 lightColor = Lighting.LightColorIntensity.rgb * Lighting.LightColorIntensity.a;
    float rim = pow(1.0 - max(dot(normal, toView), 0.0), 3.0) * Lighting.LightDirection.w;
    float3 litColor = surfaceColor.rgb * (Lighting.ViewPositionAmbient.a + diffuse * lightColor)
        + (specular + rim) * Lighting.LightColorIntensity.rgb;
    return float4(litColor, surfaceColor.a);
}
