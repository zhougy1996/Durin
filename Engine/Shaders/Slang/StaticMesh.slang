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

struct LightingUniform
{
    float4 LightDirection;
    float4 LightColorIntensity;
    float4 ViewPosition;
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
[[vk::binding(11, 0)]] SamplerState MaterialSampler;
[[vk::binding(12, 0)]] TextureCube<float4> EnvironmentIrradiance;
[[vk::binding(13, 0)]] TextureCube<float4> EnvironmentPrefiltered;
[[vk::binding(14, 0)]] Texture2D<float4> EnvironmentBrdfLut;
[[vk::binding(15, 0)]] SamplerState EnvironmentSampler;

[shader("vertex")]
VSOutput VertexMain(FLocalVertexFactoryInput input)
{
    FLocalVertexFactoryIntermediates vertex =
        GetLocalVertexFactoryIntermediates(input);
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
    return SelectUV(input, channel) * transform.xy + transform.zw;
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
float4 FragmentMain(VSOutput input) : SV_Target
{
    const float minVectorLengthSquared = 1.0e-8;
    const float minBRDFDivisor = 1.0e-5;
    const float pi = 3.14159265358979323846;
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, GetMaterialUV(input, 0u));
    float3 normalSample = DecodeTextureNormal(
        NormalTexture.Sample(MaterialSampler, GetMaterialUV(input, 1u)).rg);
    float metallicSample = MetallicTexture.Sample(MaterialSampler, GetMaterialUV(input, 2u)).r;
    float roughnessSample = RoughnessTexture.Sample(MaterialSampler, GetMaterialUV(input, 3u)).r;
    float ambientOcclusionSample = AmbientOcclusionTexture.Sample(MaterialSampler, GetMaterialUV(input, 4u)).r;
    float3 emissiveSample = EmissiveTexture.Sample(MaterialSampler, GetMaterialUV(input, 5u)).rgb;
    float opacitySample = OpacityTexture.Sample(MaterialSampler, GetMaterialUV(input, 6u)).r;
    float opacityMaskSample = OpacityMaskTexture.Sample(MaterialSampler, GetMaterialUV(input, 7u)).r;
    float3 baseColor = saturate(Material.BaseColor.rgb) * baseColorSample.rgb * input.color.rgb;
    float opacity = saturate(Material.BaseColor.a) * saturate(opacitySample) * saturate(input.color.a);
    float ambientOcclusion = saturate(Material.SurfaceParams.x) * saturate(ambientOcclusionSample);
    float opacityMask = saturate(Material.SurfaceParams.y) * saturate(opacityMaskSample);
    // SurfaceParams.w is a zero-valued runtime carry that keeps Stage 3's
    // fixed AO/mask descriptors reflected until later passes consume them.
    opacity = saturate(opacity + Material.SurfaceParams.w * (ambientOcclusion + opacityMask));
    float3 emissive = max(Material.EmissiveMetallic.xyz, 0.0) + max(emissiveSample, 0.0);
    if (Material.SurfaceParams.z < 0.5)
        return float4(baseColor + emissive, opacity);

    float normalLengthSquared = dot(input.worldNormal, input.worldNormal);
    float3 geometricNormal = normalLengthSquared > minVectorLengthSquared
        ? input.worldNormal * rsqrt(normalLengthSquared)
        : float3(0.0, 0.0, 1.0);
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
    float3 lightVector = -Lighting.LightDirection.xyz;
    float lightLengthSquared = dot(lightVector, lightVector);
    float3 toLight = lightLengthSquared > minVectorLengthSquared
        ? lightVector * rsqrt(lightLengthSquared)
        : float3(0.0);
    float3 viewVector = Lighting.ViewPosition.xyz - input.worldPosition;
    float viewLengthSquared = dot(viewVector, viewVector);
    float3 toView = viewLengthSquared > minVectorLengthSquared
        ? viewVector * rsqrt(viewLengthSquared)
        : float3(0.0);
    float3 halfVectorInput = toLight + toView;
    float halfLengthSquared = dot(halfVectorInput, halfVectorInput);
    float3 halfVector = halfLengthSquared > minVectorLengthSquared
        ? halfVectorInput * rsqrt(halfLengthSquared)
        : float3(0.0);
    float NoL = saturate(dot(normal, toLight));
    float NoV = saturate(dot(normal, toView));
    float NoH = saturate(dot(normal, halfVector));
    float VoH = saturate(dot(toView, halfVector));
    float metallic = saturate(Material.EmissiveMetallic.w) * saturate(metallicSample);
    float roughness = clamp(saturate(Material.NormalRoughness.w) * saturate(roughnessSample), 0.045, 1.0);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float distributionTerm = NoH * NoH * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(pi * distributionTerm * distributionTerm, minBRDFDivisor);
    float visibility = 0.5 / max(
        NoL * sqrt(NoV * NoV * (1.0 - alpha2) + alpha2)
            + NoV * sqrt(NoL * NoL * (1.0 - alpha2) + alpha2),
        minBRDFDivisor);
    float3 f0 = lerp(float3(0.04), baseColor, metallic);
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * baseColor / pi;
    float3 lightColor = Lighting.LightColorIntensity.rgb * Lighting.LightColorIntensity.a;
    float3 direct = (diffuse + distribution * visibility * fresnel) * lightColor * NoL;
    float3 reflectionDirection = reflect(-toView, normal);
    float3 environmentFresnel = f0
        + (max(float3(1.0 - roughness), f0) - f0) * pow(1.0 - NoV, 5.0);
    float3 environmentDiffuse = EnvironmentIrradiance.Sample(
        EnvironmentSampler, normal).rgb * baseColor / pi
        * (1.0 - environmentFresnel) * (1.0 - metallic);
    float3 prefiltered = EnvironmentPrefiltered.SampleLevel(
        EnvironmentSampler, reflectionDirection,
        roughness * 6.0).rgb;
    float2 environmentBrdf = EnvironmentBrdfLut.Sample(
        EnvironmentSampler, float2(NoV, roughness)).rg;
    float3 environmentSpecular = prefiltered
        * (f0 * environmentBrdf.x + environmentBrdf.y);
    float3 environment = (environmentDiffuse + environmentSpecular)
        * ambientOcclusion;
    return float4(direct + environment + emissive, opacity);
}
