#pragma once

#include "RendererAPI.h"
#include "Renderers/ViewRenderTelemetry.h"

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

	// Candidate lists and optional diagnostics; storage can be reused between views.
	struct FSceneVisibilityResult
	{
		std::vector<FPrimitiveVisibilityRecord> PrimitiveRecords;
		std::vector<const FPrimitiveSceneInfo*> SceneInfos;
	};

	// Replaces the result while retaining capacity. Records are opt-in; pointers
	// borrow scene storage and must be consumed before the scene is mutated.
	RENDERER_API auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderTelemetry& Telemetry,
		FSceneVisibilityResult& Result,
		bool bCollectPrimitiveRecords = false
	) -> void;

	// Convenience entry point for callers that do not retain scratch storage.
	RENDERER_API auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderTelemetry& Telemetry
	) -> FSceneVisibilityResult;
} // namespace Durin
