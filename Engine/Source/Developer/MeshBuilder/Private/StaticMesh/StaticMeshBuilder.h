#pragma once

#include "CoreMinimal.h"

#include "StaticMesh/StaticMeshBuildTypes.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	// Builds detached CPU render data without asset access.
	class FStaticMeshBuilder
	{
	public:
		static auto Build(FStaticMeshRenderData& OutRenderData,
			const FStaticMeshBuildParameters& Parameters) -> bool;
	};
}
