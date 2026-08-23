#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "Renderers/MeshRenderPreparationCommon.h"
#include "Scene.h"
#include "SceneView.h"

namespace Durin
{
	inline constexpr size_t TerrainInstanceDataBytes = 48;

	struct FTerrainPatchDescriptor;

	struct FPreparedTerrainDraw
	{
		const FPrimitiveSceneInfo* SceneInfo = nullptr;
		const FTerrainPatchDescriptor* Patch = nullptr;
		FMaterialRenderData Material;
		FMaterialRenderBinding MaterialBinding;
		FEffectiveMeshPipelineKey PipelineKey;
		FMeshDrawSortKey SortKey;
		EMeshBasePass Pass = EMeshBasePass::Opaque;
		uint32 RequestedLOD = 0;
		uint32 ResolvedLOD = 0;
		uint32 LODStep = 1;
		uint8 StitchMask = 0;
		size_t TriangleCount = 0;
		double TranslucentDistanceSquared = 0.0;
		bool bResourcesReady = false;
		FRHITexture* DirectionalShadowTexture = nullptr;
		FRHISampler* DirectionalShadowSampler = nullptr;
	};

	// Execution-only grouping of compatible logical patches. DrawIndices retain
	// stable patch identity for diagnostics and overlays.
	struct FPreparedTerrainBatch
	{
		std::vector<uint32> DrawIndices;
		bool bResourcesReady = false;
	};

	enum class EPreparedTerrainPhase : uint8 { Prepared, ResourcesPrepared, Executed };

	struct FPreparedTerrainView
	{
		std::vector<FPreparedTerrainDraw> Opaque;
		std::vector<FPreparedTerrainDraw> Masked;
		std::vector<FPreparedTerrainDraw> Translucent;
		std::vector<FPreparedTerrainBatch> OpaqueBatches;
		std::vector<FPreparedTerrainBatch> MaskedBatches;
		size_t PatchCandidates = 0;
		size_t PatchClassificationTests = 0;
		size_t SharedPrimitiveFactBuilds = 0;
		size_t SharedPrimitiveFactReuses = 0;
		size_t SharedPatchFactBuilds = 0;
		size_t SharedPatchFactReuses = 0;
		size_t VisiblePatches = 0;
		size_t CulledPatches = 0;
		size_t InnerPatches = 0;
		size_t TransitionPatches = 0;
		size_t RadialRejectedPatches = 0;
		size_t InvalidDistanceSettingFallbacks = 0;
		size_t InvalidBoundsFallbacks = 0;
		size_t LODFallbacks = 0;
		size_t LODResolutionFallbacks = 0;
		size_t AdjacencyPromotions = 0;
		size_t AdjacencyIterations = 0;
		std::vector<size_t> RequestedLODHistogram;
		std::vector<size_t> ResolvedLODHistogram;
		std::array<size_t, 16> StitchMaskHistogram{};
		size_t Triangles = 0;
		size_t HeightUploadBytes = 0;
		size_t HeightUploads = 0;
		size_t HeightReuses = 0;
		size_t TopologyCreations = 0;
		size_t TopologyReuses = 0;
		size_t TopologyBytes = 0;
		size_t ShaderLookups = 0;
		size_t ShaderCreations = 0;
		size_t ShaderReuses = 0;
		size_t PipelineLookups = 0;
		size_t PipelineCreations = 0;
		size_t PipelineReuses = 0;
		size_t ResourceAttemptedDraws = 0;
		size_t ResourceSuccessfulDraws = 0;
		size_t ResourceRejectedDraws = 0;
		size_t PreparedBatches = 0;
		size_t BatchChunks = 0;
		size_t InstanceCount = 0;
		size_t InstanceBytes = 0;
		size_t InstanceAllocations = 0;
		size_t ResourceAttemptedBatches = 0;
		size_t ResourceSuccessfulBatches = 0;
		size_t ResourceRejectedBatches = 0;
		size_t SubmittedLogicalPatches = 0;
		size_t ScalarTranslucentDraws = 0;
		uint64 LogicalPreparationNanoseconds = 0;
		uint64 BatchConstructionNanoseconds = 0;
		uint64 ResourcePreparationNanoseconds = 0;
		uint64 HeightPreparationNanoseconds = 0;
		uint64 TopologyPreparationNanoseconds = 0;
		uint64 ShaderPreparationNanoseconds = 0;
		uint64 PipelinePreparationNanoseconds = 0;
		uint64 DynamicAllocationNanoseconds = 0;
		uint64 CommandRecordingNanoseconds = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t GBufferAttemptedDraws = 0;
		size_t GBufferSuccessfulDraws = 0;
		size_t GBufferRejectedDraws = 0;
		size_t GBufferSkippedDraws = 0;
		EPreparedTerrainPhase Phase = EPreparedTerrainPhase::Prepared;

		auto GetNumDraws() const -> size_t { return Opaque.size() + Masked.size() + Translucent.size(); }
		auto GetNumHardwareDraws() const -> size_t
		{
			return OpaqueBatches.size() + MaskedBatches.size() + Translucent.size();
		}
	};

	RENDERER_API auto PrepareTerrainView_RenderThread(
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View, ERasterMode RasterMode,
		ERenderPreparationMode Mode = ERenderPreparationMode::Full)
		-> FPreparedTerrainView;
} // namespace Durin
