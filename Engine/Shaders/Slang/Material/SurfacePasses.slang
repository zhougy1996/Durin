// Pass execution owns lighting, attachments and geometry entry bodies.
// Included after generated evaluation functions; the environment includes this
// file without instantiation so its exact bytes participate in dependency identity.
#if DURIN_GENERATED_MATERIAL
#if DURIN_MATERIAL_VERTEX_STAGE


#endif
#if DURIN_MATERIAL_PIXEL_STAGE

struct FResolvedGeneratedSurfaceShading
{
    FMaterialNormalFrame normalFrame;
    float effectiveRoughness;
};
FResolvedGeneratedSurfaceShading ResolveGeneratedSurfaceShading(
    VSOutput input, FMaterialSurface surface, bool isFrontFace)
{
    FResolvedGeneratedSurfaceShading result;
    result.normalFrame = EvaluateMaterialNormalFrame(input.worldNormal,
        input.worldTangent, surface.tangentNormal, isFrontFace);
    result.effectiveRoughness = FilterSpecularRoughness(surface.roughness,
        result.normalFrame.shadingNormal, MaterialView.Parameters.w > 0.5);
    return result;
}
struct GeometryPassFragmentOutput
{
    float4 material : SV_Target0;
    float4 normals : SV_Target1;
    float4 surface : SV_Target2;
    float4 sceneColor : SV_Target3;
#if DURIN_MATERIAL_SHADING_MODEL == 2
    float4 modelData : SV_Target4;
#endif
};
[shader("fragment")]
GeometryPassFragmentOutput GeometryFragmentMain(
    VSOutput input, bool isFrontFace : SV_IsFrontFace)
{
#if DURIN_MATERIAL_SHADING_MODEL == 1
    discard;
    GeometryPassFragmentOutput empty = (GeometryPassFragmentOutput)0;
    return empty;
#else
    FMaterialSurface s = EvaluateGeneratedMaterial(input);
    FResolvedGeneratedSurfaceShading shading =
        ResolveGeneratedSurfaceShading(input, s, isFrontFace);
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(s.opacityMask, asfloat(uint(
            DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
    GeometryPassFragmentOutput o;
    o.material = float4(s.baseColor, s.ambientOcclusion);
    o.normals = float4(
        EncodeOctahedralNormal(shading.normalFrame.shadingNormal),
        EncodeOctahedralNormal(shading.normalFrame.geometricNormal));
    o.surface = float4(s.metallic, s.specular, shading.effectiveRoughness, 1.0 / 255.0);
#if DURIN_MATERIAL_SHADING_MODEL == 2
    o.modelData = float4(s.clearCoat, s.clearCoatRoughness, 0.0, 0.0);
    o.surface.a = 2.0 / 255.0;
#endif
    o.sceneColor = float4(s.emissive, 1.0);
#if DURIN_MATERIAL_AUTHORED_SPECULAR
    o.surface.a += 128.0 / 255.0;
#endif
    return o;
#endif
}
struct HitProxyUniform { uint4 Id; float4 ViewOrigin; };
#define DURIN_DECLARE_BINDINGS_HitProxy 1
#define DURIN_MATERIAL_BINDING(Role, Name, Type, Reflected, Runtime, Set, Slot, Stride, Stages, Entries) [[vk::binding(Slot, Set)]] Type Name;
#include "MaterialBindingSchema.inl"
#undef DURIN_MATERIAL_BINDING
#undef DURIN_DECLARE_BINDINGS_HitProxy

[shader("fragment")]
uint2 HitProxyFragmentMain(VSOutput input) : SV_Target0
{
#if DURIN_MATERIAL_BLEND_MODE == 1
    float mask = EvaluateGeneratedMaterial(input).opacityMask;
    if (mask < asfloat(uint(DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))) discard;
#endif
    return uint2(HitProxy.Id.x, asuint(length(input.worldPosition - HitProxy.ViewOrigin.xyz)));
}
[shader("fragment")]
void ShadowFragmentMain(VSOutput input)
{
#if DURIN_MATERIAL_BLEND_MODE == 1
        float mask = EvaluateGeneratedMaterial(input).opacityMask;
    if (mask < asfloat(uint(DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS)))
        discard;
#endif
}
[shader("fragment")]
float4 FragmentMain(
    VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    FMaterialSurface s = EvaluateGeneratedMaterial(input);
    FResolvedGeneratedSurfaceShading shading;
#if DURIN_MATERIAL_SHADING_MODEL != 1
    shading = ResolveGeneratedSurfaceShading(input, s, isFrontFace);
#endif
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(s.opacityMask, asfloat(uint(
            DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
#if DURIN_MATERIAL_SHADING_MODEL == 1
    return float4(s.emissive, s.opacity);
#else
    if (MaterialView.Parameters.z < 0.5) return float4(s.baseColor + s.emissive, s.opacity);
    FSurfaceLightingFrame lightingFrame = BuildSurfaceLightingFrame(
        input.worldPosition, shading.normalFrame.shadingNormal,
        Lighting.ViewPosition.xyz);
    float3 direct = float3(0.0);
    if (Lighting.Counts.x > 0u)
    {
        float3 toLight = SafeSurfaceNormal(-Lighting.DirectionalDirection.xyz);
        float3 radiance = Lighting.DirectionalColorIntensity.rgb
            * Lighting.DirectionalColorIntensity.a;
        FDirectionalShadowSample shadow;
        shadow.attenuation = 1.0;
        shadow.diagnosticColor = float3(1.0);
        // Match the BRDF's mapped shading normal, preserving diagnostic output.
        if (Lighting.DirectionalShadowControl.y > 0.5
            || dot(lightingFrame.normal, toLight) > 0.0)
            shadow = EvaluateDirectionalShadow(
                input.worldPosition, shading.normalFrame.geometricNormal,
                Lighting.DirectionalShadowControl,
                Lighting.DirectionalShadowViewDepthTransform,
                Lighting.DirectionalShadowSplitDepths,
                Lighting.DirectionalShadowLightTransition,
                Lighting.DirectionalShadowCascades,
                DirectionalShadowTexture, DirectionalShadowSampler);
        if (Lighting.DirectionalShadowControl.y > 0.5)
            return float4(shadow.diagnosticColor, s.opacity);
        direct += EvaluateSurfaceDirectionalLighting(s.baseColor, s.metallic, s.specular,
            shading.effectiveRoughness, lightingFrame, toLight, radiance,
            shadow.attenuation, s.clearCoat, s.clearCoatRoughness);
    }
    direct = AccumulateViewLocalLights(direct, s.baseColor, s.metallic, s.specular, shading.effectiveRoughness,
        input.worldPosition, lightingFrame, LocalLights, Lighting.Counts.y, s.clearCoat, s.clearCoatRoughness);
    float3 environment = EvaluateSurfaceEnvironmentLighting(s.baseColor,
        s.metallic, s.specular, shading.effectiveRoughness, s.ambientOcclusion,
        lightingFrame, EnvironmentIrradiance, EnvironmentPrefiltered,
        EnvironmentBrdfLut, EnvironmentSampler, Lighting.EnvironmentRotation, Lighting.EnvironmentControl, s.clearCoat, s.clearCoatRoughness);
    return ComposeSurfaceLighting(direct, environment, s.emissive, s.opacity);
#endif
}

#endif
#endif
