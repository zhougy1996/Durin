#pragma once

#include "RenderCoreAPI.h"
#include "SceneView.h"

#include <algorithm>
#include <cmath>

namespace Durin::SceneViewProjection
{
	inline constexpr double DefaultPerspectiveNearClip = 0.1;
	inline constexpr double DefaultPerspectiveFarClip = 500000.0;
	inline constexpr double DefaultPerspectiveFieldOfViewDegrees = 60.0;
	inline constexpr double MinimumPerspectiveFieldOfViewDegrees = 1.0;
	inline constexpr double MaximumPerspectiveFieldOfViewDegrees = 170.0;
	inline constexpr double MinimumPerspectiveNearClip = 0.001;
	inline constexpr double DefaultViewFadeStart = 180000.0;
	inline constexpr double DefaultViewRenderDistance = 200000.0;
	inline constexpr double MaximumFarPlaneSafetyMargin = 10000.0;
	inline constexpr double MaximumPerspectiveFarClip = 10000000.0;
	inline auto GetNearDeviceDepth(ESceneDepthConvention DepthConvention) -> double
	{
		return DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0 : 0.0;
	}
	inline auto GetFarDeviceDepth(ESceneDepthConvention DepthConvention) -> double
	{
		return 1.0 - GetNearDeviceDepth(DepthConvention);
	}
	inline auto GetFarPlaneSafetyMargin(double FarClip) -> double
	{
		return std::min(MaximumFarPlaneSafetyMargin,
			std::max(1.0, FarClip * 0.05));
	}

	// Highest view render distance a far plane admits; matches the clamp applied by
	// ClampViewDistances so UI bounds and stored values cannot drift apart.
	inline auto GetMaximumViewRenderDistance(double FarClip) -> double
	{
		return std::max(1.0, FarClip - GetFarPlaneSafetyMargin(FarClip));
	}

	// Normalizes field of view to the bounded policy shared by runtime and editor cameras.
	inline auto ClampFieldOfViewDegrees(double FieldOfViewDegrees) -> double
	{
		if (!std::isfinite(FieldOfViewDegrees)) FieldOfViewDegrees = DefaultPerspectiveFieldOfViewDegrees;
		return std::clamp(FieldOfViewDegrees,
			MinimumPerspectiveFieldOfViewDegrees,
			MaximumPerspectiveFieldOfViewDegrees);
	}

	// Normalizes near/far clip planes: finite inputs, near >= 0.001, and far >= near + 1
	// up to the maximum representable far plane.
	inline auto ClampPerspectiveClipRange(double NearClip, double FarClip,
		double& OutNearClip, double& OutFarClip) -> void
	{
		if (!std::isfinite(NearClip)) NearClip = DefaultPerspectiveNearClip;
		if (!std::isfinite(FarClip)) FarClip = DefaultPerspectiveFarClip;
		OutNearClip = std::max(NearClip, MinimumPerspectiveNearClip);
		OutFarClip = std::clamp(FarClip, OutNearClip + 1.0, MaximumPerspectiveFarClip);
	}

	// Normalizes view fade/render distances against a validated far plane.
	inline auto ClampViewDistances(double FarClip, double FadeStart, double RenderDistance,
		double& OutFadeStart, double& OutRenderDistance) -> void
	{
		if (!std::isfinite(FadeStart)) FadeStart = DefaultViewFadeStart;
		if (!std::isfinite(RenderDistance)) RenderDistance = DefaultViewRenderDistance;
		OutRenderDistance = std::clamp(RenderDistance, 1.0, GetMaximumViewRenderDistance(FarClip));
		OutFadeStart = std::clamp(FadeStart, 0.0, std::max(0.0, OutRenderDistance - 1.0));
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
