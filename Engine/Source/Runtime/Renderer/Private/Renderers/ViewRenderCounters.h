#pragma once

#include "RendererAPI.h"
#include "Renderers/VolumetricCloudSpatialRenderer.h"
#include "Renderers/VolumetricCloudShadowRenderer.h"

#include "SceneView.h"
#include "ViewRenderStatistics.h"

#include <array>
#include <cstddef>
#include <vector>

namespace Durin
{
	struct FVisibilityRenderCounters
	{
		size_t SubmittedPrimitives = 0;
		size_t HiddenPrimitives = 0;
		size_t FrustumCulledPrimitives = 0;
		size_t VisiblePrimitives = 0;
		size_t InvalidBoundsFallbacks = 0;
		size_t InvalidViewFallbacks = 0;
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

	struct FStaticMeshRenderCounters
	{
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

	struct FSplineMeshRenderCounters
	{
		size_t VisibleSplineMeshCandidates = 0;
		size_t PreparedSplineMeshPrimitives = 0;
		size_t RejectedSplineMeshPrimitives = 0;
		size_t PreparedSplineMeshSections = 0;
		size_t PreparedSplineMeshTriangles = 0;
		size_t RetainedSplineMeshDeformationBytes = 0;
		size_t AcceptedSplineMeshDynamicUpdates = 0;
	};

	struct FSkeletalMeshRenderCounters
	{
		size_t VisibleSkeletalMeshCandidates = 0;
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

	struct FTerrainRenderCounters
	{
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
	};

	struct FLightingRenderCounters
	{
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
	};

	struct FDirectionalShadowRenderCounters
	{
		size_t ShadowSelectedLights = 0;
		EDirectionalShadowCandidate ShadowCandidate =
			EDirectionalShadowCandidate::SingleMap;
		size_t ShadowCascadeCount = 0;
		std::array<FDirectionalShadowCascadeCounters, 3> ShadowCascades{};
		size_t ShadowValidReceiverViews = 0;
		size_t ShadowInvalidReceiverViews = 0;
		size_t ShadowSubmittedCasters = 0;
		size_t ShadowHiddenCasters = 0;
		size_t ShadowCulledCasters = 0;
		size_t ShadowInvalidBoundsFallbacks = 0;
		size_t ShadowSceneTraversals = 0;
		size_t ShadowUniqueSubmittedCasters = 0;
		size_t ShadowUniqueHiddenCasters = 0;
		size_t ShadowUniqueEligibleStaticMeshCasters = 0;
		size_t ShadowUniqueEligibleSplineMeshCasters = 0;
		size_t ShadowUniqueEligibleSkeletalMeshCasters = 0;
		size_t ShadowUniqueEligibleTerrainCasters = 0;
		size_t ShadowCascadeClassificationTests = 0;
		size_t ShadowMembershipPopcount = 0;
		size_t ShadowTemporaryBytes = 0;
		size_t ShadowStaticSplinePrimitiveFactBuilds = 0;
		size_t ShadowStaticSplinePrimitiveFactReuses = 0;
		size_t ShadowSelectedLODFactBuilds = 0;
		size_t ShadowSelectedLODFactReuses = 0;
		size_t ShadowStaticSplineSectionFactBuilds = 0;
		size_t ShadowStaticSplineSectionFactReuses = 0;
		size_t ShadowSkeletalPrimitiveFactBuilds = 0;
		size_t ShadowSkeletalPrimitiveFactReuses = 0;
		size_t ShadowSkeletalSectionFactBuilds = 0;
		size_t ShadowSkeletalSectionFactReuses = 0;
		size_t ShadowTerrainPrimitiveFactBuilds = 0;
		size_t ShadowTerrainPrimitiveFactReuses = 0;
		size_t ShadowTerrainPatchFactBuilds = 0;
		size_t ShadowTerrainPatchFactReuses = 0;
		size_t ShadowTerrainPatchClassificationTests = 0;
		uint64 ShadowDiscoveryMembershipNanoseconds = 0;
		uint64 ShadowStaticSplinePreparationNanoseconds = 0;
		uint64 ShadowSkeletalPreparationNanoseconds = 0;
		uint64 ShadowTerrainLogicalPreparationNanoseconds = 0;
		uint64 ShadowSortingBatchingNanoseconds = 0;
		uint64 ShadowLogicalPreparationNanoseconds = 0;
		uint64 ShadowResourcePreparationNanoseconds = 0;
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
	};

	struct FContactShadowRenderCounters
	{
		size_t ContactShadowEnabledViews = 0;
		size_t ContactShadowPassFailures = 0;
		size_t ContactShadowComputeViews = 0;
		size_t ContactShadowFragmentViews = 0;
		size_t ContactShadowFactorOneViews = 0;
		size_t ContactShadowDispatches = 0;
		size_t ContactShadowDraws = 0;
		size_t ContactShadowActiveBytes = 0;
		size_t ContactShadowRetainedBytes = 0;
		std::array<size_t, 8> ContactShadowRouteReasons{};
	};

	struct FGBufferRenderCounters
	{
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
	};

