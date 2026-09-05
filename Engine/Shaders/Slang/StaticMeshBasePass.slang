module StaticMeshBasePass;

import VertexFactory.LocalVertexFactory;
#if DURIN_SPLINE_MESH
import VertexFactory.SplineMeshVertexFactory;
#endif
#if DURIN_SKELETAL_MESH
import VertexFactory.SkeletalMeshVertexFactory;
#endif
import Lighting.PBRLighting;
import Lighting.DirectionalShadow;
import Lighting.ForwardLightingUniform;
import Lighting.SurfaceLighting;
import Material.SpecularAntialiasing;
import Material.SurfaceMaterial;

// Static mesh material foundation shader.

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

struct OpaqueShadowVSOutput
{
    float4 pos : SV_Position;
};

struct TransformUniform
{
    float4x4 LocalToClip;
    float4x4 LocalToWorld;
    float4x4 NormalToWorld;
    // x contains the local-to-world determinant sign for mirrored transforms.
    float4 TransformParams;
};

struct MaterialUniform
{
    float4 BaseColor;
    // xyz = emissive, w = metallic.
    float4 EmissiveMetallic;
    // xyz = constant tangent-space normal, w = perceptual roughness.
    float4 NormalRoughness;
    // x = ambient occlusion, y = opacity mask, z = lit mode.
    float4 SurfaceParams;
    // xy = scale, zw = offset, in texture-role order.
    float4 UVTransforms[8];
    // Texture-role UV channels, packed four per vector.
    float4 UVChannels0;
    float4 UVChannels1;
    // Texture-role rotation in radians, packed four per vector.
    float4 UVRotations0;
    float4 UVRotations1;
};

[[vk::binding(0, 0)]] ConstantBuffer<TransformUniform> Transform;
[[vk::binding(1, 0)]] ConstantBuffer<FForwardLightingUniform> Lighting;
[[vk::binding(2, 0)]] ConstantBuffer<MaterialUniform> Material;
[[vk::binding(3, 0)]] Texture2D<float4> BaseColorTexture;
[[vk::binding(4, 0)]] Texture2D<float4> NormalTexture;
[[vk::binding(5, 0)]] Texture2D<float4> MetallicTexture;
[[vk::binding(6, 0)]] Texture2D<float4> RoughnessTexture;
[[vk::binding(7, 0)]] Texture2D<float4> AmbientOcclusionTexture;
[[vk::binding(8, 0)]] Texture2D<float4> EmissiveTexture;
[[vk::binding(9, 0)]] Texture2D<float4> OpacityTexture;
[[vk::binding(10, 0)]] Texture2D<float4> OpacityMaskTexture;
[[vk::binding(11, 0)]] SamplerState BaseColorSampler;
[[vk::binding(12, 0)]] SamplerState NormalSampler;
[[vk::binding(13, 0)]] SamplerState MetallicSampler;
[[vk::binding(14, 0)]] SamplerState RoughnessSampler;
[[vk::binding(15, 0)]] SamplerState AmbientOcclusionSampler;
[[vk::binding(16, 0)]] SamplerState EmissiveSampler;
[[vk::binding(17, 0)]] SamplerState OpacitySampler;
[[vk::binding(18, 0)]] SamplerState OpacityMaskSampler;
[[vk::binding(19, 0)]] TextureCube<float4> EnvironmentIrradiance;
[[vk::binding(20, 0)]] TextureCube<float4> EnvironmentPrefiltered;
[[vk::binding(21, 0)]] Texture2D<float4> EnvironmentBrdfLut;
[[vk::binding(22, 0)]] SamplerState EnvironmentSampler;
#if DURIN_SKELETAL_MESH
[[vk::binding(23, 0)]] StructuredBuffer<float4x4> SkinPalette;
#endif
#if DURIN_SPLINE_MESH
[[vk::binding(24, 0)]] ConstantBuffer<FSplineMeshUniform> SplineMesh;
#endif
[[vk::binding(25, 0)]] Texture2DArray<float> DirectionalShadowTexture;
[[vk::binding(26, 0)]] SamplerComparisonState DirectionalShadowSampler;

