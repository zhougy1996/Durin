#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "MeshDescription/MeshDescription.h"
#endif

#if DURIN_WITH_EDITORONLY_DATA

namespace Durin
{
	// Fixed slot metadata only; material object bindings remain with the operation owner.
	struct FStaticMeshBuildMaterialSlot
	{
		FName Name;
		std::string SourceName;
		uint32 SourceMaterialIndex = 0;
	};

	// Owned worker settings contain no material object bindings or publication identity.
	struct FStaticMeshBuildSettings
	{
		std::vector<FStaticMeshBuildMaterialSlot> MaterialSlots;
		float NormalizedSize = 1.5f;
	};

	// Borrowed geometry/settings and memory budget for one synchronous detached build.
	struct FStaticMeshBuildParameters
	{
		FMeshDescriptionReadHandle Geometry;
		std::span<const FStaticMeshBuildMaterialSlot> MaterialSlots;
		float NormalizedSize = 1.5f;
		uint64 MaximumWorkingSetBytes = UINT64_MAX;
		std::span<const FMeshDescriptionReadHandle> LODGeometries;
		FMeshDescriptionReadHandle NormalizationGeometry;
	};

}

#endif
