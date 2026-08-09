#pragma once

#include "RendererAPI.h"

#include "Math/Box.h"
#include "SceneView.h"

namespace Durin
{
	inline constexpr uint32 InvalidStaticMeshLODIndex =
		std::numeric_limits<uint32>::max();

	// Identifies how a finite world AABB relates to all six view-frustum planes.
	enum class EViewBoundsClassification : uint8
	{
		Inside,
		Intersecting,
		Outside,
		InvalidBounds,
	};

	// Explains whether projected size came from ordinary finite projection or a conservative fallback.
	enum class EProjectedScreenSizeStatus : uint8
	{
		Valid,
		NearPlaneOrCameraCrossing,
		InvalidBounds,
		InvalidView,
	};

	// Stores normalized inward-facing world-space planes for one immutable view.
	struct FViewFrustum
	{
		std::array<FVector4, 6> Planes;
	};

	// Carries the normalized projected diameter and the reason for any maximum-detail fallback.
	struct FProjectedScreenSizeResult
	{
		float NormalizedScreenSize = 1.0f;
		EProjectedScreenSizeStatus Status =
			EProjectedScreenSizeStatus::InvalidView;
	};

	// Extracts Vulkan-depth clip planes from a finite world-to-clip matrix transactionally.
	RENDERER_API auto TryBuildViewFrustum(
		const FMatrix& ViewProjectionMatrix,
		FViewFrustum& OutFrustum) -> bool;

	// Rejects non-finite or unsupported projection snapshots before extraction.
	RENDERER_API auto TryBuildViewFrustum(
		const FSceneView& View,
		FViewFrustum& OutFrustum) -> bool;

	// Classifies a world AABB conservatively; exact and epsilon-scale plane contact remains visible.
	RENDERER_API auto ClassifyWorldBounds(
		const FViewFrustum& Frustum,
		const FBox& WorldBounds) -> EViewBoundsClassification;

	// Measures projected AABB diameter relative to the fitted content viewport's smaller dimension.
	RENDERER_API auto ComputeProjectedScreenSize(
		const FSceneView& View,
		const FBox& WorldBounds) -> FProjectedScreenSizeResult;

	// Builds strictly descending per-LOD thresholds with an exact zero final fallback.
	RENDERER_API auto MakeDefaultStaticMeshLODScreenSizes(uint32 NumLODs)
		-> std::vector<float>;

	RENDERER_API auto ValidateStaticMeshLODScreenSizes(
		std::span<const float> ScreenSizes) -> bool;

	// Selects the first threshold met by the size, so exact equality chooses the higher-detail LOD.
	RENDERER_API auto SelectStaticMeshLOD(
		float NormalizedScreenSize,
		std::span<const float> ScreenSizes) -> uint32;

	// Prefers ready lower-detail LODs before searching back toward higher detail.
	RENDERER_API auto ResolveAvailableStaticMeshLOD(
		uint32 RequestedLOD,
		std::span<const uint8> ReadyLODs) -> uint32;
} // namespace Durin