[shader("vertex")]
#if DURIN_SKELETAL_MESH
#if DURIN_OPAQUE_SHADOW_DEPTH
OpaqueShadowVSOutput VertexMain(FSkeletalMeshVertexFactoryInput input)
#else
VSOutput VertexMain(FSkeletalMeshVertexFactoryInput input)
#endif
#else
#if DURIN_OPAQUE_SHADOW_DEPTH
OpaqueShadowVSOutput VertexMain(FLocalVertexFactoryInput input)
#else
VSOutput VertexMain(FLocalVertexFactoryInput input)
#endif
#endif
{
#if DURIN_SKELETAL_MESH
    FSkeletalMeshVertexFactoryIntermediates vertex =
        GetSkeletalMeshVertexFactoryIntermediates(input, SkinPalette);
#elif DURIN_SPLINE_MESH
    FLocalVertexFactoryIntermediates vertex =
        GetSplineMeshVertexFactoryIntermediates(input, SplineMesh);
#else
    FLocalVertexFactoryIntermediates vertex =
        GetLocalVertexFactoryIntermediates(input);
#endif
#if DURIN_OPAQUE_SHADOW_DEPTH
    OpaqueShadowVSOutput output;
    output.pos = mul(Transform.LocalToClip, float4(vertex.localPosition, 1.0));
#else
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
#endif
    return output;
}

float2 SelectUV(VSOutput input, float channel)
{
    uint index = min((uint)(channel + 0.5), 3u);
    if (index == 1u) return input.uv1;
    if (index == 2u) return input.uv2;
    if (index == 3u) return input.uv3;
    return input.uv0;
}

float2 GetMaterialUV(VSOutput input, uint role)
{
    float channel = role < 4u
        ? Material.UVChannels0[role]
        : Material.UVChannels1[role - 4u];
    float4 transform = Material.UVTransforms[role];
    float rotation = role < 4u
        ? Material.UVRotations0[role]
        : Material.UVRotations1[role - 4u];
    float2 scaled = SelectUV(input, channel) * transform.xy;
    if (rotation == 0.0)
        return scaled + transform.zw;
    float sine;
    float cosine;
    sincos(rotation, sine, cosine);
    return float2(
        cosine * scaled.x - sine * scaled.y,
        sine * scaled.x + cosine * scaled.y) + transform.zw;
}

FMaterialSurface EvaluateInputMaterialSurface(VSOutput input)
{
    FMaterialSampleSet samples;
    samples.baseColor = BaseColorTexture.Sample(
        BaseColorSampler, GetMaterialUV(input, 0u));
    samples.normal = DecodeTextureNormal(NormalTexture.Sample(
        NormalSampler, GetMaterialUV(input, 1u)).rg);
    samples.metallic = MetallicTexture.Sample(
        MetallicSampler, GetMaterialUV(input, 2u)).r;
    samples.roughness = RoughnessTexture.Sample(
        RoughnessSampler, GetMaterialUV(input, 3u)).r;
    samples.ambientOcclusion = AmbientOcclusionTexture.Sample(
        AmbientOcclusionSampler, GetMaterialUV(input, 4u)).r;
    samples.emissive = EmissiveTexture.Sample(
        EmissiveSampler, GetMaterialUV(input, 5u)).rgb;
    samples.opacity = OpacityTexture.Sample(
        OpacitySampler, GetMaterialUV(input, 6u)).r;
    samples.opacityMask = OpacityMaskTexture.Sample(
        OpacityMaskSampler, GetMaterialUV(input, 7u)).r;
    return EvaluateMaterialSurface(
        Material.BaseColor,
        Material.EmissiveMetallic,
        Material.NormalRoughness,
        Material.SurfaceParams,
        input.color,
        samples);
}

struct FResolvedSurfaceShading
{
    FMaterialNormalFrame normalFrame;
    float effectiveRoughness;
};

FResolvedSurfaceShading ResolveSurfaceShading(
    VSOutput input,
    FMaterialSurface surface,
    bool isFrontFace)
{
    FResolvedSurfaceShading result;
    result.normalFrame = EvaluateMaterialNormalFrame(
        input.worldNormal,
        input.worldTangent,
        surface.tangentNormal,
        isFrontFace);
    result.effectiveRoughness = FilterSpecularRoughness(
        surface.roughness,
        result.normalFrame.shadingNormal,
        Material.SurfaceParams.w > 0.5);
    return result;
}

[shader("fragment")]
void OpaqueShadowFragmentMain(VSOutput input)
{
}

[shader("fragment")]
void ShadowFragmentMain(VSOutput input)
{
#if DURIN_MATERIAL_BLEND_MODE == 1
    float opacityMaskSample = OpacityMaskTexture.Sample(
        OpacityMaskSampler, GetMaterialUV(input, 7u)).r;
    float opacityMask = saturate(Material.SurfaceParams.y * opacityMaskSample);
    const float opacityMaskThreshold = saturate(asfloat(uint(
        DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS)));
    if (RejectMaterialMask(opacityMask, opacityMaskThreshold))
        discard;
#endif
}

