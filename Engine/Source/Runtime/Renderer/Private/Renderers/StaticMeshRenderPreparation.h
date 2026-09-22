#pragma once

#include "RendererAPI.h"

#include "Renderers/MeshRenderPreparationCommon.h"
#include "Renderers/SurfaceMaterial.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/MeshBatch.h"
#include "RHIResources.h"
#include "Scene.h"
#include "SceneView.h"

#include <vector>
#include <map>
#include <unordered_map>

namespace Durin
{
	class FRHICommandListImmediate;

	// Command-local facts only. Providers still collect independently for each view/LOD.
	struct FStaticMeshPreparationCache
	{
		struct FTransform
		{
			FMatrix LocalToWorld{1.0};
			FMatrix4f NormalToWorld{1.0f};
			double Determinant = 0.0;
			bool bValid = false;
		};
		std::map<std::pair<uint64, uint64>, FTransform> Transforms;
		std::unordered_multimap<uint64, uint32> MaterialIndices;
		std::vector<FMaterialRenderRepresentation> Materials;
		size_t TransformBuilds = 0;
		size_t MaterialBuilds = 0;

		RENDERER_API auto ResolveTransform(FPrimitiveComponentId PrimitiveId,
			uint64 BatchId, const FMatrix& LocalToWorld) -> const FTransform&;
		RENDERER_API auto ResolveMaterial(FMaterialRenderData& Material) -> std::optional<uint32>;
	};

	// Larger values draw first. View depth is signed along the engine's +X
	// camera axis and is independent of the projection's device-depth convention.
	inline auto ComputeTranslucentSortDepth(
		const FSceneView& View, const FVector3& WorldCenter) -> double
	{
		const auto Policy = View.Settings.Mode.TranslucentSortPolicy;
		constexpr double ProjectionKindEpsilon = 1.0e-12;
		const bool bOrthographic = std::abs(View.ProjectionMatrix[0][3]) <= ProjectionKindEpsilon
			&& std::abs(View.ProjectionMatrix[1][3]) <= ProjectionKindEpsilon
			&& std::abs(View.ProjectionMatrix[2][3]) <= ProjectionKindEpsilon
			&& std::abs(View.ProjectionMatrix[3][3]) > ProjectionKindEpsilon;
		if (Policy == ETranslucentSortPolicy::ViewDepth
			|| (Policy == ETranslucentSortPolicy::Projection && bOrthographic))
		{
			return (View.ViewMatrix * FVector4(WorldCenter, 1.0)).x;
		}
		const FVector3 Offset = WorldCenter - View.ViewLocation;
		return Math::Dot(Offset, Offset);
	}

	// Stores one batch binding/transform shared by its prepared elements.
	struct FPreparedStaticMeshPrimitive
	{
		FPrimitiveComponentId PrimitiveId = InvalidPrimitiveComponentId;
		uint64 BatchId = 0;
		uint32 RequestedLODIndex = 0;
		uint32 SelectedLODIndex = 0;
		EVertexDeformationDomain VertexDomain = EVertexDeformationDomain::Local;
		std::shared_ptr<const FVertexFactoryInputBinding> CollectedBinding;
		FMatrix LocalToWorld{1.0};
		// Validated inverse-transpose in shader uniform storage order.
		FMatrix4f NormalToWorld{1.0f};
	};

	// References its owning prepared primitive by index so vector relocation is safe.
	struct FPreparedStaticMeshDraw
	{
		uint32 ResolvedIndex = 0;
		uint32 PrimitiveIndex = 0;
		// Dense uniform group assigned after draw sorting.
		uint32 MaterialUniformIndex = UINT32_MAX;
		uint64 SectionIndex = 0;
		FGeometryDrawRange Geometry;
		FGeometryBufferView Vertices;
		FGeometryBufferView Indices;
		uint32 MaterialSlotDiagnostic = 0;
		bool bSupportsGBuffer = true;
		FVector3 SortCenter{0.0};
		// Finite descending key: squared world distance or signed view depth.
		double TranslucentSortDepth = 0.0;
		FMaterialRenderData Material;
		EMeshBasePass Pass = EMeshBasePass::Opaque;
		FEffectiveMeshPipelineKey PipelineKey;
		FMeshDrawSortKey SortKey;
	};

