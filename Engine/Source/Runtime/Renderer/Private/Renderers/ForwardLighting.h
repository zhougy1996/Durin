#pragma once

#include "Engine/LightSceneProxy.h"
#include "RHIResources.h"
#include "RendererAPI.h"

#include <array>
#include <vector>

namespace Durin
{
	struct FPreparedDirectionalShadowView;
	class FScene;
	struct FSceneView;
	struct FViewRenderTelemetry;

	inline constexpr uint32 MaxPreparedDirectionalLights = 1;
	inline constexpr uint32 MaxPreparedLocalLights = 4;
	inline constexpr float LocalLightNearDistance = 0.05f;

	struct FPreparedDirectionalLight
	{
		FLightSceneId Id;
		FDirectionalLightSceneData Data;
	};

	struct FPreparedLocalLight
	{
		FLightSceneId Id;
		ELightSceneProxyKind Kind = ELightSceneProxyKind::Point;
		FVector3 Position{0.0};
		FVector3 Direction{1.0, 0.0, 0.0};
		FVector3f Color{1.0f};
		float Intensity = 0.0f;
		float Range = 1.0f;
		float InnerConeAngle = 0.0f;
		float OuterConeAngle = 0.0f;
	};

	// Owns the copied, bounded direct-light set selected for one immutable view.
	struct FPreparedLightView
	{
		std::vector<FPreparedDirectionalLight> Directional;
		std::vector<FPreparedLocalLight> Local;
	};

	struct alignas(16) FForwardDirectionalLightUniform
	{
		FVector4f Direction{0.0f};
		FVector4f ColorIntensity{0.0f};
	};

	struct alignas(16) FForwardLocalLightUniform
	{
		FVector4f PositionInverseRange{0.0f};
		FVector4f DirectionType{0.0f};
		FVector4f ColorIntensity{0.0f};
		FVector4f SpotCone{0.0f};
	};

	struct alignas(16) FForwardDirectionalShadowCascadeUniform
	{
		FMatrix4f WorldToShadow{1.0f};
		// xy = texel world size, z = receiver world bias, w = normal offset.
		FVector4f TexelBias{0.0f};
		// xyz = raster terms, w = normalized raster separation.
		FVector4f RasterBias{0.0f};
		// xy = texture texel step, z = quality identity, w = footprint radius.
		FVector4f Filter{0.0f};
		// xy = minimum valid UV, zw = maximum valid UV.
		FVector4f ValidRegion{0.0f};
	};

	struct alignas(16) FForwardDirectionalShadowUniform
	{
		// x = enabled, y = diagnostic mode, z = cascade count, w = candidate.
		FVector4f Control{0.0f};
		// View-matrix forward row; its dot with world position is receiver depth.
		FVector4f ViewDepthTransform{0.0f};
		// Four ordered boundaries; unused entries repeat the far boundary.
		FVector4f SplitDepths{0.0f};
		// xyz = selected light travel direction, w = transition fraction.
		FVector4f LightTransition{0.0f};
		std::array<FForwardDirectionalShadowCascadeUniform,
			3> Cascades{};
	};

	// Fixed reflected ABI uploaded exactly once for each rendered view.
	struct alignas(16) FForwardLightingUniform
	{
		FVector4f ViewPosition{0.0f};
		alignas(16) std::array<uint32, 4> Counts{};
		FForwardDirectionalLightUniform Directional;
		FForwardDirectionalShadowUniform DirectionalShadow;
		std::array<FForwardLocalLightUniform, MaxPreparedLocalLights> Local{};
	};

	static_assert(sizeof(FForwardDirectionalLightUniform) == 32);
	static_assert(sizeof(FForwardLocalLightUniform) == 64);
	static_assert(sizeof(FForwardDirectionalShadowCascadeUniform) == 128);
	static_assert(offsetof(FForwardDirectionalShadowCascadeUniform, TexelBias) == 64);
	static_assert(offsetof(FForwardDirectionalShadowCascadeUniform, RasterBias) == 80);
	static_assert(offsetof(FForwardDirectionalShadowCascadeUniform, Filter) == 96);
	static_assert(offsetof(FForwardDirectionalShadowCascadeUniform, ValidRegion) == 112);
	static_assert(sizeof(FForwardDirectionalShadowUniform) == 448);
	static_assert(offsetof(FForwardDirectionalShadowUniform, Cascades) == 64);
	static_assert(sizeof(FForwardLightingUniform) == 768);
	static_assert(alignof(FForwardLightingUniform) == 16);
	static_assert(offsetof(FForwardLightingUniform, DirectionalShadow) == 64);
	static_assert(offsetof(FForwardLightingUniform, Local) == 512);

	RENDERER_API auto PrepareLightView_RenderThread(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderTelemetry& Telemetry) -> FPreparedLightView;
	RENDERER_API auto BuildForwardLightingUniform(
		const FPreparedLightView& Lights,
		const FSceneView& View,
		const FPreparedDirectionalShadowView* Shadow = nullptr)
		-> FForwardLightingUniform;
}
