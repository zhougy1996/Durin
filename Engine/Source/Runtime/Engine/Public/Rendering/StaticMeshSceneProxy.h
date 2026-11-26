#pragma once

#include "CoreMinimal.h"

#include "Rendering/PrimitiveSceneProxy.h"
#include "Rendering/StaticMeshBatchPublication.h"

namespace Durin
{
	struct FStaticMeshRenderData;
	struct FMeshBatch;

	// Couples static-mesh render resources with ordered per-slot material bindings.
	class FStaticMeshSceneProxy : public FPrimitiveSceneProxy
	{
	public:
		ENGINE_API explicit FStaticMeshSceneProxy(
			const FStaticMeshRenderData* InRenderData,
			std::vector<FMaterialRenderProxyRef> InMaterialProxies,
			FStaticMeshRenderInstances InInstances = {});
		ENGINE_API auto SetRenderInstances(FStaticMeshRenderInstances InInstances) -> bool;
		auto GetInstanceSceneDataBuffers() const -> std::shared_ptr<const FInstanceSceneDataBuffers> override { return InstanceSceneData; }

		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		ENGINE_API auto DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) const -> void override;
		auto GetKind() const -> EPrimitiveSceneProxyKind override { return EPrimitiveSceneProxyKind::StaticMesh; }
		ENGINE_API auto GetLocalBounds() const -> FBox override;
		ENGINE_API auto CanBeOccluded() const -> bool override;
		ENGINE_API auto CaptureLODSelection_RenderThread() const -> std::optional<FMeshLODSelectionSnapshot> override;
		auto GetUsedMaterials() const -> std::span<const FMaterialRenderProxyRef> override { return Materials; }

		ENGINE_API auto GetMaterialRenderProxy(uint32 SlotIndex) const
			-> const FMaterialRenderProxyRef&;
		ENGINE_API auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate& Update) -> bool override;

		ENGINE_API auto CollectMeshBatches(const FMeshCollectionContext& Context,
			FMeshBatchCollector& Collector) const -> void override;

	private:
		auto GetMeshElement(uint32 LODIndex, uint32 SectionIndex, FMeshBatch& OutMeshBatch) const -> void;
		auto GetShadowMeshElement(uint32 LODIndex, FMeshBatch& OutMeshBatch) const -> bool;

		// Non-owning borrow bounded by the component render-state lifetime. The
		// component removes this proxy before the asset retires the render data.
		const FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;
		FStaticMeshRenderInstances Instances;
		std::shared_ptr<const FInstanceSceneDataBuffers> InstanceSceneData;

	};
}
