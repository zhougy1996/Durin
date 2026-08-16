module DeferredDirectionalLighting;

import Lighting.DirectionalShadow;
import Lighting.ForwardLightingUniform;
import Lighting.PBRLighting;
import Lighting.SurfaceLighting;
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

struct FDeferredDirectionalViewUniform
{
    float4 ProjectionRow0;
    float4 ProjectionRow1;
    float4 ProjectionRow2;
    float4 ProjectionRow3;
    float4x4 ViewToWorld;
    float4 ClearColor;
    float4 Params; // xy = inverse viewport, z = diagnostic, w = preserve background
	float4 ContactParams; // x = enabled, y = contribution diagnostic
};

struct FDeferredLightingOutput
{
    float4 Color : SV_Target0;
};

FDeferredLightingOutput MakeDeferredLightingOutput(
    float4 color, float3 directionalDirect)
{
    FDeferredLightingOutput output;
    output.Color = color;
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> GBufferMaterial;
[[vk::binding(1, 0)]] Texture2D<float4> GBufferNormals;
[[vk::binding(2, 0)]] Texture2D<float4> GBufferSurface;
[[vk::binding(3, 0)]] Texture2D<float3> GBufferEmissive;
[[vk::binding(4, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(5, 0)]] TextureCube<float4> EnvironmentIrradiance;
[[vk::binding(6, 0)]] TextureCube<float4> EnvironmentPrefiltered;
[[vk::binding(7, 0)]] Texture2D<float4> EnvironmentBrdfLut;
[[vk::binding(8, 0)]] SamplerState EnvironmentSampler;
[[vk::binding(9, 0)]] Texture2DArray<float> DirectionalShadowTexture;
[[vk::binding(10, 0)]] SamplerComparisonState DirectionalShadowSampler;
[[vk::binding(11, 0)]] ConstantBuffer<FDeferredDirectionalViewUniform> View;
[[vk::binding(12, 0)]] ConstantBuffer<FForwardLightingUniform> Lighting;
[[vk::binding(13, 0)]] Texture2D<float> GroundTruthAmbientOcclusionRaw;
[[vk::binding(14, 0)]] Texture2D<float> GroundTruthAmbientOcclusionFiltered;
[[vk::binding(15, 0)]] Texture2D<float> ContactVisibility;
[[vk::binding(16, 0)]] Texture2D<float> GroundTruthAmbientOcclusionResolved;
[[vk::binding(17, 0)]] Texture2D<float> GroundTruthAmbientOcclusionSelector;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

FDeferredLightingOutput EvaluateDeferredLighting(VSOutput input)
{
    int2 pixel = int2(input.pos.xy);
    FGBufferRecord record = DecodeGBuffer(
        GBufferMaterial.Load(int3(pixel, 0)),
        GBufferNormals.Load(int3(pixel, 0)),
        GBufferSurface.Load(int3(pixel, 0)),
        GBufferEmissive.Load(int3(pixel, 0)));
	uint aoDiagnostic = View.Params.w < -0.5
		? (uint)round(-View.Params.w) : 0u;
	bool gtaoEnabled = View.Params.z >= 0.0;
	int2 aoNativePixel = View.ContactParams.z > 0.5 ? pixel / 2 : pixel;
    if ((record.flags & GBufferStandardLitFlag) == 0u)
    {
		if (aoDiagnostic != 0u)
			return MakeDeferredLightingOutput(float4(0.0, 0.0, 0.0, 1.0), float3(0.0));
        if (View.Params.w > 0.5)
            discard;
        return MakeDeferredLightingOutput(View.ClearColor, float3(0.0));
    }

    float depth = SceneDepth.Load(int3(pixel, 0));
    uint targetWidth;
    uint targetHeight;
    GBufferMaterial.GetDimensions(targetWidth, targetHeight);
    // FitViewToOutput centers constrained content. Recover that integer origin
    // from the frozen inverse-viewport ABI so letterboxed views reconstruct in
    // viewport-local NDC while still loading absolute GBuffer pixels.
    float2 viewportSize = round(rcp(View.Params.xy));
    float2 viewportOrigin = floor(
        (float2(targetWidth, targetHeight) - viewportSize) * 0.5);
    float2 uv = (float2(pixel) + 0.5 - viewportOrigin) * View.Params.xy;
    float3 viewPosition;
    if (!ReconstructViewPositionAnalytic(
            View.ProjectionRow0,
            View.ProjectionRow1,
            View.ProjectionRow2,
            View.ProjectionRow3,
            uv * 2.0 - 1.0,
            depth,
            viewPosition))
    {
        if (View.Params.w > 0.5)
            discard;
        return MakeDeferredLightingOutput(View.ClearColor, float3(0.0));
    }
    float4 worldPositionH = mul(
        View.ViewToWorld, float4(viewPosition, 1.0));
    if (!all(isfinite(worldPositionH)) || abs(worldPositionH.w) <= 1.0e-8)
    {
        if (View.Params.w > 0.5)
            discard;
        return MakeDeferredLightingOutput(View.ClearColor, float3(0.0));
    }
    float3 worldPosition = worldPositionH.xyz / worldPositionH.w;
    FSurfaceLightingFrame frame = BuildSurfaceLightingFrame(
        worldPosition, record.shadingNormal, Lighting.ViewPosition.xyz);

    float3 directional = float3(0.0);
	float contactVisibility = View.ContactParams.x > 0.5
		? ContactVisibility.Load(int3(pixel, 0)) : 1.0;
    if (Lighting.Counts.x > 0u)
    {
        float3 lightVector = -Lighting.DirectionalDirection.xyz;
        float lightLengthSquared = dot(lightVector, lightVector);
        float3 toLight = lightLengthSquared > 1.0e-8
            ? lightVector * rsqrt(lightLengthSquared)
            : float3(0.0, 0.0, 1.0);
        float3 radiance = Lighting.DirectionalColorIntensity.rgb
            * Lighting.DirectionalColorIntensity.a;
        FDirectionalShadowSample shadow = EvaluateDirectionalShadow(
            worldPosition,
            record.geometricNormal,
            Lighting.DirectionalShadowControl,
            Lighting.DirectionalShadowViewDepthTransform,
            Lighting.DirectionalShadowSplitDepths,
            Lighting.DirectionalShadowLightTransition,
            Lighting.DirectionalShadowCascades,
            DirectionalShadowTexture,
            DirectionalShadowSampler);
        if (Lighting.DirectionalShadowControl.y > 0.5)
            return MakeDeferredLightingOutput(
                float4(shadow.diagnosticColor, record.effectiveOpacity),
                float3(0.0));
        directional = EvaluateSurfaceDirectionalLighting(
            record.baseColor,
            record.metallic,
            record.roughness,
            frame,
            toLight,
            radiance,
			shadow.attenuation * contactVisibility);
    }
	if (View.ContactParams.y > 0.5)
		return MakeDeferredLightingOutput(
			float4(1.0 - contactVisibility, 0.0, 0.0, 1.0), directional);

    float3 local = float3(0.0);
    [unroll]
    for (uint lightIndex = 0u; lightIndex < 4u; ++lightIndex)
    {
        if (lightIndex >= Lighting.Counts.y)
            break;
        local += EvaluateSurfaceLocalLighting(
            record.baseColor,
            record.metallic,
            record.roughness,
            worldPosition,
            frame,
            Lighting.Local[lightIndex]);
    }

    float3 environment = EvaluateSurfaceEnvironmentLighting(
        record.baseColor,
        record.metallic,
        record.roughness,
		record.ambientOcclusion * (gtaoEnabled
			? GroundTruthAmbientOcclusionResolved.Load(int3(pixel, 0)) : 1.0),
        frame,
        EnvironmentIrradiance,
        EnvironmentPrefiltered,
        EnvironmentBrdfLut,
        EnvironmentSampler);
    float4 composed = ComposeSurfaceLighting(
        directional + local,
        environment,
        record.emissive,
        record.effectiveOpacity);
	uint diagnostic = gtaoEnabled
		? (uint)round(View.Params.z)
		: (uint)round(-View.Params.z - 1.0);
	if (aoDiagnostic == 1u)
	{
		float visibility = gtaoEnabled
			? GroundTruthAmbientOcclusionRaw.Load(int3(aoNativePixel, 0)) : 1.0;
		return MakeDeferredLightingOutput(
			float4(visibility.xxx, 1.0), directional);
	}
	if (aoDiagnostic == 2u)
	{
		if (View.ContactParams.z > 0.5)
		{
			float2 halfCoordinate = (float2(pixel) + 0.5) * 0.5 - 0.5;
			int2 halfBase = int2(floor(halfCoordinate));
			uint halfWidth;
			uint halfHeight;
			GroundTruthAmbientOcclusionSelector.GetDimensions(
				halfWidth, halfHeight);
			uint acceptedCount = 0u;
			bool rejectedDepth = false;
			bool rejectedNormal = false;
			[unroll]
			for (uint index = 0u; index < 4u; ++index)
			{
				int2 offset = int2(index & 1u, index >> 1u);
				int2 halfPixel = halfBase + offset;
				if (any(halfPixel < int2(0, 0))
					|| any(halfPixel >= int2(halfWidth, halfHeight)))
					continue;
				uint selector = (uint)round(saturate(
					GroundTruthAmbientOcclusionSelector.Load(
						int3(halfPixel, 0))) * 255.0);
				if (selector < 1u || selector > 4u)
					continue;
				uint local = selector - 1u;
				int2 representativePixel = halfPixel * 2
					+ int2(local & 1u, local >> 1u);
				if (any(representativePixel < int2(0, 0))
					|| any(representativePixel
						>= int2(targetWidth, targetHeight)))
					continue;
				FGBufferRecord representativeRecord = DecodeGBuffer(
					GBufferMaterial.Load(int3(representativePixel, 0)),
					GBufferNormals.Load(int3(representativePixel, 0)),
					GBufferSurface.Load(int3(representativePixel, 0)),
					GBufferEmissive.Load(int3(representativePixel, 0)));
				float2 representativeUv =
					(float2(representativePixel) + 0.5 - viewportOrigin)
					* View.Params.xy;
				float3 representativePosition;
				if (!ReconstructViewPositionAnalytic(
						View.ProjectionRow0, View.ProjectionRow1,
						View.ProjectionRow2, View.ProjectionRow3,
						representativeUv * 2.0 - 1.0,
						SceneDepth.Load(int3(representativePixel, 0)),
						representativePosition)
					|| abs(representativePosition.x - viewPosition.x)
						> abs(viewPosition.x) * 0.01 + 0.01)
				{
					rejectedDepth = true;
					continue;
				}
				if (dot(record.geometricNormal,
						representativeRecord.geometricNormal) < 0.90)
				{
					rejectedNormal = true;
					continue;
				}
				++acceptedCount;
			}
			float3 confidence = acceptedCount > 0u
				? float3(0.0, float(acceptedCount) * 0.25, 0.0)
				: rejectedDepth && rejectedNormal ? float3(1.0, 0.0, 1.0)
				: rejectedDepth ? float3(1.0, 0.0, 0.0)
				: rejectedNormal ? float3(0.0, 0.25, 1.0)
				: float3(1.0);
			return MakeDeferredLightingOutput(
				float4(confidence, 1.0), directional);
		}
		uint width;
		uint height;
		GBufferSurface.GetDimensions(width, height);
		bool acceptedNeighbor = false;
		bool rejectedEdge = false;
		static const int2 offsets[4] = {
			int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
		float3 centerNormal = record.geometricNormal;
		[unroll]
		for (uint index = 0u; index < 4u; ++index)
		{
			int2 neighbor = pixel + offsets[index];
			if (any(neighbor < int2(0, 0))
				|| any(neighbor >= int2(width, height)))
			{
				rejectedEdge = true;
				continue;
			}
			FGBufferRecord neighborRecord = DecodeGBuffer(
				GBufferMaterial.Load(int3(neighbor, 0)),
				GBufferNormals.Load(int3(neighbor, 0)),
				GBufferSurface.Load(int3(neighbor, 0)),
				GBufferEmissive.Load(int3(neighbor, 0)));
			if ((neighborRecord.flags & GBufferStandardLitFlag) == 0u
				|| dot(centerNormal, neighborRecord.geometricNormal) < 0.90)
				rejectedEdge = true;
			else
				acceptedNeighbor = true;
		}
		float3 confidence = rejectedEdge ? float3(1.0, 0.75, 0.0)
			: acceptedNeighbor ? float3(0.0, 1.0, 0.0)
			: float3(0.0, 0.25, 1.0);
		return MakeDeferredLightingOutput(float4(confidence, 1.0), directional);
	}
	if (aoDiagnostic == 3u)
	{
		float visibility = gtaoEnabled
			? GroundTruthAmbientOcclusionFiltered.Load(int3(aoNativePixel, 0)) : 1.0;
		return MakeDeferredLightingOutput(
			float4(visibility.xxx, 1.0), directional);
	}
	if (aoDiagnostic == 4u)
	{
		float factor = record.ambientOcclusion * (gtaoEnabled
			? GroundTruthAmbientOcclusionResolved.Load(int3(pixel, 0)) : 1.0);
		return MakeDeferredLightingOutput(float4(factor.xxx, 1.0), directional);
	}
    if (diagnostic == 1u)
        return MakeDeferredLightingOutput(
            float4(record.baseColor, record.metallic), directional);
    if (diagnostic == 2u)
        return MakeDeferredLightingOutput(
            float4(directional, record.effectiveOpacity), directional);
    if (diagnostic == 3u)
        return MakeDeferredLightingOutput(
            float4(local, record.effectiveOpacity), directional);
    if (diagnostic == 4u)
        return MakeDeferredLightingOutput(
            float4(environment, record.effectiveOpacity), directional);
    if (diagnostic == 5u)
        return MakeDeferredLightingOutput(
            float4(record.emissive, record.effectiveOpacity), directional);
    if (diagnostic == 6u)
        return MakeDeferredLightingOutput(
            float4(record.effectiveOpacity.xxx, record.effectiveOpacity),
            directional);
    return MakeDeferredLightingOutput(composed, directional);
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target0
{
    return EvaluateDeferredLighting(input).Color;
}

[shader("fragment")]
FDeferredLightingOutput ProductionFragmentMain(VSOutput input)
{
    return EvaluateDeferredLighting(input);
}
