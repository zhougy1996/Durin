#pragma once

#include "EngineAPI.h"

namespace Durin
{
	struct FStaticMeshLODResources;

	inline constexpr uint32 InvalidStaticMeshLODIndex =
		std::numeric_limits<uint32>::max();

	// Value-only selection facts; no resource or proxy lifetime is borrowed.
	struct FMeshLODSelectionEntry
	{
		float ScreenSize = 0.0f;
		bool bReadyForRendering = false;
	};
	using FMeshLODSelectionSnapshot = std::vector<FMeshLODSelectionEntry>;
	ENGINE_API auto CaptureStaticMeshLODSelection(
		std::span<const FStaticMeshLODResources> LODResources) -> FMeshLODSelectionSnapshot;
	ENGINE_API auto SelectStaticMeshLOD(float NormalizedScreenSize,
		std::span<const FMeshLODSelectionEntry> LODs) -> uint32;
	ENGINE_API auto ResolveAvailableStaticMeshLOD(uint32 RequestedLOD,
		std::span<const FMeshLODSelectionEntry> LODs) -> uint32;

	// Selects the first threshold met by the size, so exact equality chooses the higher-detail LOD.
	ENGINE_API auto SelectStaticMeshLOD(
		float NormalizedScreenSize,
		std::span<const FStaticMeshLODResources> LODResources) -> uint32;

	// Prefers ready lower-detail LODs before searching back toward higher detail.
	ENGINE_API auto ResolveAvailableStaticMeshLOD(
		uint32 RequestedLOD,
		std::span<const FStaticMeshLODResources> LODResources) -> uint32;
} // namespace Durin
