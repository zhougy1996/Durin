// Pass execution owns lighting, attachments and geometry entry bodies.
// Included after generated evaluation functions; the environment includes this
// file without instantiation so its exact bytes participate in dependency identity.
#if !DURIN_GENERATED_MATERIAL
#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage)
#include "SkyParameters.inl"
#include "HitProxyParameters.inl"
#include "SingleLayerWaterParameters.inl"
#undef DURIN_MATERIAL_PARAMETER
#endif
#if DURIN_GENERATED_MATERIAL
#if DURIN_MATERIAL_VERTEX_STAGE


#endif
#if DURIN_MATERIAL_PIXEL_STAGE

#if DURIN_MATERIAL_SHADING_MODEL == 1
import ProceduralSky;

struct MaterialSkyUniform
{
    float4x4 ClipToSkyDirection;
    float4 TintIntensity;
    FProceduralSkyUniform ProceduralSky;
    float4 DepthParams;
};
#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage) Type Name;
#include "SkyParameters.inl"
#undef DURIN_MATERIAL_PARAMETER

[shader("fragment")]
float4 SkyBackgroundMain(float4 position : SV_Position, float2 clipPosition : TEXCOORD0, out float depth : SV_Depth) : SV_Target0
{
    depth = MaterialSky.DepthParams.x;
    float3 direction = normalize(mul(MaterialSky.ClipToSkyDirection, float4(clipPosition, 1, 1)).xyz);
    VSOutput input = (VSOutput)0;
    input.pos = position;
    input.worldPosition = direction;
    input.worldNormal = direction;
    input.uv0 = clipPosition * 0.5 + 0.5;
    input.color = float4(1);
#if DURIN_MATERIAL_PROCEDURAL_SKY
    float3 radiance = MaterialSky.ProceduralSky.ToSunEnabled.w > 0.5
        ? EvaluateProceduralSky(MaterialSky.ProceduralSky, direction) : float3(0);
    return float4(EvaluateGeneratedMaterial(input, radiance).emissive, 1);
#else
    return float4(EvaluateGeneratedMaterial(input).emissive, 1);
#endif
}
#endif

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
#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage) Type Name;
#include "HitProxyParameters.inl"
#undef DURIN_MATERIAL_PARAMETER

