#pragma once

#include "RendererAPI.h"

#include "Materials/MaterialRenderProxy.h"
#include "Engine/SplineMeshSceneProxy.h"
#include "RHIResources.h"
#include "Scene.h"
#include "SceneView.h"
#include "StaticMesh/StaticMeshResources.h"

#include <array>
#include <vector>

namespace Durin
{
	class FRHICommandListImmediate;
	enum class EStaticMeshBasePass : uint8
	{
		Opaque,
		Masked,
		Translucent,
	};

	enum class EVertexDeformationDomain : uint8
	{
		Local,
		Spline,
		Skeletal
	};

	struct FMeshShaderMapKey
	{
		FMaterialShaderMapIdentity Material;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		auto operator==(const FMeshShaderMapKey&) const -> bool = default;
	};

	struct FEffectiveStaticMeshPipelineKey
	{
		FMaterialPipelineIdentity Material;
		FRHIRasterizerState Rasterizer;
		FRHIDepthStencilState Depth;
		FRHIColorBlendState ColorBlend;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;

		auto operator==(const FEffectiveStaticMeshPipelineKey&) const
			-> bool = default;
	};

	// Complete value-only ordering facts. Stable identity is kept last so state
	// grouping happens before deterministic primitive/section tie breaking.
	struct FStaticMeshDrawSortKey
	{
		std::array<uint32, 26> Pipeline{};
		std::vector<uint8> MaterialUniform;
		std::array<uint32, 1 + MaxVertexElementCount * 5> VertexFactory{};
		std::array<uint32, 6> Geometry{};
		uint64 PrimitiveId = 0;
		uint32 SelectedLODIndex = 0;
		uint32 SectionIndex = 0;

		auto operator==(const FStaticMeshDrawSortKey&) const -> bool = default;
	};

	enum class EPreparedStaticMeshPhase : uint8
	{
		Prepared,
		ResourcesPrepared,
		Executed,
	};

	// Stores primitive/selected-LOD facts once for all of its prepared draws.
	struct FPreparedStaticMeshPrimitive
	{
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		uint32 RequestedLODIndex = 0;
		uint32 SelectedLODIndex = 0;
		const FStaticMeshLODResources* LOD = nullptr;
		const FLocalVertexFactory* VertexFactory = nullptr;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		FSplineMeshRenderDynamicData SplineDynamicData;
		FMatrix LocalToWorld{1.0};
	};

	// References its owning prepared primitive by index so vector relocation is safe.
	struct FPreparedStaticMeshDraw
	{
		uint32 PrimitiveIndex = 0;
		uint32 SectionIndex = 0;
		const FStaticMeshSection* Section = nullptr;
		FVector3 SortCenter{0.0};
		double TranslucentDistanceSquared = 0.0;
		FMaterialRenderData Material;
		FMaterialRenderV3Binding MaterialBinding;
		EStaticMeshBasePass Pass = EStaticMeshBasePass::Opaque;
		FMaterialShaderMapIdentity ShaderMapIdentity;
		FEffectiveStaticMeshPipelineKey PipelineKey;
		FStaticMeshDrawSortKey SortKey;
		bool bResourcesReady = false;
		FRHITexture* DirectionalShadowTexture = nullptr;
		FRHISampler* DirectionalShadowSampler = nullptr;
	};

	struct FPreparedStaticMeshView
	{
		std::vector<FPreparedStaticMeshPrimitive> Primitives;
		std::vector<FPreparedStaticMeshDraw> Opaque;
		std::vector<FPreparedStaticMeshDraw> Masked;
		std::vector<FPreparedStaticMeshDraw> Translucent;
		std::vector<size_t> RequestedLODHistogram;
		std::vector<size_t> SelectedLODHistogram;
		size_t VisibleCandidates = 0;
		size_t VisibleLocalCandidates = 0;
		size_t VisibleSplineCandidates = 0;
		size_t PreparedLocalPrimitives = 0;
		size_t PreparedSplinePrimitives = 0;
		size_t RejectedSplinePrimitives = 0;
		size_t PreparedSplineSections = 0;
		size_t PreparedSplineTriangles = 0;
		size_t RetainedSplineDeformationBytes = 0;
		size_t AcceptedSplineDynamicUpdates = 0;
		size_t RejectedPrimitives = 0;
		size_t ProjectedSizeFallbacks = 0;
		size_t ResourceFallbacks = 0;
		size_t SelectedSections = 0;
		size_t SelectedTriangles = 0;
		size_t OpaqueSections = 0;
		size_t MaskedSections = 0;
		size_t TranslucentSections = 0;
		size_t OpaqueTriangles = 0;
		size_t MaskedTriangles = 0;
		size_t TranslucentTriangles = 0;
		size_t OpaqueStateGroups = 0;
		size_t MaskedStateGroups = 0;
		size_t OpaqueInputStateGroups = 0;
		size_t MaskedInputStateGroups = 0;
		size_t PipelineTransitions = 0;
		size_t MaterialTransitions = 0;
		size_t VertexFactoryTransitions = 0;
		size_t GeometryTransitions = 0;
		size_t ResourcePreparationAttemptedDraws = 0;
		size_t ResourcePreparationSuccessfulDraws = 0;
		size_t ResourcePreparationRejectedDraws = 0;
		size_t AttemptedDraws = 0;
		size_t SuccessfulDraws = 0;
		size_t RejectedDraws = 0;
		size_t GBufferAttemptedDraws = 0;
		size_t GBufferSuccessfulDraws = 0;
		size_t GBufferRejectedDraws = 0;
		size_t GBufferSkippedDraws = 0;
		size_t GBufferLocalAttemptedDraws = 0;
		size_t GBufferLocalSuccessfulDraws = 0;
		size_t GBufferLocalRejectedDraws = 0;
		size_t GBufferLocalSkippedDraws = 0;
		size_t GBufferSplineAttemptedDraws = 0;
		size_t GBufferSplineSuccessfulDraws = 0;
		size_t GBufferSplineRejectedDraws = 0;
		size_t GBufferSplineSkippedDraws = 0;
		EPreparedStaticMeshPhase Phase = EPreparedStaticMeshPhase::Prepared;

		auto GetNumSections() const -> size_t
		{
			return Opaque.size() + Masked.size() + Translucent.size();
		}

		auto GetPrimitive(const FPreparedStaticMeshDraw& Draw) const
			-> const FPreparedStaticMeshPrimitive*
		{
			return Draw.PrimitiveIndex < Primitives.size()
				? &Primitives[Draw.PrimitiveIndex] : nullptr;
		}
	};

	RENDERER_API auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode,
		std::span<const FPrimitiveSceneInfo* const> SplineSceneInfos = {}
	) -> FPreparedStaticMeshView;
} // namespace Durin
