#pragma once

#include "RenderCoreAPI.h"
#include "SceneView.h"

namespace Durin::SceneViewProjection
{
	inline constexpr double DefaultPerspectiveNearClip = 0.1;
	inline constexpr double DefaultPerspectiveFarClip = 500000.0;
	inline constexpr double DefaultTerrainFadeStart = 180000.0;
	inline constexpr double DefaultTerrainRenderDistance = 200000.0;
	inline constexpr double MinimumTerrainFarPlaneSafetyMargin = 10000.0;
	inline constexpr double MaximumPerspectiveFarClip = 10000000.0;
	inline auto GetNearDeviceDepth(ESceneDepthConvention DepthConvention) -> double
	{
		return DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0 : 0.0;
	}
	inline auto GetFarDeviceDepth(ESceneDepthConvention DepthConvention) -> double
	{
		return 1.0 - GetNearDeviceDepth(DepthConvention);
	}
	inline auto GetTerrainFarPlaneSafetyMargin(double FarClip) -> double
	{
		return std::min(MinimumTerrainFarPlaneSafetyMargin,
			std::max(1.0, FarClip * 0.05));
	}

	// Builds the engine's x-forward Vulkan perspective matrix after bounded validation.
	RENDERCORE_API auto BuildPerspectiveProjection(double FieldOfViewDegrees,
		double AspectRatio, double NearClip, double FarClip,
		ESceneDepthConvention DepthConvention, FMatrix& OutProjection) -> bool;
	RENDERCORE_API auto IsValidPerspectiveClipRange(double NearClip,
		double FarClip) -> bool;
	RENDERCORE_API auto ProjectWorldToViewport(const FSceneView& View, const FVector3& WorldPosition, FVector2f& OutPosition) -> bool;
	RENDERCORE_API auto BuildViewportRay(const FSceneView& View, const FVector2f& ViewportPosition, FVector3& OutOrigin, FVector3& OutDirection) -> bool;
}