#if DURIN_MATERIAL_SHADING_MODEL == 3
[shader("fragment")]
float SingleLayerWaterDepthFragmentMain(VSOutput input) : SV_Target0
{
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(EvaluateGeneratedMaterial(input).opacityMask,
            asfloat(uint(DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
    return 1.0;
}

#if DURIN_NUM_GetSingleLayerWaterMaterialOutput
struct SingleLayerWaterMaterialOutput
{
    float4 sceneColor : SV_Target0;
    float4 normalAndRoughness : SV_Target1;
    float4 specularF0 : SV_Target2;
};

#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage) Type Name;
#include "SingleLayerWaterParameters.inl"
#undef DURIN_MATERIAL_PARAMETER

[shader("fragment")]
SingleLayerWaterMaterialOutput SingleLayerWaterFragmentMain(VSOutput input, bool isFrontFace : SV_IsFrontFace)
{
    int3 pixel = int3(int2(input.pos.xy), 0);
    FMaterialCustomOutputs customOutputs;
    FMaterialSurface surface = EvaluateGeneratedMaterial(input, customOutputs);
    float3 scattering = max(FiniteSurfaceColor(customOutputs.GetSingleLayerWaterMaterialOutput0(), 0.0), 0.0);
    float3 absorption = max(FiniteSurfaceColor(customOutputs.GetSingleLayerWaterMaterialOutput1(), 0.0), 0.0);
    float phaseG = clamp(FiniteSurfaceValue(customOutputs.GetSingleLayerWaterMaterialOutput2(), 0.0), -1.0, 1.0);
    float3 colorScale = max(FiniteSurfaceColor(customOutputs.GetSingleLayerWaterMaterialOutput3(), 1.0), 0.0);
    FResolvedGeneratedSurfaceShading shading = ResolveGeneratedSurfaceShading(input, surface, isFrontFace);
    float3 viewForward = WaterViewForward(WaterView);
    float3 surfaceInView = mul(MaterialView.WorldToView, float4(input.worldPosition, 1.0)).xyz;
    float surfaceDepth = dot(surfaceInView, viewForward);
    float slopeTolerance = max(abs(ddx_fine(surfaceDepth)), abs(ddy_fine(surfaceDepth)));
    if (WaterCoverage.Load(pixel) < 0.5)
        discard;
#if DURIN_MATERIAL_BLEND_MODE == 1
    if (RejectMaterialMask(surface.opacityMask, asfloat(uint(DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS))))
        discard;
#endif
    float3 normal = shading.normalFrame.shadingNormal;
    float roughness = shading.effectiveRoughness;
    float3 incident = WaterIncidentDirection(WaterView, surfaceInView, viewForward);
    FSurfaceLightingFrame frame = BuildSurfaceLightingFrameFromViewVector(normal, mul((float3x3)MaterialView.ViewToWorld, -incident));
    float waterIor = WaterIor(surface.specular);
    float4 viewport = MaterialView.Viewport;
    FWaterBackground background = SampleWaterBackground(WaterBackgroundColor, WaterBackgroundDepth, WaterCoverage, WaterView,
        viewport, input.pos.xy, viewForward, surfaceDepth, slopeTolerance, DURIN_MATERIAL_REFRACTION_METHOD != 0);
    float2 offset = 0.0;
#if DURIN_MATERIAL_REFRACTION_METHOD != 0
    float3 viewNormal = normalize(mul((float3x3)MaterialView.WorldToView, normal));
#if DURIN_MATERIAL_REFRACTION_METHOD == 3
    float3 vertexNormal = normalize(mul((float3x3)MaterialView.WorldToView, shading.normalFrame.geometricNormal));
    float3 distortionNormal = vertexNormal - viewNormal;
#else
    float3 distortionNormal = viewNormal;
#endif
#if DURIN_MATERIAL_REFRACTION_METHOD == 2
    float distortionStrength = waterIor;
#else
    float distortionStrength = surface.refraction;
#endif
    if (background.valid)
    {
        float2 distortion = WaterDistortionPixels(WaterView, viewport.zw, distortionNormal, viewForward, distortionStrength,
            max(background.depth - surfaceDepth, 0.0) * 100.0,
            asfloat(uint(DURIN_MATERIAL_REFRACTION_DEPTH_BIAS_BITS)), background.open);
        float2 target = ClampWaterPixel(ClampWaterPixel(input.pos.xy, viewport) + distortion, viewport);
        if (any(distortion != 0.0))
        {
            FWaterBackground refracted = SampleWaterBackground(WaterBackgroundColor, WaterBackgroundDepth, WaterCoverage, WaterView,
                viewport, target, viewForward, surfaceDepth, slopeTolerance, true);
            if (refracted.valid)
            {
                background = refracted;
                offset = target - input.pos.xy;
            }
        }
    }
#endif
    if (!background.valid) background.depth = surfaceDepth;
    float thicknessCm = max(background.depth - surfaceDepth, 0.0) * 100.0;
    float3 extinction = scattering + absorption;
    float3 transmission = WaterTransmission(extinction, thicknessCm, background.open);
    float3 scatterWeight = WaterIntegratedScattering(scattering, extinction, transmission);
    float3 bedLight = 1.0;
    if (background.valid && !background.open)
    {
        float3 bedInView = ReconstructWaterBed(WaterView, viewport, input.pos.xy, viewForward, background.depth);
        float3 bedInWorld = mul(MaterialView.ViewToWorld, float4(bedInView, 1.0)).xyz;
        bedLight = exp(-extinction * (max(input.worldPosition.z - bedInWorld.z, 0.0) * 100.0));
    }
    float shoreline = background.open ? 1.0 : saturate(thicknessCm * 0.02);
    float3 behind = background.radiance * bedLight * lerp(1.0, colorScale, shoreline);

    float3 rotatedNormal = normal + 2.0 * cross(Lighting.EnvironmentRotation.xyz,
        cross(Lighting.EnvironmentRotation.xyz, normal) + Lighting.EnvironmentRotation.w * normal);
    float3 ambient = EnvironmentIrradiance.SampleLevel(EnvironmentSampler, rotatedNormal, 0).rgb
        * Lighting.EnvironmentControl.x * surface.ambientOcclusion;
    float3 incoming = ambient * 0.079577471546;
    float3 direct = 0.0;
    if (Lighting.Counts.x > 0u)
    {
        float3 lightDirection = SafeSurfaceNormal(Lighting.DirectionalDirection.xyz);
        float3 radiance = Lighting.DirectionalColorIntensity.rgb * Lighting.DirectionalColorIntensity.a;
        FDirectionalShadowSample shadow = EvaluateDirectionalShadow(input.worldPosition, shading.normalFrame.geometricNormal,
            Lighting.DirectionalShadowControl, Lighting.DirectionalShadowViewDepthTransform,
            Lighting.DirectionalShadowSplitDepths, Lighting.DirectionalShadowLightTransition,
            Lighting.DirectionalShadowCascades, DirectionalShadowTexture, DirectionalShadowSampler);
        float3 ray = refract(-frame.toView, normal, 1.0 / waterIor);
        if (dot(ray, ray) > 0.0)
            incoming += radiance * shadow.attenuation * WaterSchlickPhase(phaseG, dot(lightDirection, ray));
        direct = EvaluateSurfaceDirectionalLighting(surface.baseColor, surface.metallic, surface.specular, roughness,
            frame, -lightDirection, radiance, shadow.attenuation, 0.0, 0.1, surface.opacity);
    }
    float f0 = 0.08 * surface.specular;
    float2 brdf = EnvironmentBrdfLut.SampleLevel(EnvironmentSampler, float2(frame.NoV, roughness), 0).rg;
    float3 scattered = incoming * scatterWeight * (1.0 - saturate(f0 * brdf.x + brdf.y));
    SingleLayerWaterMaterialOutput result;
    float3 color = surface.emissive + direct + (1.0 - surface.opacity) * (scattered + behind * transmission);
    if (WaterView.DepthParameters.y == 1.0) color = transmission;
    if (WaterView.DepthParameters.y == 2.0) color = bedLight;
    if (WaterView.DepthParameters.y == 3.0) color = float3(offset, float(background.valid));
    result.sceneColor = float4(color, 0.0);
    result.normalAndRoughness = float4(EncodeOctahedralNormal(normal), roughness, 0.0);
    result.specularF0 = float4(float3(f0), 0.0);
    return result;
}
#endif
#endif

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
