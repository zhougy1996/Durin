#pragma once

#include "RendererAPI.h"

#include "SceneView.h"
#include "ViewRenderStatistics.h"

#include <array>
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

	// Reconciles one cascade's independent caster, draw, and fitting outcomes.
	struct FDirectionalShadowCascadeCounters
	{
		double NearDepth = 0.0;
		double FarDepth = 0.0;
		double TransitionStartDepth = 0.0;
		double TexelWorldSizeX = 0.0;
		double TexelWorldSizeY = 0.0;
		size_t SubmittedCasters = 0;
		size_t HiddenCasters = 0;
		size_t CulledCasters = 0;
		size_t InvalidBoundsFallbacks = 0;
		size_t PreparedStaticMeshCasters = 0;
		size_t PreparedSplineMeshCasters = 0;
		size_t PreparedSkeletalMeshCasters = 0;
		size_t PreparedTerrainCasters = 0;
		size_t PreparedTriangles = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t ComparisonOperations = 0;
		size_t GuardTexels = 0;
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
		size_t InnerTerrainPatches = 0;
		size_t TransitionTerrainPatches = 0;
		size_t RadialRejectedTerrainPatches = 0;
		size_t InvalidTerrainDistanceSettingFallbacks = 0;
		size_t InvalidTerrainPatchBounds = 0;
		size_t TerrainLODFallbacks = 0;
		size_t TerrainLODResolutionFallbacks = 0;
		size_t TerrainAdjacencyPromotions = 0;
		size_t TerrainAdjacencyIterations = 0;
		std::vector<size_t> RequestedTerrainLODHistogram;
		std::vector<size_t> ResolvedTerrainLODHistogram;
		std::array<size_t, 16> TerrainStitchMaskHistogram{};
		size_t PreparedTerrainTriangles = 0;
		size_t OpaqueTerrainPatches = 0;
		size_t MaskedTerrainPatches = 0;
		size_t TranslucentTerrainPatches = 0;
		size_t TerrainResourceAttemptedDraws = 0;
		size_t TerrainResourceSuccessfulDraws = 0;
		size_t TerrainResourceRejectedDraws = 0;
		size_t PreparedTerrainBatches = 0;
		size_t TerrainBatchChunks = 0;
		size_t TerrainInstances = 0;
		size_t TerrainInstanceBytes = 0;
		size_t TerrainInstanceAllocations = 0;
		size_t TerrainResourceAttemptedBatches = 0;
		size_t TerrainResourceSuccessfulBatches = 0;
		size_t TerrainResourceRejectedBatches = 0;
		size_t TerrainSubmittedLogicalPatches = 0;
		size_t TerrainScalarTranslucentDraws = 0;
		uint64 TerrainLogicalPreparationNanoseconds = 0;
		uint64 TerrainBatchConstructionNanoseconds = 0;
		uint64 TerrainResourcePreparationNanoseconds = 0;
		uint64 TerrainHeightPreparationNanoseconds = 0;
		uint64 TerrainTopologyPreparationNanoseconds = 0;
		uint64 TerrainShaderPreparationNanoseconds = 0;
		uint64 TerrainPipelinePreparationNanoseconds = 0;
		uint64 TerrainDynamicAllocationNanoseconds = 0;
		uint64 TerrainCommandRecordingNanoseconds = 0;
		size_t TerrainAttemptedDraws = 0;
		size_t TerrainSuccessfulDraws = 0;
		size_t TerrainRejectedDraws = 0;
		size_t TerrainHeightUploadBytes = 0;
		size_t TerrainHeightUploads = 0;
		size_t TerrainHeightReuses = 0;
		size_t TerrainTopologyCreations = 0;
		size_t TerrainTopologyReuses = 0;
		size_t TerrainTopologyBytes = 0;
		size_t TerrainShaderLookups = 0;
		size_t TerrainShaderCreations = 0;
		size_t TerrainShaderReuses = 0;
		size_t TerrainPipelineLookups = 0;
		size_t TerrainPipelineCreations = 0;
		size_t TerrainPipelineReuses = 0;
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
		EDirectionalShadowCandidate ShadowCandidate =
			EDirectionalShadowCandidate::SingleMap;
		size_t ShadowCascadeCount = 0;
		std::array<FDirectionalShadowCascadeCounters, 3>
			ShadowCascades{};
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
		size_t ShadowPreparedTriangles = 0;
		size_t ShadowResourceAttempts = 0;
		size_t ShadowResourceSuccesses = 0;
		size_t ShadowResourceFailures = 0;
		size_t ShadowPreparationFailures = 0;
		size_t ShadowTargetLogicalBytes = 0;
		size_t ShadowTargetBackendBytes = 0;
		size_t ShadowAttemptedDraws = 0;
		size_t ShadowSuccessfulDraws = 0;
		size_t ShadowRejectedDraws = 0;
		std::array<size_t, static_cast<size_t>(EDirectionalShadowDiagnosticMode::Count)>
			ShadowDiagnosticViews{};
		std::array<size_t, static_cast<size_t>(EDirectionalShadowFilterQuality::Count)>
			ShadowQualityViews{};
		size_t ShadowComparisonOperations = 0;
		size_t ShadowTransitionComparisonOperations = 0;
		size_t ShadowGuardTexels = 0;
		size_t ShadowInvalidQualityFallbacks = 0;
		size_t ShadowBiasFallbacks = 0;
		size_t ShadowBiasClamps = 0;
		size_t ContactShadowEnabledViews = 0;
		size_t ContactShadowPassFailures = 0;
		size_t GBufferEnabledViews = 0;
		size_t GBufferUnavailableViews = 0;
		size_t GBufferDebugViews = 0;
		size_t GBufferDebugFailures = 0;
		size_t GBufferAttachmentBytes = 0;
		size_t GBufferAttemptedDraws = 0;
		size_t GBufferSuccessfulDraws = 0;
		size_t GBufferRejectedDraws = 0;
		size_t GBufferSkippedDraws = 0;
		size_t GBufferStaticMeshAttemptedDraws = 0;
		size_t GBufferStaticMeshSuccessfulDraws = 0;
		size_t GBufferStaticMeshRejectedDraws = 0;
		size_t GBufferStaticMeshSkippedDraws = 0;
		size_t GBufferSplineMeshAttemptedDraws = 0;
		size_t GBufferSplineMeshSuccessfulDraws = 0;
		size_t GBufferSplineMeshRejectedDraws = 0;
		size_t GBufferSplineMeshSkippedDraws = 0;
		size_t GBufferSkeletalMeshAttemptedDraws = 0;
		size_t GBufferSkeletalMeshSuccessfulDraws = 0;
		size_t GBufferSkeletalMeshRejectedDraws = 0;
		size_t GBufferSkeletalMeshSkippedDraws = 0;
		size_t GBufferTerrainAttemptedDraws = 0;
		size_t GBufferTerrainSuccessfulDraws = 0;
		size_t GBufferTerrainRejectedDraws = 0;
		size_t GBufferTerrainSkippedDraws = 0;
		size_t DeferredDirectionalEnabledViews = 0;
		size_t DeferredDirectionalUnavailableViews = 0;
		size_t DeferredDirectionalPassFailures = 0;
		size_t DeferredDirectionalDebugViews = 0;
		size_t DeferredDirectionalOutputBytes = 0;
		size_t GroundTruthAmbientOcclusionAttemptedViews = 0;
		size_t GroundTruthAmbientOcclusionEnabledViews = 0;
		size_t GroundTruthAmbientOcclusionUnavailableViews = 0;
		size_t GroundTruthAmbientOcclusionRawPassFailures = 0;
		size_t GroundTruthAmbientOcclusionFilterPassFailures = 0;
		size_t GroundTruthAmbientOcclusionDebugViews = 0;
		size_t GroundTruthAmbientOcclusionActiveBytes = 0;
		size_t GroundTruthAmbientOcclusionRetainedBytes = 0;
		size_t HybridDeferredEnabledViews = 0;
		size_t HybridDeferredUnavailableViews = 0;
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
		const FViewRenderCounters& Counters
	) -> void;
	// Reduces Renderer-private counters to the stable editor-facing summary.
	RENDERER_API auto BuildSceneViewStatistics(
		const FViewRenderCounters& Counters
	) -> FSceneViewStatistics;

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
