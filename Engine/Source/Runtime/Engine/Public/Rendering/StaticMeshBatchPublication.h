#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/MeshGeometryRecord.h"
#include "StaticMesh/StaticMeshLODSelection.h"

namespace Durin
{
	// A submitted batch captures one geometry segment and one material.
	struct FStaticMeshBatchInputs
	{
		FMeshGeometryRecord::FRef Geometry;
		uint32 SegmentIndex = 0;
		FMaterialRenderData Material;
	};

	// Material values are frozen for one render command and shared across batches.
	class FStaticMeshBatchCaptureContext final
	{
		std::unordered_map<const FMaterialRenderProxy*, FMaterialRenderData> Materials;
	public:
		ENGINE_API auto ResolveMaterial(const FMaterialRenderProxyRef& Proxy) -> const FMaterialRenderData&;
	};
}
