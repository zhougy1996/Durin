#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"

namespace Durin
{
	using namespace RendererPrivate;

	auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode,
		std::span<const FPrimitiveSceneInfo* const> SplineSceneInfos,
		ERenderPreparationMode Mode
	) -> FPreparedStaticMeshView
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(), "StaticMesh preparation must occur before the scene render pass.");
		FPreparedStaticMeshView Result;
		Result.Primitives.reserve(SceneInfos.size() + SplineSceneInfos.size());
		auto PrepareSceneInfo = [&](const FPrimitiveSceneInfo* SceneInfo) {
			if (SceneInfo == nullptr)
			{
				++Result.RejectedPrimitives;
				return;
			}
			const bool bSplineMesh = SceneInfo->GetKind() == EPrimitiveSceneProxyKind::SplineMesh;
			++Result.VisibleCandidates;
			if (bSplineMesh)
				++Result.VisibleSplineCandidates;
			else
				++Result.VisibleLocalCandidates;
			check(bSplineMesh || SceneInfo->GetKind() == EPrimitiveSceneProxyKind::StaticMesh);
			const FStaticMeshSceneProxy* StaticProxy = bSplineMesh ? nullptr : &SceneInfo->GetStaticMeshProxy();
			const FSplineMeshSceneProxy* SplineProxy = bSplineMesh ? &SceneInfo->GetSplineMeshProxy() : nullptr;
			const FStaticMeshRenderData* RenderData = bSplineMesh ? SplineProxy->GetRenderData() : StaticProxy->GetRenderData();
			++Result.SharedPrimitiveFactBuilds;
			if (RenderData == nullptr || RenderData->LODResources.empty()
				|| RenderData->LODVertexFactories.size()
					   != RenderData->LODResources.size())
			{
				++Result.RejectedPrimitives;
				return;
			}

			const FProjectedScreenSizeResult ProjectedSize =
				ComputeProjectedScreenSize(View, SceneInfo->GetWorldBounds());
			if (ProjectedSize.Status != EProjectedScreenSizeStatus::Valid)
			{
				++Result.ProjectedSizeFallbacks;
			}
			const uint32 RequestedLODIndex =
				View.Settings.Mode.LODMode == EViewLODMode::ForceLOD0 ?
					0u :
					SelectStaticMeshLOD(
						ProjectedSize.NormalizedScreenSize,
						RenderData->LODResources
					);
			const uint32 SelectedLODIndex = ResolveAvailableStaticMeshLOD(
				RequestedLODIndex, RenderData->LODResources
			);
			if (SelectedLODIndex == InvalidStaticMeshLODIndex)
			{
				++Result.RejectedPrimitives;
				return;
			}
			if (SelectedLODIndex != RequestedLODIndex)
			{
				++Result.ResourceFallbacks;
			}
			const FStaticMeshLODResources& LOD =
				RenderData->LODResources[SelectedLODIndex];
			++Result.SelectedLODFactBuilds;
			const FLocalVertexFactory& VertexFactory =
				RenderData->LODVertexFactories[SelectedLODIndex].VertexFactory;
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			const FMatrix& LocalToWorld = SceneInfo->GetTransform();
			if (!Math::IsFinite(LocalToWorld))
			{
				++Result.RejectedPrimitives;
				return;
			}
			const double Determinant = Math::LinearDeterminant(LocalToWorld);
			FMatrix WorldToLocal;
			if (!std::isfinite(Determinant)
				|| !Math::TryInverse(LocalToWorld, WorldToLocal))
			{
				++Result.RejectedPrimitives;
				return;
			}
			const FMatrix4f NormalToWorld = Math::TransposeToFloat(
				Math::Transpose(WorldToLocal)
			);
			if (!Math::IsFinite(FMatrix(NormalToWorld)))
			{
				++Result.RejectedPrimitives;
				return;
			}

			const uint32 PrimitiveIndex =
				static_cast<uint32>(Result.Primitives.size());
			Result.Primitives.push_back({.PrimitiveId = SceneInfo->GetId(), .RequestedLODIndex = RequestedLODIndex, .SelectedLODIndex = SelectedLODIndex, .LOD = &LOD, .VertexFactory = &VertexFactory, .VertexDomain = bSplineMesh ? EVertexDeformationDomain::Spline : EVertexDeformationDomain::Local, .SplineDynamicData = bSplineMesh ? SplineProxy->GetDynamicData() : FSplineMeshRenderDynamicData{}, .LocalToWorld = LocalToWorld, .NormalToWorld = NormalToWorld});
			const size_t FirstSectionCount = Result.GetNumSections();
			const size_t FirstTriangleCount = Result.SelectedTriangles;

			for (uint32 SectionIndex = 0;
				 SectionIndex < static_cast<uint32>(LOD.Sections.size());
				 ++SectionIndex)
			{
				const FStaticMeshSection& Section = LOD.Sections[SectionIndex];
				if (Section.IndexCount == 0
					|| static_cast<uint64>(Section.FirstIndex) + Section.IndexCount
						   > Indices.size())
				{
					continue;
				}
				++Result.SharedSectionFactBuilds;

				const FMaterialRenderData& ResolvedMaterial = bSplineMesh ? SplineProxy->ResolveMaterialRenderData_RenderThread(Section.MaterialSlotIndex) : StaticProxy->ResolveMaterialRenderData_RenderThread(Section.MaterialSlotIndex);
				FPreparedStaticMeshDraw Item;
				Item.Material = ResolvedMaterial;
				FMaterialRenderBinding LogicalBinding;
				if (!ResolveMaterialBinding(Item.Material, LogicalBinding,
						"StaticMeshMaterialSelection"))
					continue;

				Item.PrimitiveIndex = PrimitiveIndex;
				Item.SectionIndex = SectionIndex;
				Item.Section = &Section;
				Item.ShaderMapIdentity = Item.Material.PlanningPassIdentity.ShaderMap;
				Item.PipelineKey.Material = Item.Material.PlanningPassIdentity;
				Item.PipelineKey.VertexDomain = Result.Primitives[PrimitiveIndex].VertexDomain;
				Item.PipelineKey.Rasterizer.PolygonMode =
					RasterMode == ERasterMode::Wireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
				Item.PipelineKey.Rasterizer.CullMode =
					Item.Material.PlanningPassIdentity.bTwoSided ? ERHICullMode::None : ERHICullMode::Back;
				Item.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0 ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
				Item.PipelineKey.Depth.bEnableTest = true;
				Item.PipelineKey.Depth.CompareOp =
					View.DepthConvention == ESceneDepthConvention::ReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less;
				const auto BlendMode =
					Item.Material.PlanningPassIdentity.ShaderMap.BlendMode;
				Item.Pass = BlendMode == EMaterialBlendMode::Masked ? EMeshBasePass::Masked : BlendMode == EMaterialBlendMode::Translucent ? EMeshBasePass::Translucent :
																												   EMeshBasePass::Opaque;
				if (Mode == ERenderPreparationMode::ShadowDepth
					&& Item.Pass == EMeshBasePass::Translucent)
					continue;
				const EMaterialDepthWritePolicy DepthPolicy =
					Item.Material.PlanningPassIdentity.DepthWritePolicy;
				Item.PipelineKey.Depth.bEnableWrite =
					DepthPolicy == EMaterialDepthWritePolicy::Enabled
					|| (DepthPolicy == EMaterialDepthWritePolicy::Automatic
						&& Item.Pass != EMeshBasePass::Translucent);
				if (Item.Pass == EMeshBasePass::Translucent)
				{
					Item.PipelineKey.ColorBlend = FRHIColorBlendState::StraightAlpha();
				}

				auto TryCenter = [&](const FBox& Bounds, bool bLocal) -> bool {
					if (!Bounds.bIsValid || !Math::IsFinite(Bounds.Min)
						|| !Math::IsFinite(Bounds.Max))
					{
						return false;
					}
					const FVector4 Candidate = bLocal ? LocalToWorld * FVector4(Bounds.GetCenter(), 1.0) : FVector4(Bounds.GetCenter(), 1.0);
					if (!Math::IsFinite(Candidate))
					{
						return false;
					}
					Item.SortCenter = FVector3(Candidate);
					return true;
				};
				if (!TryCenter(Section.LocalBounds, true)
					&& !TryCenter(SceneInfo->GetWorldBounds(), false))
				{
					const FVector4 Origin = LocalToWorld * FVector4(0.0, 0.0, 0.0, 1.0);
					if (!Math::IsFinite(Origin))
					{
						continue;
					}
					Item.SortCenter = FVector3(Origin);
				}
				const FVector3 Offset = Item.SortCenter - View.ViewLocation;
				Item.TranslucentDistanceSquared = Math::Dot(Offset, Offset);
				if (!std::isfinite(Item.TranslucentDistanceSquared))
				{
					continue;
				}
				const bool bFiniteSortKey = std::isfinite(
					Item.PipelineKey.Material.ShaderMap.OpacityMaskThreshold
				);
				checkf(bFiniteSortKey, "StaticMesh prepared ordering keys must be finite.");
				if (!bFiniteSortKey)
				{
					continue;
				}
				Item.SortKey = MakeStaticMeshDrawSortKey(
					Result.Primitives[PrimitiveIndex], Item
				);

				switch (Item.Pass)
				{
				case EMeshBasePass::Opaque:
					++Result.OpaqueSections;
					Result.OpaqueTriangles += Section.IndexCount / 3;
					Result.Opaque.push_back(std::move(Item));
					break;
				case EMeshBasePass::Masked:
					++Result.MaskedSections;
					Result.MaskedTriangles += Section.IndexCount / 3;
					Result.Masked.push_back(std::move(Item));
					break;
				case EMeshBasePass::Translucent:
					++Result.TranslucentSections;
					Result.TranslucentTriangles += Section.IndexCount / 3;
					Result.Translucent.push_back(std::move(Item));
					break;
				}
				Result.SelectedTriangles += Section.IndexCount / 3;
			}

			const size_t PreparedSectionCount =
				Result.GetNumSections() - FirstSectionCount;
			if (PreparedSectionCount == 0)
			{
				Result.Primitives.pop_back();
				Result.SelectedTriangles = FirstTriangleCount;
				++Result.RejectedPrimitives;
				return;
			}
			if (bSplineMesh)
			{
				++Result.PreparedSplinePrimitives;
				Result.PreparedSplineSections += PreparedSectionCount;
				Result.PreparedSplineTriangles += Result.SelectedTriangles - FirstTriangleCount;
				Result.RetainedSplineDeformationBytes += sizeof(FSplineMeshRenderDynamicData);
				Result.AcceptedSplineDynamicUpdates += SplineProxy->GetAcceptedDynamicUpdateCount();
			}
			else
				++Result.PreparedLocalPrimitives;
			Result.SelectedSections += PreparedSectionCount;
			const size_t HistogramSize = RenderData->LODResources.size();
			Result.RequestedLODHistogram.resize(
				std::max(Result.RequestedLODHistogram.size(), HistogramSize)
			);
			Result.SelectedLODHistogram.resize(
				std::max(Result.SelectedLODHistogram.size(), HistogramSize)
			);
			++Result.RequestedLODHistogram[RequestedLODIndex];
			++Result.SelectedLODHistogram[SelectedLODIndex];
		};
		std::ranges::for_each(SceneInfos, PrepareSceneInfo);
		std::ranges::for_each(SplineSceneInfos, PrepareSceneInfo);
		Result.RejectedSplinePrimitives = Result.VisibleSplineCandidates
										  - std::min(Result.VisibleSplineCandidates, Result.PreparedSplinePrimitives);
		const auto SortingStart = std::chrono::steady_clock::now();
		auto CountInputStateGroups = [](const auto& Bucket) -> size_t {
			if (Bucket.empty())
			{
				return 0;
			}
			size_t Groups = 1;
			for (size_t Index = 1; Index < Bucket.size(); ++Index)
			{
				const FMeshDrawSortKey& Previous =
					Bucket[Index - 1].SortKey;
				const FMeshDrawSortKey& Current = Bucket[Index].SortKey;
				const bool bStateChanged = Previous.Pipeline != Current.Pipeline
										   || Previous.MaterialUniform != Current.MaterialUniform
										   || Previous.VertexFactory != Current.VertexFactory;
				Groups += bStateChanged ? 1u : 0u;
			}
			return Groups;
		};
		Result.OpaqueInputStateGroups = CountInputStateGroups(Result.Opaque);
		Result.MaskedInputStateGroups = CountInputStateGroups(Result.Masked);
		auto StateSort = [](const FPreparedStaticMeshDraw& A,
							const FPreparedStaticMeshDraw& B) {
			return A.SortKey < B.SortKey;
		};
		std::ranges::sort(Result.Opaque, StateSort);
		std::ranges::sort(Result.Masked, StateSort);
		std::ranges::sort(
			Result.Translucent,
			[](const FPreparedStaticMeshDraw& A,
			   const FPreparedStaticMeshDraw& B) {
				if (A.TranslucentDistanceSquared
					!= B.TranslucentDistanceSquared)
				{
					return A.TranslucentDistanceSquared
						   > B.TranslucentDistanceSquared;
				}
				return A.SortKey < B.SortKey;
			}
		);
		AssignResolvedIndices(Result.Opaque, Result.Masked, Result.Translucent);
		Result.SortingNanoseconds = static_cast<uint64>(std::chrono::duration_cast<
															std::chrono::nanoseconds>(
															std::chrono::steady_clock::now() - SortingStart
		)
															.count());

		auto CountStateFacts = [&Result](const auto& Bucket) -> size_t {
			if (Bucket.empty())
			{
				return 0;
			}
			size_t StateGroups = 1;
			for (size_t Index = 1; Index < Bucket.size(); ++Index)
			{
				const FMeshDrawSortKey& Previous = Bucket[Index - 1].SortKey;
				const FMeshDrawSortKey& Current = Bucket[Index].SortKey;
				const bool bPipelineChanged =
					Previous.Pipeline != Current.Pipeline;
				const bool bMaterialChanged =
					Previous.MaterialUniform != Current.MaterialUniform;
				const bool bVertexFactoryChanged =
					Previous.VertexFactory != Current.VertexFactory;
				const bool bGeometryChanged =
					Previous.Geometry != Current.Geometry
					|| Previous.PrimitiveId != Current.PrimitiveId
					|| Previous.SelectedLODIndex != Current.SelectedLODIndex;
				Result.PipelineTransitions += bPipelineChanged ? 1u : 0u;
				Result.MaterialTransitions += bMaterialChanged ? 1u : 0u;
				Result.VertexFactoryTransitions +=
					bVertexFactoryChanged ? 1u : 0u;
				Result.GeometryTransitions += bGeometryChanged ? 1u : 0u;
				StateGroups += bPipelineChanged || bMaterialChanged
									   || bVertexFactoryChanged ?
								   1u :
								   0u;
			}
			return StateGroups;
		};
		Result.OpaqueStateGroups = CountStateFacts(Result.Opaque);
		Result.MaskedStateGroups = CountStateFacts(Result.Masked);
		const bool bOpaqueGroupingDidNotRegress =
			Result.OpaqueStateGroups <= Result.OpaqueInputStateGroups;
		const bool bMaskedGroupingDidNotRegress =
			Result.MaskedStateGroups <= Result.MaskedInputStateGroups;
		check(bOpaqueGroupingDidNotRegress);
		check(bMaskedGroupingDidNotRegress);
		const size_t RequestedHistogramTotal = std::accumulate(
			Result.RequestedLODHistogram.begin(),
			Result.RequestedLODHistogram.end(), size_t{0}
		);
		const size_t SelectedHistogramTotal = std::accumulate(
			Result.SelectedLODHistogram.begin(),
			Result.SelectedLODHistogram.end(), size_t{0}
		);
		const size_t PreparedPrimitiveCount = Result.Primitives.size();
		const size_t PreparedDrawCount = Result.GetNumSections();
		const bool bPrimitiveCountersConserved = Result.VisibleCandidates
												 == PreparedPrimitiveCount + Result.RejectedPrimitives;
		const bool bSectionCountersConserved =
			Result.SelectedSections == PreparedDrawCount;
		const bool bPassSectionCountersConserved = Result.SelectedSections
												   == Result.OpaqueSections + Result.MaskedSections
														  + Result.TranslucentSections;
		const bool bPassTriangleCountersConserved = Result.SelectedTriangles
													== Result.OpaqueTriangles + Result.MaskedTriangles
														   + Result.TranslucentTriangles;
		const bool bRequestedHistogramConserved =
			RequestedHistogramTotal == PreparedPrimitiveCount;
		const bool bSelectedHistogramConserved =
			SelectedHistogramTotal == PreparedPrimitiveCount;
		check(bPrimitiveCountersConserved);
		check(bSectionCountersConserved);
		check(bPassSectionCountersConserved);
		check(bPassTriangleCountersConserved);
		check(bRequestedHistogramConserved);
		check(bSelectedHistogramConserved);
		return Result;
	}
} // namespace Durin
