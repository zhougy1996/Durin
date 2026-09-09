module StaticMeshBasePass;

import VertexFactory.LocalVertexFactory;
#if DURIN_SPLINE_MESH
import VertexFactory.SplineMeshVertexFactory;
#endif
import Lighting.PBRLighting;
import Lighting.DirectionalShadow;
import Lighting.ForwardLightingUniform;
import Lighting.SurfaceLighting;
import Material.SpecularAntialiasing;
import Material.SurfaceMaterial;

// Shared geometry vertex stage and independent error material fragments.

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 worldTangent : TEXCOORD2;
    float2 uv0 : TEXCOORD3;
    float2 uv1 : TEXCOORD4;
    float2 uv2 : TEXCOORD5;
    float2 uv3 : TEXCOORD6;
};



struct TransformUniform
{
    float4x4 LocalToClip;
    float4x4 LocalToWorld;
    float4x4 NormalToWorld;
    // x contains the local-to-world determinant sign for mirrored transforms.
    float4 TransformParams;
};

[[vk::binding(0, 0)]] ConstantBuffer<TransformUniform> Transform;
#if DURIN_SPLINE_MESH
[[vk::binding(24, 0)]] ConstantBuffer<FSplineMeshUniform> SplineMesh;
#endif

[shader("vertex")]
VSOutput VertexMain(FLocalVertexFactoryInput input)
{
#if DURIN_SPLINE_MESH
    FLocalVertexFactoryIntermediates vertex =
        GetSplineMeshVertexFactoryIntermediates(input, SplineMesh);
#else
    FLocalVertexFactoryIntermediates vertex =
        GetLocalVertexFactoryIntermediates(input);
#endif
    VSOutput output;
    output.pos = mul(Transform.LocalToClip, float4(vertex.localPosition, 1.0));
    output.color = vertex.color;
    output.worldPosition = mul(Transform.LocalToWorld, float4(vertex.localPosition, 1.0)).xyz;
    output.worldNormal = mul(Transform.NormalToWorld, float4(vertex.localNormal, 0.0)).xyz;
    float3 worldTangent = mul(Transform.LocalToWorld, float4(vertex.localTangent.xyz, 0.0)).xyz;
    output.worldTangent = float4(worldTangent, vertex.localTangent.w * Transform.TransformParams.x);
    output.uv0 = vertex.texCoords[0];
    output.uv1 = vertex.texCoords[1];
    output.uv2 = vertex.texCoords[2];
    output.uv3 = vertex.texCoords[3];

    return output;
}

// Material-authored fragments are supplied by accepted compiled generations.
// The fixed fragment set is an independent, resource-free error terminal.
[shader("fragment")]
void OpaqueShadowFragmentMain(VSOutput input)
{
}

[shader("fragment")]
void ShadowFragmentMain(VSOutput input)
{
}

struct GeometryPassFragmentOutput
{
    float4 material : SV_Target0;
    float4 normals : SV_Target1;
    float4 surface : SV_Target2;
    float4 emissive : SV_Target3;
};

[shader("fragment")]
GeometryPassFragmentOutput GeometryFragmentMain(VSOutput input,
    bool isFrontFace : SV_IsFrontFace)
{
    GeometryPassFragmentOutput output;
    output.material = float4(1.0, 0.0, 1.0, 0.0);
    float2 normal = EncodeOctahedralNormal(normalize(input.worldNormal) * (isFrontFace ? 1.0 : -1.0));
    output.normals = float4(normal, normal);
    output.surface = float4(0.5, 1.0, 1.0, 1.0 / 255.0);
    output.emissive = float4(0.0);
    return output;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input,
    bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    return float4(1.0, 0.0, 1.0, 1.0);
}
