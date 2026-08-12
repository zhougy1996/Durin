#pragma once

#include "EngineAPI.h"
#include "Math/Box.h"
#include "Materials/MaterialRenderProxy.h"

namespace Durin
{
	// Stores renderer-owned state detached from the game-thread primitive component.
	enum class EPrimitiveSceneProxyKind : uint8
	{
		StaticMesh,
		SkeletalMesh,
		Terrain,
		SplineMesh
	};

	// Defines the common render-thread interface for all primitive proxy types.
	class FPrimitiveSceneProxy
	{
	public:
		ENGINE_API virtual ~FPrimitiveSceneProxy() = default;
		virtual auto GetKind() const -> EPrimitiveSceneProxyKind = 0;
		virtual auto GetLocalBounds() const -> FBox = 0;
		virtual auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate&) -> bool { return false; }
	};
}
