#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Scene.h"

namespace Durin
{
	struct FTerrainPatchDescriptor;

	struct FPreparedTerrainDraw
	{
		const FPrimitiveSceneInfo* SceneInfo = nullptr;
		const FTerrainPatchDescriptor* Patch = nullptr;
		FMaterialRenderData Material;
		FMaterialRenderV3Binding MaterialBinding;
		FEffectiveStaticMeshPipelineKey PipelineKey;
		FStaticMeshDrawSortKey SortKey;
		EStaticMeshBasePass Pass = EStaticMeshBasePass::Opaque;
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

	enum class EPreparedTerrainPhase : uint8 { Prepared, ResourcesPrepared, Executed };

	struct FPreparedTerrainView
	{
		std::vector<FPreparedTerrainDraw> Opaque;
		std::vector<FPreparedTerrainDraw> Masked;
		std::vector<FPreparedTerrainDraw> Translucent;
		size_t PatchCandidates = 0;
		size_t VisiblePatches = 0;
		size_t CulledPatches = 0;
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
		size_t ResourceAttemptedDraws = 0;
		size_t ResourceSuccessfulDraws = 0;
		size_t ResourceRejectedDraws = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		EPreparedTerrainPhase Phase = EPreparedTerrainPhase::Prepared;

		auto GetNumDraws() const -> size_t { return Opaque.size() + Masked.size() + Translucent.size(); }
	};

	RENDERER_API auto PrepareTerrainView_RenderThread(
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View, ERasterMode RasterMode) -> FPreparedTerrainView;
} // namespace Durin
