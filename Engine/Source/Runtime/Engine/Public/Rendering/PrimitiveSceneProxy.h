#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "RHIFeatureLevel.h"
#include "Math/Box.h"
#include "Materials/MaterialRenderProxy.h"
#include "StaticMesh/StaticMeshLODSelection.h"
#include "Rendering/PrimitiveCustomData.h"
#include "Rendering/PrimitiveSceneTransform.h"
#include "Rendering/PrimitiveRenderParticipation.h"

namespace Durin
{
	struct FMeshCollectionContext;
	struct FStaticMeshBatchPublication;
	class FStaticMeshBatchCaptureContext;
	struct FInstanceSceneDataBuffers;
	class FMeshBatchCollector;
	// Stores renderer-owned state detached from the game-thread primitive component.
	enum class EPrimitiveSceneProxyKind : uint8
	{
		StaticMesh,
		SplineMesh,
		Generic
	};

	// Defines the common render-thread interface for all primitive proxy types.
	class FPrimitiveSceneProxy
	{
	public:
		auto GetLocalToWorld() const -> const FMatrix& { return Transform.LocalToWorld; }
		auto GetPrimitiveSceneTransform() const -> const FPrimitiveSceneTransform& { return Transform; }
		auto GetFeatureLevel() const -> ERHIFeatureLevel { return FeatureLevel; }
		auto SetFeatureLevel(ERHIFeatureLevel Value) -> void { FeatureLevel = Value; }
		auto GetRenderParticipation() const -> const FPrimitiveRenderParticipation& { return RenderParticipation; }
		auto SetRenderParticipation(FPrimitiveRenderParticipation Value) -> void { RenderParticipation = Value; }
		ENGINE_API virtual ~FPrimitiveSceneProxy() = default;
		auto GetCustomPrimitiveData() const -> const FPrimitiveCustomData& { return CustomPrimitiveData; }
		auto SetCustomPrimitiveData(const FPrimitiveCustomData& Value) -> bool
		{
			if (!Value.IsValid()) return false;
			CustomPrimitiveData = Value;
			return true;
		}
		// Materials used by retained batches; Scene tracks structural updates through this list.
		virtual auto GetUsedMaterials() const -> std::span<const FMaterialRenderProxyRef> { return {}; }
		virtual auto GetKind() const -> EPrimitiveSceneProxyKind { return EPrimitiveSceneProxyKind::Generic; }
		virtual auto GetLocalBounds() const -> FBox = 0;
		virtual auto CanBeOccluded() const -> bool { return false; }
		// Null describes the ordinary single instance; an empty snapshot draws none.
		virtual auto GetInstanceSceneDataBuffers() const -> std::shared_ptr<const FInstanceSceneDataBuffers> { return {}; }
		// Optional value snapshot for threshold-based LOD preparation. Capture and
		// collection belong to one render command, without intervening scene mutation.
		virtual auto CaptureLODSelection_RenderThread() const -> std::optional<FMeshLODSelectionSnapshot> { return {}; }
		// Publish reusable inputs for all LODs. Return empty to collect batches per view.
		virtual auto DrawStaticElements(FStaticMeshBatchCaptureContext&) const
			-> std::shared_ptr<const FStaticMeshBatchPublication> { return {}; }
		// Called on the rendering thread; an empty default emits no geometry.
		virtual auto CollectMeshBatches(
			const FMeshCollectionContext&, FMeshBatchCollector&) const -> void {}
		virtual auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate&) -> bool { return false; }
	private:
		friend class FPrimitiveSceneInfo;
		auto SetTransform(const FMatrix& LocalToWorld) -> void
		{
			if (LocalToWorld != Transform.LocalToWorld)
				Transform = FPrimitiveSceneTransform(LocalToWorld);
		}
		FPrimitiveSceneTransform Transform{FMatrix(1.0)};
		FPrimitiveCustomData CustomPrimitiveData;
		FPrimitiveRenderParticipation RenderParticipation;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
	};
}
