#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Asset/AssetBuildTaskContext.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "StaticMesh/StaticMeshData.h"

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

	// Borrowed settings and controls for one synchronous detached build.
	struct FStaticMeshBuildParameters
	{
		FMeshDescriptionReadHandle Geometry;
		std::span<const FStaticMeshBuildMaterialSlot> MaterialSlots;
		float NormalizedSize = 1.5f;
		FAssetBuildTaskContext Control;
	};

}

#endif
