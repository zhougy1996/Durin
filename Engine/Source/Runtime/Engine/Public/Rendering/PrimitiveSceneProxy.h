#pragma once

#include "EngineAPI.h"
#include "Math/Box.h"
#include "Materials/MaterialRenderProxy.h"
#include "StaticMesh/StaticMeshLODSelection.h"

namespace Durin
{
	struct FMeshCollectionContext;
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
		ENGINE_API virtual ~FPrimitiveSceneProxy() = default;
		virtual auto GetKind() const -> EPrimitiveSceneProxyKind { return EPrimitiveSceneProxyKind::Generic; }
		virtual auto GetLocalBounds() const -> FBox = 0;
		// Optional value snapshot for threshold-based LOD preparation. Capture and
		// collection belong to one render command, without intervening scene mutation.
		virtual auto CaptureLODSelection_RenderThread() const -> std::optional<FMeshLODSelectionSnapshot> { return {}; }
		// Called on the rendering thread; an empty default emits no geometry.
		virtual auto CollectMeshBatches(
			const FMeshCollectionContext&, FMeshBatchCollector&) const -> void {}
		virtual auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate&) -> bool { return false; }
	};
}
