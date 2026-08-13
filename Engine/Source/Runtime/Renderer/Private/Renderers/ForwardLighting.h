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
	struct FViewRenderCounters;

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

	struct alignas(16) FForwardDirectionalShadowUniform
	{
		FMatrix4f WorldToShadow{1.0f};
		// x = enabled, y = receiver bias, zw = texel world size.
		FVector4f Params{0.0f};
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
	static_assert(sizeof(FForwardDirectionalShadowUniform) == 80);
	static_assert(sizeof(FForwardLightingUniform) == 400);
	static_assert(alignof(FForwardLightingUniform) == 16);

	RENDERER_API auto PrepareLightView_RenderThread(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderCounters& Counters) -> FPreparedLightView;
	RENDERER_API auto BuildForwardLightingUniform(
		const FPreparedLightView& Lights,
		const FSceneView& View,
		const FPreparedDirectionalShadowView* Shadow = nullptr)
		-> FForwardLightingUniform;
}