	struct FPreparedStaticMeshView
	{
		struct FMaterialUniformGroup
		{
			uint32 RepresentativeDraw = 0;
		};
		std::vector<FMaterialUniformGroup> MaterialUniformGroups;
		std::vector<FPreparedStaticMeshPrimitive> Primitives;
		std::vector<FPreparedStaticMeshDraw> Opaque;
		std::vector<FPreparedStaticMeshDraw> Masked;
		std::vector<FPreparedStaticMeshDraw> Translucent;
		std::vector<size_t> RequestedLODHistogram;
		std::vector<size_t> SelectedLODHistogram;
		std::array<size_t, 6> SubmissionOutcomes{};
		bool bResourceFailure = false;
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
		uint64 SortingNanoseconds = 0;
		size_t SharedPrimitiveFactBuilds = 0;
		size_t SelectedLODFactBuilds = 0;
		size_t SharedSectionFactBuilds = 0;

		auto GetDraw(uint32 Index) const -> const FPreparedStaticMeshDraw&
		{
			if (Index < Opaque.size()) return Opaque[Index];
			Index -= static_cast<uint32>(Opaque.size());
			if (Index < Masked.size()) return Masked[Index];
			return Translucent[Index - Masked.size()];
		}

		auto GetNumSections() const -> size_t
		{
			return Opaque.size() + Masked.size() + Translucent.size();
		}

		auto GetPrimitive(const FPreparedStaticMeshDraw& Draw) const
			-> const FPreparedStaticMeshPrimitive*
		{
			return Draw.PrimitiveIndex < Primitives.size() ? &Primitives[Draw.PrimitiveIndex] : nullptr;
		}
	};

	struct FStaticMeshRenderObservations
	{
		size_t PrimitiveUniformUploads = 0;
		size_t MaterialUniformUploads = 0;
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
	};

	// Owns fallible bindings without mutating logical draws.
	struct FResolvedStaticMeshView
	{
		struct FMaterialUniform
		{
			RendererPrivate::FResolvedSurfaceMaterial Surface;
			FRHIUniformBufferRange Uniform;
		};
		std::vector<FResolvedMeshDrawRecord> Draws;
		// Dense primitive/material indices avoid per-draw pointer-keyed lookups.
		std::vector<FRHIUniformBufferRange> PrimitiveUniforms;
		std::array<FRHIUniformBufferRange, 3> ViewUniforms;
		// Forward, GBuffer, and masked-shadow slots are prepared before recording.
		std::vector<std::array<std::optional<FMaterialUniform>, 3>> MaterialUniforms;
		FRHITexture* DirectionalShadowTexture = nullptr;
		FRHISampler* DirectionalShadowSampler = nullptr;
		FStaticMeshRenderObservations Observations;

		auto IsReady(const FPreparedStaticMeshDraw& Draw) const -> bool
		{
			return Draw.ResolvedIndex < Draws.size()
				&& Draws[Draw.ResolvedIndex].bReady;
		}
		auto GetMaterialBinding(const FPreparedStaticMeshDraw& Draw) const
			-> const FMaterialRenderBinding*
		{
			return Draw.ResolvedIndex < Draws.size()
				&& Draws[Draw.ResolvedIndex].MaterialBinding
				? &*Draws[Draw.ResolvedIndex].MaterialBinding : nullptr;
		}
	};

	RENDERER_API auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode,
		ERenderPreparationMode Mode = ERenderPreparationMode::Full,
		FStaticMeshPreparationCache* SharedCache = nullptr
	) -> FPreparedStaticMeshView;
} // namespace Durin