struct GeometryPassFragmentOutput
{
    float4 material : SV_Target0;
    float4 normals : SV_Target1;
    float4 surface : SV_Target2;
    float4 emissive : SV_Target3;
};

[shader("fragment")]
GeometryPassFragmentOutput GeometryFragmentMain(
    VSOutput input,
    bool isFrontFace : SV_IsFrontFace)
{
    FMaterialSurface surface = EvaluateInputMaterialSurface(input);
    FResolvedSurfaceShading shading = ResolveSurfaceShading(
        input, surface, isFrontFace);
#if DURIN_MATERIAL_BLEND_MODE == 1
    const float opacityMaskThreshold = saturate(asfloat(uint(
        DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS)));
    if (RejectMaterialMask(surface.opacityMask, opacityMaskThreshold))
        discard;
#endif
    if (Material.SurfaceParams.z < 0.5)
        discard;
    GeometryPassFragmentOutput output;
    output.material = float4(surface.baseColor, surface.metallic);
    output.normals = float4(
        EncodeOctahedralNormal(shading.normalFrame.shadingNormal),
        EncodeOctahedralNormal(shading.normalFrame.geometricNormal));
    output.surface = float4(
        shading.effectiveRoughness,
        surface.ambientOcclusion,
        surface.opacity,
        1.0 / 255.0);
    output.emissive = float4(surface.emissive, 0.0);
    return output;
}

[shader("fragment")]
float4 FragmentMain(
    VSOutput input,
    bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    FMaterialSurface surface = EvaluateInputMaterialSurface(input);
    FResolvedSurfaceShading shading;
    if (Material.SurfaceParams.z >= 0.5)
    {
        shading = ResolveSurfaceShading(input, surface, isFrontFace);
    }
#if DURIN_MATERIAL_BLEND_MODE == 1
    const float opacityMaskThreshold = saturate(asfloat(uint(
        DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS)));
    if (RejectMaterialMask(surface.opacityMask, opacityMaskThreshold))
        discard;
#endif
    if (Material.SurfaceParams.z < 0.5)
	{
		return float4(
			surface.baseColor + surface.emissive, surface.opacity);
	}

    FSurfaceLightingFrame lightingFrame = BuildSurfaceLightingFrame(
        input.worldPosition,
        shading.normalFrame.shadingNormal,
        Lighting.ViewPosition.xyz);
    float3 direct = float3(0.0);
    if (Lighting.Counts.x > 0u)
    {
        float3 toLight = SafeSurfaceNormal(
            -Lighting.DirectionalDirection.xyz);
        float3 radiance = Lighting.DirectionalColorIntensity.rgb
            * Lighting.DirectionalColorIntensity.a;
        FDirectionalShadowSample shadow = EvaluateDirectionalShadow(
            input.worldPosition,
            shading.normalFrame.geometricNormal,
            Lighting.DirectionalShadowControl,
            Lighting.DirectionalShadowViewDepthTransform,
            Lighting.DirectionalShadowSplitDepths,
            Lighting.DirectionalShadowLightTransition,
            Lighting.DirectionalShadowCascades,
            DirectionalShadowTexture,
            DirectionalShadowSampler);
        if (Lighting.DirectionalShadowControl.y > 0.5)
        {
            return float4(
                shadow.diagnosticColor, surface.opacity);
        }
        direct += EvaluateSurfaceDirectionalLighting(
            surface.baseColor, surface.metallic, shading.effectiveRoughness,
            lightingFrame, toLight, radiance, shadow.attenuation);
    }
    [unroll]
    for (uint lightIndex = 0u; lightIndex < 4u; ++lightIndex)
    {
        if (lightIndex >= Lighting.Counts.y)
            break;
        direct += EvaluateSurfaceLocalLighting(
            surface.baseColor, surface.metallic, shading.effectiveRoughness,
            input.worldPosition, lightingFrame, Lighting.Local[lightIndex]);
    }
    float3 environment = EvaluateSurfaceEnvironmentLighting(
        surface.baseColor,
        surface.metallic,
        shading.effectiveRoughness,
        surface.ambientOcclusion,
        lightingFrame,
        EnvironmentIrradiance,
        EnvironmentPrefiltered,
        EnvironmentBrdfLut,
        EnvironmentSampler);
	return ComposeSurfaceLighting(
		direct,
		environment,
		surface.emissive,
		surface.opacity);
}
