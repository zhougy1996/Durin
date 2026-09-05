#pragma once

#include "Misc/CoreTypes.h"
#include "VolumetricCloudView.h"

namespace Durin
{
	enum class EContactShadowExecutionRoute : uint8
	{
		None,
		Compute,
		Fragment,
	};

	struct FSceneViewVisibilityStatistics
	{
		uint64 SubmittedPrimitives = 0;
		uint64 VisiblePrimitives = 0;

		auto operator==(const FSceneViewVisibilityStatistics&) const
			-> bool = default;
	};

	struct FSceneViewSummaryStatistics
	{
		uint64 Triangles = 0;
		uint64 DrawCalls = 0;

		auto operator==(const FSceneViewSummaryStatistics&) const -> bool = default;
	};

	struct FSceneViewGeometryStatistics
	{
		uint64 Primitives = 0;
		uint64 Triangles = 0;

		auto operator==(const FSceneViewGeometryStatistics&) const -> bool = default;
	};

	struct FSceneViewMeshStatistics
	{
		uint64 Primitives = 0;
		uint64 Triangles = 0;
		uint64 DrawCalls = 0;

		auto operator==(const FSceneViewMeshStatistics&) const -> bool = default;
	};

	struct FSceneViewShadowStatistics
	{
		uint64 Triangles = 0;
		uint64 DrawCalls = 0;
		uint32 Cascades = 0;
		bool bEnabled = false;
		bool bContactEnabled = false;
		EContactShadowExecutionRoute ContactRoute =
			EContactShadowExecutionRoute::None;

		auto operator==(const FSceneViewShadowStatistics&) const -> bool = default;
	};

	struct FSceneViewLightStatistics
	{
		uint64 Directional = 0;
		uint64 Point = 0;
		uint64 Spot = 0;

		auto operator==(const FSceneViewLightStatistics&) const -> bool = default;
	};

	struct FSceneViewVolumetricCloudStatistics
	{
		EVolumetricCloudQuality Quality = EVolumetricCloudQuality::High;
		EVolumetricCloudDebugMode DebugMode = EVolumetricCloudDebugMode::Lit;
		EVolumetricCloudExecutionRoute Route = EVolumetricCloudExecutionRoute::None;
		EVolumetricCloudRouteReason Reason =
			EVolumetricCloudRouteReason::DisabledOrUnneeded;
		EVolumetricCloudExecutionRoute ShadowRoute =
			EVolumetricCloudExecutionRoute::None;
		uint32 TargetWidth = 0;
		uint32 TargetHeight = 0;
		uint32 OutputWidth = 0;
		uint32 OutputHeight = 0;
		uint64 PrimarySamples = 0;
		uint64 LightSamples = 0;
		uint64 ShadowSamples = 0;
		uint64 ActiveBytes = 0;
		uint64 RetainedBytes = 0;
		uint64 HistoryBytes = 0;
		uint64 ShadowActiveBytes = 0;
		uint64 ShadowRetainedBytes = 0;
		bool bEnabled = false;
		bool bHistoryAvailable = false;
		bool bHistoryAccepted = false;
		bool bGPUTimingAvailable = false;

		auto operator==(const FSceneViewVolumetricCloudStatistics&) const
			-> bool = default;
	};
	static_assert(sizeof(FSceneViewVolumetricCloudStatistics) <= 160);

	// Carries the stable, bounded summary produced by one complete scene-view render.
	struct FSceneViewStatistics
	{
		FSceneViewVisibilityStatistics Visibility;
		// Headline totals cover the complete scene-view invocation and need not
		// equal the sum of the feature-owned breakdowns.
		FSceneViewSummaryStatistics Summary;
		FSceneViewMeshStatistics StaticMesh;
		// Spline draws share the static-mesh route and have no independent count.
		FSceneViewGeometryStatistics SplineMesh;
		FSceneViewShadowStatistics Shadow;
		FSceneViewLightStatistics Lights;
		FSceneViewVolumetricCloudStatistics VolumetricCloud;

		auto operator==(const FSceneViewStatistics&) const -> bool = default;
	};
} // namespace Durin
