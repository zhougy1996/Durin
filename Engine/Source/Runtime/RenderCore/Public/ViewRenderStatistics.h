#pragma once

#include "Misc/CoreTypes.h"

namespace Durin
{
	enum class EContactShadowExecutionRoute : uint8
	{
		None,
		Compute,
		Fragment,
	};

	// Carries the stable, bounded summary produced by one complete scene-view render.
	struct FSceneViewStatistics
	{
		uint64 SubmittedPrimitives = 0;
		uint64 VisiblePrimitives = 0;
		uint64 StaticMeshPrimitives = 0;
		uint64 SplineMeshPrimitives = 0;
		uint64 SkeletalMeshPrimitives = 0;
		uint64 VisibleTerrainPatches = 0;

		uint64 Triangles = 0;
		uint64 StaticMeshTriangles = 0;
		uint64 SplineMeshTriangles = 0;
		uint64 SkeletalMeshTriangles = 0;
		uint64 TerrainTriangles = 0;
		uint64 ShadowTriangles = 0;

		uint64 DrawCalls = 0;
		uint64 StaticMeshDrawCalls = 0;
		uint64 SkeletalMeshDrawCalls = 0;
		uint64 TerrainDrawCalls = 0;
		uint64 ShadowDrawCalls = 0;

		uint64 DirectionalLights = 0;
		uint64 PointLights = 0;
		uint64 SpotLights = 0;
		uint32 ShadowCascades = 0;
		bool bShadowEnabled = false;
		bool bContactShadowEnabled = false;
		EContactShadowExecutionRoute ContactShadowRoute =
			EContactShadowExecutionRoute::None;

		auto operator==(const FSceneViewStatistics&) const -> bool = default;
	};
} // namespace Durin
