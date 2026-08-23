#pragma once

#include "RendererAPI.h"
#include "Renderers/ViewRenderCounters.h"

#include "SceneView.h"

#include <vector>

namespace Durin
{
	class FPrimitiveSceneInfo;
	class FScene;

	enum class EPrimitiveVisibilityClassification : uint8
	{
		Invalid,
		Hidden,
		VisibleInside,
		VisibleIntersecting,
		VisibleCullingDisabled,
		VisibleInvalidBoundsFallback,
		VisibleInvalidViewFallback,
		FrustumCulled,
	};

	struct FPrimitiveVisibilityRecord
	{
		const FPrimitiveSceneInfo* SceneInfo = nullptr;
		EPrimitiveVisibilityClassification Classification =
			EPrimitiveVisibilityClassification::Invalid;
	};

	struct FSceneVisibilityResult
	{
		std::vector<FPrimitiveVisibilityRecord> PrimitiveRecords;
		std::vector<const FPrimitiveSceneInfo*> StaticMeshSceneInfos;
		std::vector<const FPrimitiveSceneInfo*> SkeletalMeshSceneInfos;
		std::vector<const FPrimitiveSceneInfo*> TerrainSceneInfos;
		std::vector<const FPrimitiveSceneInfo*> SplineMeshSceneInfos;
	};

	// Classifies every authoritative live primitive once for one immutable view.
	RENDERER_API auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderCounters& Counters
	) -> FSceneVisibilityResult;
} // namespace Durin
