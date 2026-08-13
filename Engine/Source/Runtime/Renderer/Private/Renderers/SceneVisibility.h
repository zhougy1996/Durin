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
		size_t VisibleSkeletalMeshCandidates = 0;
		size_t VisibleTerrainCandidates = 0;
		size_t TerrainPatchCandidates = 0;
		size_t VisibleTerrainPatches = 0;
		size_t CulledTerrainPatches = 0;
		size_t InvalidTerrainPatchBounds = 0;
		size_t PreparedTerrainTriangles = 0;
		size_t OpaqueTerrainPatches = 0;
		size_t MaskedTerrainPatches = 0;
		size_t TranslucentTerrainPatches = 0;
		size_t TerrainResourceAttemptedDraws = 0;
		size_t TerrainResourceSuccessfulDraws = 0;
		size_t TerrainResourceRejectedDraws = 0;
		size_t TerrainAttemptedDraws = 0;
		size_t TerrainSuccessfulDraws = 0;
		size_t TerrainRejectedDraws = 0;
		size_t TerrainHeightUploadBytes = 0;
		size_t TerrainHeightUploads = 0;
		size_t TerrainHeightReuses = 0;
		size_t TerrainTopologyCreations = 0;
		size_t TerrainTopologyReuses = 0;
		size_t TerrainTopologyBytes = 0;
		size_t VisibleSplineMeshCandidates = 0;
		size_t PreparedSplineMeshPrimitives = 0;
		size_t RejectedSplineMeshPrimitives = 0;
		size_t PreparedSplineMeshSections = 0;
		size_t PreparedSplineMeshTriangles = 0;
		size_t RetainedSplineMeshDeformationBytes = 0;
		size_t AcceptedSplineMeshDynamicUpdates = 0;
		size_t PreparedStaticMeshPrimitives = 0;
		size_t RejectedStaticMeshPrimitives = 0;
		size_t PreparedStaticMeshSections = 0;
		size_t PreparedStaticMeshTriangles = 0;
		size_t StaticMeshProjectedSizeFallbacks = 0;
		size_t StaticMeshResourceFallbacks = 0;
		std::vector<size_t> RequestedStaticMeshLODHistogram;
		std::vector<size_t> SelectedStaticMeshLODHistogram;
		size_t SubmittedDirectionalLights = 0;
		size_t RejectedDirectionalLights = 0;
		size_t SelectedDirectionalLights = 0;
		size_t OverflowDirectionalLights = 0;
		size_t SubmittedPointLights = 0;
		size_t RejectedPointLights = 0;
		size_t FrustumCulledPointLights = 0;
		size_t SelectedPointLights = 0;
		size_t OverflowPointLights = 0;
		size_t SubmittedSpotLights = 0;
		size_t RejectedSpotLights = 0;
		size_t FrustumCulledSpotLights = 0;
		size_t SelectedSpotLights = 0;
		size_t OverflowSpotLights = 0;
		size_t PackedLightBytes = 0;
		size_t ShadowSelectedLights = 0;
		size_t ShadowValidReceiverViews = 0;
		size_t ShadowInvalidReceiverViews = 0;
		size_t ShadowSubmittedCasters = 0;
		size_t ShadowHiddenCasters = 0;
		size_t ShadowCulledCasters = 0;
		size_t ShadowInvalidBoundsFallbacks = 0;
		size_t ShadowPreparedStaticMeshCasters = 0;
		size_t ShadowPreparedSplineMeshCasters = 0;
		size_t ShadowPreparedSkeletalMeshCasters = 0;
		size_t ShadowPreparedTerrainCasters = 0;
		size_t ShadowResourceAttempts = 0;
		size_t ShadowResourceSuccesses = 0;
		size_t ShadowResourceFailures = 0;
		size_t ShadowPreparationFailures = 0;
		size_t ShadowTargetLogicalBytes = 0;
		size_t ShadowTargetBackendBytes = 0;
		size_t ShadowAttemptedDraws = 0;
		size_t ShadowSuccessfulDraws = 0;
		size_t ShadowRejectedDraws = 0;
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
		size_t PreparedSkeletalMeshPrimitives = 0;
		size_t RejectedSkeletalMeshPrimitives = 0;
		size_t PreparedSkeletalMeshSections = 0;
		size_t PreparedSkeletalMeshTriangles = 0;
		size_t OpaqueSkeletalMeshSections = 0;
		size_t MaskedSkeletalMeshSections = 0;
		size_t TranslucentSkeletalMeshSections = 0;
		size_t OpaqueSkeletalMeshTriangles = 0;
		size_t MaskedSkeletalMeshTriangles = 0;
		size_t TranslucentSkeletalMeshTriangles = 0;
		size_t OpaqueSkeletalMeshStateGroups = 0;
		size_t MaskedSkeletalMeshStateGroups = 0;
		size_t SkeletalMeshPipelineTransitions = 0;
		size_t SkeletalMeshMaterialTransitions = 0;
		size_t SkeletalMeshVertexFactoryTransitions = 0;
		size_t SkeletalMeshGeometryTransitions = 0;
		size_t CombinedTranslucentGeometryDraws = 0;
		size_t SkeletalMeshResourceAttemptedDraws = 0;
		size_t SkeletalMeshResourceSuccessfulDraws = 0;
		size_t SkeletalMeshResourceRejectedDraws = 0;
		size_t SkeletalMeshAttemptedDraws = 0;
		size_t SkeletalMeshSuccessfulDraws = 0;
		size_t SkeletalMeshRejectedDraws = 0;
		size_t RequestedSkeletalPaletteUploads = 0;
		size_t UploadedSkeletalPalettes = 0;
		size_t ReusedSkeletalPalettes = 0;
		size_t RejectedSkeletalPalettes = 0;
		size_t UploadedSkeletalPaletteMatrices = 0;
		size_t UploadedSkeletalPaletteBytes = 0;
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
		std::vector<const FPrimitiveSceneInfo*> SkeletalMeshSceneInfos;
		std::vector<const FPrimitiveSceneInfo*> TerrainSceneInfos;
		std::vector<const FPrimitiveSceneInfo*> SplineMeshSceneInfos;
	};

	// Classifies every authoritative live primitive once for one immutable view.
	RENDERER_API auto PrepareSceneVisibility(
		const FScene& Scene,
		const FSceneView& View,
		FViewRenderCounters& Counters) -> FSceneVisibilityResult;
} // namespace Durin
