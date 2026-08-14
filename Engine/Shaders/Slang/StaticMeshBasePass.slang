module StaticMeshBasePass;

import VertexFactory.LocalVertexFactory;
#if DURIN_TERRAIN
import VertexFactory.TerrainVertexFactory;
#endif
#if DURIN_SPLINE_MESH
import VertexFactory.SplineMeshVertexFactory;
#endif
#if DURIN_SKELETAL_MESH
import VertexFactory.SkeletalMeshVertexFactory;
#endif
import Lighting.PBRLighting;
import Lighting.DirectionalShadow;

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
#if DURIN_TERRAIN
    float terrainTransition : TEXCOORD7;
#endif
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

struct LocalLightUniform
{
    float4 PositionInverseRange;
    float4 DirectionType;
    float4 ColorIntensity;
    float4 SpotCone;
};

struct LightingUniform
{
    float4 ViewPosition;
    uint4 Counts;
    float4 DirectionalDirection;
    float4 DirectionalColorIntensity;
    float4 DirectionalShadowControl;
    float4 DirectionalShadowViewDepthTransform;
    float4 DirectionalShadowSplitDepths;
    float4 DirectionalShadowLightTransition;
    FDirectionalShadowCascadeUniform DirectionalShadowCascades[3];
    LocalLightUniform Local[4];
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
[[vk::binding(1, 0)]] ConstantBuffer<LightingUniform> Lighting;
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
#if DURIN_TERRAIN
struct TerrainUniform
{
    uint4 SamplePatch; // width, height, reserved, reserved
    float4 SpacingHeight; // spacing x/y, signed height scale, height offset
    float4 DistanceTransition; // fade start, render distance, reserved, reserved
};
[[vk::binding(23, 0)]] Texture2D<uint> HeightTexture;
[[vk::binding(24, 0)]] ConstantBuffer<TerrainUniform> Terrain;
struct TerrainInstanceData
{
    uint2 SampleOrigin;
    float2 Padding;
    float4 AnchorClip;
    float4 AnchorRelativeWorld;
};
[[vk::binding(27, 0)]] StructuredBuffer<TerrainInstanceData> TerrainPatchOrigins;
#endif
#if DURIN_SPLINE_MESH
[[vk::binding(24, 0)]] ConstantBuffer<FSplineMeshUniform> SplineMesh;
#endif
[[vk::binding(25, 0)]] Texture2DArray<float> DirectionalShadowTexture;
[[vk::binding(26, 0)]] SamplerComparisonState DirectionalShadowSampler;

[shader("vertex")]
#if DURIN_TERRAIN
#if DURIN_OPAQUE_SHADOW_DEPTH
OpaqueShadowVSOutput VertexMain(FTerrainVertexFactoryInput input, uint instanceID : SV_InstanceID)
#else
VSOutput VertexMain(FTerrainVertexFactoryInput input, uint instanceID : SV_InstanceID)
#endif
#elif DURIN_SKELETAL_MESH
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
#if DURIN_TERRAIN
    TerrainInstanceData instance = TerrainPatchOrigins[instanceID];
    uint2 sampleCoordinate = instance.SampleOrigin + input.gridCoordinate;
    uint2 maximumCoordinate = Terrain.SamplePatch.xy - uint2(1u, 1u);
    uint2 leftCoordinate = uint2(sampleCoordinate.x > 0u ? sampleCoordinate.x - 1u : 0u, sampleCoordinate.y);
    uint2 rightCoordinate = uint2(min(sampleCoordinate.x + 1u, maximumCoordinate.x), sampleCoordinate.y);
    uint2 upCoordinate = uint2(sampleCoordinate.x, sampleCoordinate.y > 0u ? sampleCoordinate.y - 1u : 0u);
    uint2 downCoordinate = uint2(sampleCoordinate.x, min(sampleCoordinate.y + 1u, maximumCoordinate.y));
    float height = Terrain.SpacingHeight.w
        + ((float)HeightTexture.Load(int3(sampleCoordinate, 0)) / 65535.0) * Terrain.SpacingHeight.z;
    float leftHeight = ((float)HeightTexture.Load(int3(leftCoordinate, 0)) / 65535.0) * Terrain.SpacingHeight.z;
    float rightHeight = ((float)HeightTexture.Load(int3(rightCoordinate, 0)) / 65535.0) * Terrain.SpacingHeight.z;
    float upHeight = ((float)HeightTexture.Load(int3(upCoordinate, 0)) / 65535.0) * Terrain.SpacingHeight.z;
    float downHeight = ((float)HeightTexture.Load(int3(downCoordinate, 0)) / 65535.0) * Terrain.SpacingHeight.z;
    float spanX = max((float)(rightCoordinate.x - leftCoordinate.x) * Terrain.SpacingHeight.x, 1.0e-8);
    float spanY = max((float)(downCoordinate.y - upCoordinate.y) * Terrain.SpacingHeight.y, 1.0e-8);
    float3 localNormal = normalize(float3(-(rightHeight - leftHeight) / spanX, -(downHeight - upHeight) / spanY, 1.0));
    float3 patchLocalPosition = float3(
        (float)input.gridCoordinate.x * Terrain.SpacingHeight.x,
        (float)input.gridCoordinate.y * Terrain.SpacingHeight.y, height);
    float3 localPosition = float3(
        (float)sampleCoordinate.x * Terrain.SpacingHeight.x,
        (float)sampleCoordinate.y * Terrain.SpacingHeight.y, height);
#elif DURIN_SKELETAL_MESH
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
#if DURIN_TERRAIN
    output.pos = instance.AnchorClip
        + mul(Transform.LocalToClip, float4(patchLocalPosition, 0.0));
#else
    output.pos = mul(Transform.LocalToClip, float4(vertex.localPosition, 1.0));
#endif
#else
    VSOutput output;
#if DURIN_TERRAIN
    output.pos = instance.AnchorClip
        + mul(Transform.LocalToClip, float4(patchLocalPosition, 0.0));
    output.color = float4(1.0);
    output.worldPosition = mul(Transform.LocalToWorld, float4(localPosition, 1.0)).xyz;
    output.worldNormal = mul(Transform.NormalToWorld, float4(localNormal, 0.0)).xyz;
    float3 worldTangent = mul(Transform.LocalToWorld, float4(1.0, 0.0, 0.0, 0.0)).xyz;
    output.worldTangent = float4(worldTangent, Transform.TransformParams.x);
    float3 relativeWorldPosition = instance.AnchorRelativeWorld.xyz
        + mul(Transform.LocalToWorld, float4(patchLocalPosition, 0.0)).xyz;
    output.terrainTransition = saturate(
        (length(relativeWorldPosition.xy) - Terrain.DistanceTransition.x)
        / max(Terrain.DistanceTransition.y - Terrain.DistanceTransition.x, 1.0));
    float2 uv = float2(sampleCoordinate) / float2(maximumCoordinate);
    output.uv0 = uv;
    output.uv1 = uv;
    output.uv2 = uv;
    output.uv3 = uv;
#else
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

float3 SafeNormal(float3 value)
{
    const float minVectorLengthSquared = 1.0e-8;
    float lengthSquared = dot(value, value);
    return lengthSquared > minVectorLengthSquared
        ? value * rsqrt(lengthSquared)
        : float3(0.0, 0.0, 1.0);
}

float3 DecodeTextureNormal(float2 encoded)
{
    float2 xy = encoded * 2.0 - 1.0;
    float xyLengthSquared = dot(xy, xy);
    if (xyLengthSquared > 1.0)
        xy *= rsqrt(xyLengthSquared);
    return float3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

float3 BlendRNM(float3 baseNormal, float3 detailNormal)
{
    float3 first = SafeNormal(baseNormal) + float3(0.0, 0.0, 1.0);
    float3 second = SafeNormal(detailNormal) * float3(-1.0, -1.0, 1.0);
    return SafeNormal(first * dot(first, second) / max(first.z, 1.0e-5) - second);
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
    if (opacityMask < opacityMaskThreshold)
        discard;
#endif
}

struct BasePassFragmentOutput
{
    float4 color : SV_Target0;
    float4 directionalDirect : SV_Target1;
};

[shader("fragment")]
BasePassFragmentOutput FragmentMain(
    VSOutput input,
    bool isFrontFace : SV_IsFrontFace)
{
	BasePassFragmentOutput output;
	output.directionalDirect = float4(0.0);
#if DURIN_TERRAIN && DURIN_MATERIAL_BLEND_MODE != 2
    static const float coverageThresholds[16] =
    {
        0.03125, 0.53125, 0.15625, 0.65625,
        0.78125, 0.28125, 0.90625, 0.40625,
        0.21875, 0.71875, 0.09375, 0.59375,
        0.96875, 0.46875, 0.84375, 0.34375
    };
    uint2 coveragePixel = uint2(input.pos.xy) & uint2(3u, 3u);
    if (input.terrainTransition >= coverageThresholds[coveragePixel.y * 4u + coveragePixel.x])
        discard;
#endif
    const float minVectorLengthSquared = 1.0e-8;
    float4 baseColorSample = BaseColorTexture.Sample(BaseColorSampler, GetMaterialUV(input, 0u));
    float3 normalSample = DecodeTextureNormal(
        NormalTexture.Sample(NormalSampler, GetMaterialUV(input, 1u)).rg);
    float metallicSample = MetallicTexture.Sample(MetallicSampler, GetMaterialUV(input, 2u)).r;
    float roughnessSample = RoughnessTexture.Sample(RoughnessSampler, GetMaterialUV(input, 3u)).r;
    float ambientOcclusionSample = AmbientOcclusionTexture.Sample(AmbientOcclusionSampler, GetMaterialUV(input, 4u)).r;
    float3 emissiveSample = EmissiveTexture.Sample(EmissiveSampler, GetMaterialUV(input, 5u)).rgb;
    float opacitySample = OpacityTexture.Sample(OpacitySampler, GetMaterialUV(input, 6u)).r;
    float opacityMaskSample = OpacityMaskTexture.Sample(OpacityMaskSampler, GetMaterialUV(input, 7u)).r;
    float3 baseColor = saturate(Material.BaseColor.rgb) * baseColorSample.rgb * input.color.rgb;
    float opacity = saturate(Material.BaseColor.a) * saturate(opacitySample) * saturate(input.color.a);
    float ambientOcclusion = saturate(Material.SurfaceParams.x) * saturate(ambientOcclusionSample);
    float opacityMask = saturate(Material.SurfaceParams.y * opacityMaskSample);
#if DURIN_MATERIAL_BLEND_MODE == 1
    const float opacityMaskThreshold = saturate(asfloat(uint(
        DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS)));
    if (opacityMask < opacityMaskThreshold)
        discard;
#endif
    // SurfaceParams.w is a zero-valued runtime carry that keeps the fixed
    // material descriptor ABI reflected across every static shader variant.
    opacity = saturate(opacity + Material.SurfaceParams.w * (ambientOcclusion + opacityMask));
    float3 emissive = max(Material.EmissiveMetallic.xyz, 0.0) + max(emissiveSample, 0.0);
    if (Material.SurfaceParams.z < 0.5)
	{
		output.color = float4(baseColor + emissive, opacity);
		return output;
	}

    float normalLengthSquared = dot(input.worldNormal, input.worldNormal);
    float3 geometricNormal = normalLengthSquared > minVectorLengthSquared
        ? input.worldNormal * rsqrt(normalLengthSquared)
        : float3(0.0, 0.0, 1.0);
    // Back faces are only rasterized by two-sided material pipelines. Orient
    // their tangent frame toward the visible side before applying normal maps.
    if (!isFrontFace)
        geometricNormal = -geometricNormal;
    float3 tangent = input.worldTangent.xyz
        - geometricNormal * dot(geometricNormal, input.worldTangent.xyz);
    float tangentLengthSquared = dot(tangent, tangent);
    bool hasValidTangentBasis = tangentLengthSquared > minVectorLengthSquared
        && abs(input.worldTangent.w) > 0.5;
    float3 tangentFallbackAxis = abs(geometricNormal.z) < 0.999
        ? float3(0.0, 0.0, 1.0)
        : float3(0.0, 1.0, 0.0);
    float3 tangentFallback = cross(tangentFallbackAxis, geometricNormal);
    tangentFallback *= rsqrt(max(dot(tangentFallback, tangentFallback), minVectorLengthSquared));
    tangent = tangentLengthSquared > minVectorLengthSquared
        ? tangent * rsqrt(tangentLengthSquared)
        : tangentFallback;
    float3 bitangent = cross(geometricNormal, tangent) * input.worldTangent.w;
    float3 tangentNormal = BlendRNM(Material.NormalRoughness.xyz, normalSample);
    float3 mappedNormal = tangent * tangentNormal.x
        + bitangent * tangentNormal.y + geometricNormal * tangentNormal.z;
    float mappedNormalLengthSquared = dot(mappedNormal, mappedNormal);
    float3 normal = hasValidTangentBasis
        && mappedNormalLengthSquared > minVectorLengthSquared
        ? mappedNormal * rsqrt(mappedNormalLengthSquared)
        : geometricNormal;
    float3 viewVector = Lighting.ViewPosition.xyz - input.worldPosition;
    float viewLengthSquared = dot(viewVector, viewVector);
    float3 toView = viewLengthSquared > minVectorLengthSquared
        ? viewVector * rsqrt(viewLengthSquared)
        : float3(0.0);
    float NoV = saturate(dot(normal, toView));
    float metallic = saturate(Material.EmissiveMetallic.w) * saturate(metallicSample);
    float roughness = clamp(saturate(Material.NormalRoughness.w) * saturate(roughnessSample), 0.045, 1.0);
    float3 direct = float3(0.0);
	float3 directionalDirect = float3(0.0);
    if (Lighting.Counts.x > 0u)
    {
        float3 toLight = SafeNormal(-Lighting.DirectionalDirection.xyz);
        float3 radiance = Lighting.DirectionalColorIntensity.rgb
            * Lighting.DirectionalColorIntensity.a;
        FDirectionalShadowSample shadow = EvaluateDirectionalShadow(
            input.worldPosition,
            normal,
            Lighting.DirectionalShadowControl,
            Lighting.DirectionalShadowViewDepthTransform,
            Lighting.DirectionalShadowSplitDepths,
            Lighting.DirectionalShadowLightTransition,
            Lighting.DirectionalShadowCascades,
            DirectionalShadowTexture,
            DirectionalShadowSampler);
        if (Lighting.DirectionalShadowControl.y > 0.5)
		{
			output.color = float4(shadow.diagnosticColor, opacity);
			return output;
		}
		directionalDirect = EvaluatePBRDirectLighting(
            baseColor, metallic, roughness, normal, toLight, toView, radiance)
            * shadow.attenuation;
		direct += directionalDirect;
    }
    [unroll]
    for (uint lightIndex = 0u; lightIndex < 4u; ++lightIndex)
    {
        if (lightIndex >= Lighting.Counts.y)
            break;
        float3 lightVector = Lighting.Local[lightIndex].PositionInverseRange.xyz
            - input.worldPosition;
        float distanceSquared = dot(lightVector, lightVector);
        float3 toLight = distanceSquared > minVectorLengthSquared
            ? lightVector * rsqrt(distanceSquared) : float3(0.0);
        float inverseRange = Lighting.Local[lightIndex].PositionInverseRange.w;
        float normalizedDistance = sqrt(max(distanceSquared, 0.0)) * inverseRange;
        float rangeBase = 1.0 - saturate(normalizedDistance * normalizedDistance);
        float rangeAttenuation = rangeBase * rangeBase;
        float inverseSquare = 1.0 / max(distanceSquared, 0.0025);
        float coneAttenuation = 1.0;
        if (Lighting.Local[lightIndex].DirectionType.w > 0.5)
        {
            float angleCosine = dot(
                Lighting.Local[lightIndex].DirectionType.xyz, -toLight);
            float4 cone = Lighting.Local[lightIndex].SpotCone;
            if (cone.w > 0.5)
                coneAttenuation = angleCosine > cone.y ? 1.0 : 0.0;
            else
            {
                float coneValue = saturate((angleCosine - cone.y) * cone.z);
                coneAttenuation = coneValue * coneValue
                    * (3.0 - 2.0 * coneValue);
            }
        }
        float3 radiance = Lighting.Local[lightIndex].ColorIntensity.rgb
            * Lighting.Local[lightIndex].ColorIntensity.a
            * inverseSquare * rangeAttenuation * coneAttenuation;
        direct += EvaluatePBRDirectLighting(
            baseColor, metallic, roughness, normal, toLight, toView, radiance);
    }
    float3 reflectionDirection = reflect(-toView, normal);
    float3 irradiance = EnvironmentIrradiance.Sample(
        EnvironmentSampler, normal).rgb;
    float3 prefiltered = EnvironmentPrefiltered.SampleLevel(
        EnvironmentSampler, reflectionDirection,
        roughness * 6.0).rgb;
    float2 environmentBrdf = EnvironmentBrdfLut.Sample(
        EnvironmentSampler, float2(NoV, roughness)).rg;
    float3 environment = EvaluatePBREnvironmentLighting(
        baseColor,
        metallic,
        roughness,
        ambientOcclusion,
        NoV,
        irradiance,
        prefiltered,
        environmentBrdf);
	output.color = float4(direct + environment + emissive, opacity);
	output.directionalDirect = float4(directionalDirect, 0.0);
	return output;
}
