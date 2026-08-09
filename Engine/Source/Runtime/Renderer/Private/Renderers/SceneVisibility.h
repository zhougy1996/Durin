#pragma once

#include "RendererAPI.h"

#include "SceneView.h"

#include <cstddef>
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

	struct FViewRenderCounters
	{
		size_t SubmittedPrimitives = 0;
		size_t HiddenPrimitives = 0;
		size_t FrustumCulledPrimitives = 0;
		size_t VisiblePrimitives = 0;
		size_t InvalidBoundsFallbacks = 0;
		size_t InvalidViewFallbacks = 0;
		size_t VisibleStaticMeshCandidates = 0;
		size_t PreparedStaticMeshPrimitives = 0;
		size_t RejectedStaticMeshPrimitives = 0;
		size_t PreparedStaticMeshSections = 0;
		size_t PreparedStaticMeshTriangles = 0;
		size_t StaticMeshProjectedSizeFallbacks = 0;
		size_t StaticMeshResourceFallbacks = 0;
		std::vector<size_t> RequestedStaticMeshLODHistogram;
		std::vector<size_t> SelectedStaticMeshLODHistogram;
		size_t OpaqueStaticMeshSections = 0;
		size_t MaskedStaticMeshSections = 0;
		size_t TranslucentStaticMeshSections = 0;
		size_t OpaqueStaticMeshTriangles = 0;
		size_t MaskedStaticMeshTriangles = 0;
		size_t TranslucentStaticMeshTriangles = 0;
		size_t OpaqueStaticMeshStateGroups = 0;
		size_t MaskedStaticMeshStateGroups = 0;
		size_t OpaqueStaticMeshInputStateGroups = 0;
		size_t MaskedStaticMeshInputStateGroups = 0;
		size_t StaticMeshPipelineTransitions = 0;
		size_t StaticMeshMaterialTransitions = 0;
		size_t StaticMeshVertexFactoryTransitions = 0;
		size_t StaticMeshGeometryTransitions = 0;
		size_t StaticMeshResourceAttemptedDraws = 0;
		size_t StaticMeshResourceSuccessfulDraws = 0;
		size_t StaticMeshResourceRejectedDraws = 0;
		size_t StaticMeshAttemptedDraws = 0;
		size_t StaticMeshSuccessfulDraws = 0;
		size_t StaticMeshRejectedDraws = 0;
	};

	using FViewRenderCounterSink = void (*)(const FViewRenderCounters&);

	// Development/profiling seam. The command-local snapshot is delivered once
	// per RenderView invocation and is never retained by Renderer.
	RENDERER_API auto SetViewRenderCounterSink(FViewRenderCounterSink Sink) -> void;
	RENDERER_API auto EmitViewRenderCounterSnapshot(
		const FViewRenderCounters& Counters) -> void;

	struct FSceneVisibilityResult
	{
		std::vector<FPrimitiveVisibilityRecord> PrimitiveRecords;
		std::vector<const FPrimitiveSceneInfo*> StaticMeshSceneInfos;
		std::vector<const FPrimitiveSceneInfo*> TextureCubePreviewSceneInfos;
	};

	// Classifies every authoritative live primitive once for one immutable view.
	RENDERER_API auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderCounters& Counters) -> FSceneVisibilityResult;
} // namespace Durin
