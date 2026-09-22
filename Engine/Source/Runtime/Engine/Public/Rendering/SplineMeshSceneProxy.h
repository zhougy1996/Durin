#pragma once

#include "Rendering/PrimitiveSceneProxy.h"
#include "Spline/SplineTypes.h"

namespace Durin
{
	struct FStaticMeshRenderData;
	class FMeshGeometryRecord;
	class FVertexFactoryInputBinding;

	// Captures revisioned spline deformation state copied for render-thread use.
	struct FSplineMeshRenderDynamicData
	{
		FSplineMeshParams Params;
		FBox LocalBounds;
		uint64 Revision = 0;
	};

	// Couples static-mesh resources with revisioned spline deformation and material state.
	class FSplineMeshSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		ENGINE_API FSplineMeshSceneProxy(const FStaticMeshRenderData* InRenderData,
			std::vector<FMaterialRenderProxyRef> InMaterialProxies,
			FSplineMeshRenderDynamicData InDynamicData);
		auto GetKind() const -> EPrimitiveSceneProxyKind override { return EPrimitiveSceneProxyKind::SplineMesh; }
		auto GetRenderData() const -> const FStaticMeshRenderData* { return RenderData; }
		auto GetDynamicData() const -> const FSplineMeshRenderDynamicData& { return DynamicData; }
		auto GetAcceptedDynamicUpdateCount() const -> uint64 { return AcceptedDynamicUpdateCount; }
		auto GetLocalBounds() const -> FBox override { return DynamicData.LocalBounds; }
		ENGINE_API auto CaptureLODSelection_RenderThread() const -> std::optional<FMeshLODSelectionSnapshot> override;
		ENGINE_API auto GetMaterialRenderProxy(uint32 SlotIndex) const -> const FMaterialRenderProxyRef&;
		ENGINE_API auto ResolveMaterialRenderData_RenderThread(uint32 SlotIndex) const -> const FMaterialRenderData&;
		ENGINE_API auto UpdateMaterialBinding_RenderThread(const FMaterialRenderProxyBindingUpdate& Update) -> bool override;
		ENGINE_API auto UpdateDynamicData_RenderThread(FSplineMeshRenderDynamicData InDynamicData) -> bool;
		ENGINE_API auto ResolveGeometryRecord_RenderThread(uint32 LODIndex,
			const FMeshGeometryRecord& Geometry) const
			-> std::shared_ptr<const FMeshGeometryRecord>;

		ENGINE_API auto CollectMeshBatches(const FMeshCollectionContext& Context,
			FMeshBatchCollector& Collector) const -> void override;

	private:
		const FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;

		FSplineMeshRenderDynamicData DynamicData;
		uint64 AcceptedDynamicUpdateCount = 0;
		struct FCachedBinding
		{
			uint64 GeometryRecordId = 0;
			std::shared_ptr<const FMeshGeometryRecord> Record;
		};
		// Render-thread only; at most one current deformation binding per asset LOD.
		mutable std::vector<FCachedBinding> GeometryBindings;
	};
}
