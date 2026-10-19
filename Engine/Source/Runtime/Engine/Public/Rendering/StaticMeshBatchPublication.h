#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/MeshGeometryRecord.h"
#include "StaticMesh/StaticMeshLODSelection.h"

namespace Durin
{
	class FStaticMeshSceneProxy;
	struct FMeshCollectionContext;
	class FMeshBatchCollector;

	// Immutable asset inputs shared by every ordinary StaticMesh proxy and view.
	// These owners retain checked geometry and RHI inputs, never an asset pointer.
	struct FStaticMeshBatchGeometry
	{
		FMeshLODSelectionSnapshot LODs;
		std::vector<FMeshGeometryRecord::FRef> Geometry;
		bool bValidLayout = false;
	};

	// Stable per-LOD inputs. Transform, custom data, view and selected LOD are
	// applied separately when this publication is selected for a frame.
	struct FStaticMeshBatchInputs
	{
		FMeshGeometryRecord::FRef Geometry;
		std::vector<FMaterialRenderData> Materials;
	};

	struct FStaticMeshBatchPublication
	{
		using FRef = std::shared_ptr<const FStaticMeshBatchPublication>;
		std::shared_ptr<const FStaticMeshBatchGeometry> Geometry;
		std::vector<std::shared_ptr<const FStaticMeshBatchInputs>> LODs;
		std::vector<uint64> MaterialRecords;
		uint64 FallbackMaterialRecord = 0;
		bool bUsesFallbackMaterial = false;
	};

	// Render-command-local capture: no scene/material mutations may intervene
	// between uses. Only immutable publication owners may escape to worker jobs.
	class FStaticMeshBatchCaptureContext final
	{
		friend class FStaticMeshSceneProxy;
		ENGINE_API auto ResolveMaterial(const FMaterialRenderProxyRef& Proxy) -> const FMaterialRenderData&;
		std::unordered_map<const FStaticMeshSceneProxy*, FStaticMeshBatchPublication::FRef> Publications;
		std::unordered_map<const FMaterialRenderProxy*, FMaterialRenderData> Materials;
	};

	// Selects stable inputs and admits a fresh instance overlay through the same
	// validation boundary used by dynamic providers.
	ENGINE_API auto CollectPublishedStaticMeshBatch(const FStaticMeshBatchPublication& Publication,
		const FMeshCollectionContext& Context, FMeshBatchCollector& Collector) -> void;
}