	struct FDeferredRenderCounters
	{
		size_t DeferredDirectionalEnabledViews = 0;
		size_t DeferredDirectionalUnavailableViews = 0;
		size_t DeferredDirectionalPassFailures = 0;
		size_t DeferredDirectionalDebugViews = 0;
		size_t DeferredDirectionalOutputBytes = 0;
		size_t HybridDeferredEnabledViews = 0;
		size_t HybridDeferredUnavailableViews = 0;
	};

	struct FAmbientOcclusionRenderCounters
	{
		size_t GroundTruthAmbientOcclusionAttemptedViews = 0;
		size_t GroundTruthAmbientOcclusionEnabledViews = 0;
		size_t GroundTruthAmbientOcclusionHalfResolutionViews = 0;
		size_t GroundTruthAmbientOcclusionFullResolutionViews = 0;
		size_t GroundTruthAmbientOcclusionUnavailableViews = 0;
		size_t GroundTruthAmbientOcclusionRawPassFailures = 0;
		size_t GroundTruthAmbientOcclusionFilterPassFailures = 0;
		size_t GroundTruthAmbientOcclusionResolvePassFailures = 0;
		size_t GroundTruthAmbientOcclusionDebugViews = 0;
		size_t GroundTruthAmbientOcclusionActiveBytes = 0;
		size_t GroundTruthAmbientOcclusionRetainedBytes = 0;
	};

	struct FVolumetricCloudRenderCounters
	{
		size_t VolumetricCloudEnabledViews = 0;
		size_t VolumetricCloudComputeViews = 0;
		size_t VolumetricCloudFragmentViews = 0;
		size_t VolumetricCloudDisabledViews = 0;
		size_t VolumetricCloudDispatches = 0;
		size_t VolumetricCloudDraws = 0;
		size_t VolumetricCloudCompositeDraws = 0;
		uint64 VolumetricCloudPrimarySamples = 0;
		uint64 VolumetricCloudLightSamples = 0;
		EVolumetricCloudQuality VolumetricCloudQuality =
			EVolumetricCloudQuality::High;
		EVolumetricCloudDebugMode VolumetricCloudDebugMode =
			EVolumetricCloudDebugMode::Lit;
		uint32 VolumetricCloudTargetWidth = 0;
		uint32 VolumetricCloudTargetHeight = 0;
		uint32 VolumetricCloudOutputWidth = 0;
		uint32 VolumetricCloudOutputHeight = 0;
		uint64 VolumetricCloudActiveBytes = 0;
		uint64 VolumetricCloudRetainedBytes = 0;
		uint64 VolumetricCloudHistoryBytes = 0;
		uint64 VolumetricCloudShadowActiveBytes = 0;
		uint64 VolumetricCloudShadowRetainedBytes = 0;
		uint64 VolumetricCloudShadowSamples = 0;
		size_t VolumetricCloudShadowEnabledViews = 0;
		size_t VolumetricCloudShadowComputeViews = 0;
		size_t VolumetricCloudShadowFragmentViews = 0;
		size_t VolumetricCloudShadowDispatches = 0;
		size_t VolumetricCloudShadowDraws = 0;
		size_t VolumetricCloudShadowFactorOneViews = 0;
		std::array<size_t, static_cast<size_t>(FVolumetricCloudShadowRenderer::ERouteReason::Count)>
			VolumetricCloudShadowRouteReasons{};
		size_t VolumetricCloudTemporalDraws = 0;
		size_t VolumetricCloudHistoryAccepted = 0;
		size_t VolumetricCloudHistoryRejected = 0;
		std::array<size_t, static_cast<size_t>(FVolumetricCloudSpatialRenderer::ERouteReason::Count)>
			VolumetricCloudRouteReasons{};
	};

	struct FViewRenderCounters
	{
		FVisibilityRenderCounters Visibility;
		FStaticMeshRenderCounters StaticMesh;
		FSplineMeshRenderCounters SplineMesh;
		FSkeletalMeshRenderCounters SkeletalMesh;
		FTerrainRenderCounters Terrain;
		FLightingRenderCounters Lighting;
		FDirectionalShadowRenderCounters DirectionalShadow;
		FContactShadowRenderCounters ContactShadow;
		FGBufferRenderCounters GBuffer;
		FDeferredRenderCounters Deferred;
		FAmbientOcclusionRenderCounters AmbientOcclusion;
		FVolumetricCloudRenderCounters VolumetricCloud;
		size_t CombinedTranslucentGeometryDraws = 0;
	};

	using FViewRenderCounterSink = void (*)(const FViewRenderCounters&);

	// Development/profiling seam. The command-local snapshot is delivered once
	// per successful RenderView invocation and is never retained by Renderer.
	RENDERER_API auto SetViewRenderCounterSink(FViewRenderCounterSink Sink) -> void;
	RENDERER_API auto EmitViewRenderCounterSnapshot(
		const FViewRenderCounters& Counters
	) -> void;
	// Reduces Renderer-private counters to the stable editor-facing summary.
	RENDERER_API auto BuildSceneViewStatistics(
		const FViewRenderCounters& Counters
	) -> FSceneViewStatistics;
} // namespace Durin
