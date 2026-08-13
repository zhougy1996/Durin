#pragma once

#include "Engine/TerrainSceneProxy.h"
#include "RendererAPI.h"
#include "SceneView.h"

namespace Durin
{
	inline constexpr float TerrainScreenErrorPixels = 2.0f;
	inline constexpr uint32 InvalidTerrainLODIndex = std::numeric_limits<uint32>::max();

	struct FTerrainLODSelection
	{
		uint32 LODIndex = 0;
		bool bFallback = false;
	};

	struct FTerrainLODResolution
	{
		std::vector<uint32> ResolvedLODs;
		std::vector<uint8> StitchMasks;
		size_t Promotions = 0;
		size_t Iterations = 0;
		bool bValid = false;
	};

	RENDERER_API auto SelectTerrainPatchLOD(
		const FSceneView& View, const FMatrix& LocalToWorld,
		const FTerrainPatchDescriptor& Patch) -> FTerrainLODSelection;

	RENDERER_API auto ResolveTerrainPatchAdjacency(
		std::span<const FTerrainPatchDescriptor> Patches,
		std::span<const uint32> RequestedLODs) -> FTerrainLODResolution;
}
