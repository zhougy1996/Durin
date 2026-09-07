#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"
#include "SceneInfo.h"
#include "Rendering/MeshBatch.h"
#include "Renderers/MeshVertexFactory.h"

namespace Durin
{
	using namespace RendererPrivate;

	auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode,
		ERenderPreparationMode Mode
	) -> FPreparedStaticMeshView
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(), "StaticMesh preparation must occur before the scene render pass.");
		FPreparedStaticMeshView Result;
		Result.Primitives.reserve(SceneInfos.size());
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
			++Result.SharedPrimitiveFactBuilds;
			const auto ProjectedSize = ComputeProjectedScreenSize(View, SceneInfo->GetWorldBounds());
			if (ProjectedSize.Status != EProjectedScreenSizeStatus::Valid)
				++Result.ProjectedSizeFallbacks;
			FMeshCollectionContext Context;
			Context.PrimitiveId = SceneInfo->GetId();
			Context.LocalToWorld = SceneInfo->GetTransform();
			Context.WorldBounds = SceneInfo->GetWorldBounds();
			Context.NormalizedScreenSize = ProjectedSize.NormalizedScreenSize;
			switch (ProjectedSize.Status)
			{
			case EProjectedScreenSizeStatus::Valid: Context.ProjectionStatus = EMeshCollectionProjectionStatus::Valid; break;
			case EProjectedScreenSizeStatus::NearPlaneOrCameraCrossing: Context.ProjectionStatus = EMeshCollectionProjectionStatus::NearPlaneOrCameraCrossing; break;
			case EProjectedScreenSizeStatus::InvalidBounds: Context.ProjectionStatus = EMeshCollectionProjectionStatus::InvalidBounds; break;
			case EProjectedScreenSizeStatus::InvalidView: Context.ProjectionStatus = EMeshCollectionProjectionStatus::InvalidView; break;
			}
			Context.bForceLOD0 = View.Settings.Mode.LODMode == EViewLODMode::ForceLOD0;
			Context.Purpose = Mode == ERenderPreparationMode::ShadowDepth
				? EMeshCollectionPurpose::Shadow : EMeshCollectionPurpose::Receiver;
			FMeshBatchCollector Collector(Context.Purpose);
			SceneInfo->GetProxy().CollectMeshBatches(Context, Collector);
			for (size_t I = 0; I < Result.SubmissionOutcomes.size(); ++I)
				Result.SubmissionOutcomes[I] += Collector.GetOutcomeCount(static_cast<EGeometrySubmissionOutcome>(I));
			Result.bResourceFailure |= Collector.GetOutcomeCount(EGeometrySubmissionOutcome::ResourceFailure) != 0;
			if (Collector.GetBatches().empty())
			{
				++Result.RejectedPrimitives;
				return;
			}
			auto Batches = std::move(Collector).TakeBatches();
			const size_t FirstPrimitiveBatch = Result.Primitives.size();
			auto PrepareBatch = [&](FMeshBatch& Batch) {
				const auto Binding = std::dynamic_pointer_cast<const FVertexFactoryInputBinding>(Batch.Binding);
				if (!Binding || !Binding->Declaration || Binding->Streams.empty())
				{
					++Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)];
					Result.bResourceFailure = true;
					return;
				}
				const auto Factory = FindMeshVertexFactory(Batch.FactoryKey);
				if (!Factory || Factory->GetLayoutKey() != Batch.LayoutKey)
				{
					++Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Unsupported)];
					return;
				}
				const uint32 RequestedLODIndex = Batch.RequestedLOD;
				const uint32 SelectedLODIndex = Batch.SelectedLOD;
				if (SelectedLODIndex != RequestedLODIndex) ++Result.ResourceFallbacks;
				++Result.SelectedLODFactBuilds;
				const FMatrix& LocalToWorld = Batch.LocalToWorld;
				if (!Math::IsFinite(LocalToWorld))
				{
					return;
				}
				const double Determinant = Math::LinearDeterminant(LocalToWorld);
				FMatrix WorldToLocal;
				if (!std::isfinite(Determinant)
					|| !Math::TryInverse(LocalToWorld, WorldToLocal))
				{
					return;
				}
				const FMatrix4f NormalToWorld = Math::TransposeToFloat(
					Math::Transpose(WorldToLocal)
				);
				if (!Math::IsFinite(FMatrix(NormalToWorld)))
				{
					return;
				}

				const uint32 PrimitiveIndex =
					static_cast<uint32>(Result.Primitives.size());
				Result.Primitives.push_back({.PrimitiveId = SceneInfo->GetId(), .BatchId = Batch.BatchId, .RequestedLODIndex = RequestedLODIndex, .SelectedLODIndex = SelectedLODIndex, .VertexDomain = bSplineMesh ? EVertexDeformationDomain::Spline : EVertexDeformationDomain::Local, .CollectedBinding = Binding, .LocalToWorld = LocalToWorld, .NormalToWorld = NormalToWorld});
				const size_t FirstSectionCount = Result.GetNumSections();
				const size_t FirstTriangleCount = Result.SelectedTriangles;

				for (auto& Element : Batch.Elements)
				{
					const bool bResourceViewsMatch = std::ranges::any_of(Binding->Streams, [&](const auto& Stream) {
						return Stream.VertexBuffer == Element.Vertices.Buffer && Stream.Offset == Element.Vertices.Range.ByteOffset
							&& Stream.Stride == Element.Vertices.Range.Stride;
					}) && std::ranges::all_of(Element.InstanceStreams, [&](const auto& Instance) {
						return std::ranges::any_of(Binding->Streams, [&](const auto& Stream) {
							return Stream.VertexBuffer == Instance.Buffer && Stream.Offset == Instance.Range.ByteOffset
								&& Stream.Stride == Instance.Range.Stride
								&& std::ranges::any_of(Binding->DeclarationElements, [&](const auto& Attribute) {
									return Attribute.Type != EVertexElementType::None && Attribute.StreamIndex == Stream.StreamIndex
										&& Attribute.InputRate == FRHIVertexElementIdentity::EInputRate::Instance;
								});
						});
					});
					const auto InputOutcome = bResourceViewsMatch ? Binding->ValidateInputs(Element.Draw) : EGeometrySubmissionOutcome::InvalidSubmission;
					if (InputOutcome != EGeometrySubmissionOutcome::Submitted)
					{
						++Result.SubmissionOutcomes[static_cast<size_t>(InputOutcome)];
						Result.bResourceFailure |= InputOutcome == EGeometrySubmissionOutcome::ResourceFailure;
						continue;
					}
					if (!Factory->Supports(Mode == ERenderPreparationMode::ShadowDepth ? MaterialMeshPassShadow : MaterialMeshPassForward, Element.Draw))
					{
						++Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Unsupported)];
						continue;
					}
					const size_t TriangleCount = Element.Draw.Topology == EGeometryTopology::TriangleList ? Element.Draw.ElementCount / 3 : 0;
					const uint64 SectionIndex = Element.ElementId;
					++Result.SharedSectionFactBuilds;
					FPreparedStaticMeshDraw Item;
					Item.Material = std::move(Element.Material);
					FMaterialRenderBinding LogicalBinding;
					if (!ResolveMaterialBinding(Item.Material, LogicalBinding,
							"StaticMeshMaterialSelection"))
						continue;

					Item.PrimitiveIndex = PrimitiveIndex;
					Item.SectionIndex = SectionIndex;
					Item.Geometry = Element.Draw;
					Item.bSupportsGBuffer = Factory->Supports(MaterialMeshPassGBuffer, Element.Draw);
					Item.Vertices = Element.Vertices;
					Item.Indices = Element.Indices;
					Item.MaterialSlotDiagnostic = Element.MaterialSlotDiagnostic;
					Item.PipelineKey.Material = Item.Material.PlanningPassIdentity;
					Item.PipelineKey.VertexDomain = Result.Primitives[PrimitiveIndex].VertexDomain;
					Item.PipelineKey.FactoryKey = Batch.FactoryKey;
					Item.PipelineKey.LayoutKey = Batch.LayoutKey;
					Item.PipelineKey.VertexDeclaration = Binding->Declaration;
					Item.PipelineKey.Topology = Element.Draw.Topology;
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
					if (!TryCenter(Element.LocalBounds, true)
						&& !TryCenter(Batch.WorldBounds, false))
					{
						const FVector4 Origin = LocalToWorld * FVector4(0.0, 0.0, 0.0, 1.0);
						if (!Math::IsFinite(Origin))
						{
							continue;
						}
						Item.SortCenter = FVector3(Origin);
					}
					Item.TranslucentSortDepth = ComputeTranslucentSortDepth(View, Item.SortCenter);
					if (!std::isfinite(Item.TranslucentSortDepth))
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
						Result.OpaqueTriangles += TriangleCount;
						Result.Opaque.push_back(std::move(Item));
						break;
					case EMeshBasePass::Masked:
						++Result.MaskedSections;
						Result.MaskedTriangles += TriangleCount;
						Result.Masked.push_back(std::move(Item));
						break;
					case EMeshBasePass::Translucent:
						++Result.TranslucentSections;
						Result.TranslucentTriangles += TriangleCount;
						Result.Translucent.push_back(std::move(Item));
						break;
					}
					Result.SelectedTriangles += TriangleCount;
				}

				const size_t PreparedSectionCount =
					Result.GetNumSections() - FirstSectionCount;
				if (PreparedSectionCount == 0)
				{
					Result.Primitives.pop_back();
					Result.SelectedTriangles = FirstTriangleCount;
					return;
				}
				if (bSplineMesh)
				{
					Result.PreparedSplineSections += PreparedSectionCount;
					Result.PreparedSplineTriangles += Result.SelectedTriangles - FirstTriangleCount;
					Result.RetainedSplineDeformationBytes += Batch.RetainedDeformationBytes;
					Result.AcceptedSplineDynamicUpdates += Batch.AcceptedDynamicUpdates;
				}
				Result.SelectedSections += PreparedSectionCount;
				const size_t HistogramSize = Batch.LODCount;
				Result.RequestedLODHistogram.resize(
					std::max(Result.RequestedLODHistogram.size(), HistogramSize)
				);
				Result.SelectedLODHistogram.resize(
					std::max(Result.SelectedLODHistogram.size(), HistogramSize)
				);
				++Result.RequestedLODHistogram[RequestedLODIndex];
				++Result.SelectedLODHistogram[SelectedLODIndex];
			};
			for (auto& Batch : Batches) PrepareBatch(Batch);
			if (Result.Primitives.size() == FirstPrimitiveBatch) ++Result.RejectedPrimitives;
			else if (bSplineMesh) ++Result.PreparedSplinePrimitives;
			else ++Result.PreparedLocalPrimitives;
		};
		std::ranges::for_each(SceneInfos, PrepareSceneInfo);
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
				if (A.TranslucentSortDepth
					!= B.TranslucentSortDepth)
				{
					return A.TranslucentSortDepth
						   > B.TranslucentSortDepth;
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
					|| Previous.BatchId != Current.BatchId
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
		const size_t PreparedPrimitiveCount = Result.PreparedLocalPrimitives + Result.PreparedSplinePrimitives;
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
			RequestedHistogramTotal == Result.Primitives.size();
		const bool bSelectedHistogramConserved =
			SelectedHistogramTotal == Result.Primitives.size();
		check(bPrimitiveCountersConserved);
		check(bSectionCountersConserved);
		check(bPassSectionCountersConserved);
		check(bPassTriangleCountersConserved);
		check(bRequestedHistogramConserved);
		check(bSelectedHistogramConserved);
		return Result;
	}
} // namespace Durin
