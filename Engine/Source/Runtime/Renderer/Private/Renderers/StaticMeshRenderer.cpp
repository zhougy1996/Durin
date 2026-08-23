#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"

namespace Durin
{
	using namespace RendererPrivate;

	struct FStaticMeshRenderer::FState
	{
		struct FShaderMapPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FStaticMeshVertexShader> VertexShader;
			TShaderRef<FSplineMeshVertexShader> SplineVertexShader;
			TShaderRef<FSurfaceFragmentShader> FragmentShader;
			TShaderRef<FSurfaceOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
		};

		struct FPipelinePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FStaticMeshVertexShader> VertexShader;
			TShaderRef<FSplineMeshVertexShader> SplineVertexShader;
			TShaderRef<FSurfaceFragmentShader> FragmentShader;
			TShaderRef<FSurfaceOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};

		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload>
			ShadowShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FEffectiveMeshPipelineKey,
			FPipelinePayload>
			Pipelines{
				ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device
			};
		TRendererResourceSlotCache<
			FEffectiveMeshPipelineKey,
			FPipelinePayload>
			ShadowPipelines{
				ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device
			};
	};
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
		std::vector<const FPrimitiveSceneInfo*> CombinedSceneInfos;
		CombinedSceneInfos.reserve(SceneInfos.size() + SplineSceneInfos.size());
		CombinedSceneInfos.insert(CombinedSceneInfos.end(), SceneInfos.begin(), SceneInfos.end());
		CombinedSceneInfos.insert(CombinedSceneInfos.end(), SplineSceneInfos.begin(), SplineSceneInfos.end());
		for (const FPrimitiveSceneInfo* SceneInfo : CombinedSceneInfos)
		{
			if (SceneInfo == nullptr)
			{
				++Result.RejectedPrimitives;
				continue;
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
				continue;
			}

			std::vector<float> ScreenSizes;
			std::vector<uint8> ReadyLODs;
			ScreenSizes.reserve(RenderData->LODResources.size());
			ReadyLODs.reserve(RenderData->LODResources.size());
			for (uint32 LODIndex = 0;
				 LODIndex < static_cast<uint32>(RenderData->LODResources.size());
				 ++LODIndex)
			{
				ScreenSizes.push_back(
					RenderData->LODResources[LODIndex].ScreenSize
				);
				ReadyLODs.push_back(
					RenderData->IsReadyForRendering(LODIndex) ? 1u : 0u
				);
			}
			const FProjectedScreenSizeResult ProjectedSize =
				ComputeProjectedScreenSize(View, SceneInfo->GetWorldBounds());
			if (ProjectedSize.Status != EProjectedScreenSizeStatus::Valid)
			{
				++Result.ProjectedSizeFallbacks;
			}
			const uint32 RequestedLODIndex =
				View.Settings.Mode.LODMode == EViewLODMode::ForceLOD0 ? 0u : SelectStaticMeshLOD(ProjectedSize.NormalizedScreenSize, ScreenSizes);
			const uint32 SelectedLODIndex = ResolveAvailableStaticMeshLOD(
				RequestedLODIndex, ReadyLODs
			);
			if (SelectedLODIndex == InvalidStaticMeshLODIndex)
			{
				++Result.RejectedPrimitives;
				continue;
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
				continue;
			}
			const double Determinant = Math::LinearDeterminant(LocalToWorld);
			if (!std::isfinite(Determinant))
			{
				++Result.RejectedPrimitives;
				continue;
			}

			const uint32 PrimitiveIndex =
				static_cast<uint32>(Result.Primitives.size());
			Result.Primitives.push_back({.PrimitiveId = SceneInfo->GetId(), .RequestedLODIndex = RequestedLODIndex, .SelectedLODIndex = SelectedLODIndex, .LOD = &LOD, .VertexFactory = &VertexFactory, .VertexDomain = bSplineMesh ? EVertexDeformationDomain::Spline : EVertexDeformationDomain::Local, .SplineDynamicData = bSplineMesh ? SplineProxy->GetDynamicData() : FSplineMeshRenderDynamicData{}, .LocalToWorld = LocalToWorld});
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
				Item.ShaderMapIdentity = Item.Material.PipelineIdentity.ShaderMap;
				Item.PipelineKey.Material = Item.Material.PipelineIdentity;
				Item.PipelineKey.VertexDomain = Result.Primitives[PrimitiveIndex].VertexDomain;
				Item.PipelineKey.Rasterizer.PolygonMode =
					RasterMode == ERasterMode::Wireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
				Item.PipelineKey.Rasterizer.CullMode =
					Item.Material.PipelineIdentity.bTwoSided ? ERHICullMode::None : ERHICullMode::Back;
				Item.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0 ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
				Item.PipelineKey.Depth.bEnableTest = true;
				Item.PipelineKey.Depth.CompareOp =
					View.DepthConvention == ESceneDepthConvention::ReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less;
				const EMaterialBlendMode BlendMode =
					Item.Material.PipelineIdentity.ShaderMap.BlendMode;
				Item.Pass = BlendMode == EMaterialBlendMode::Masked ? EMeshBasePass::Masked : BlendMode == EMaterialBlendMode::Translucent ? EMeshBasePass::Translucent :
																												   EMeshBasePass::Opaque;
				if (Mode == ERenderPreparationMode::ShadowDepth
					&& Item.Pass == EMeshBasePass::Translucent)
					continue;
				const EMaterialDepthWritePolicy DepthPolicy =
					Item.Material.PipelineIdentity.DepthWritePolicy;
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
				continue;
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
		}
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

	FStaticMeshRenderer::FStaticMeshRenderer(
		FRendererResourceCoordinator& InCoordinator,
		RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials
	)
		: Coordinator(InCoordinator)
		, SurfaceMaterials(InSurfaceMaterials)
		, State(std::make_unique<FState>())
	{
	}

	FStaticMeshRenderer::~FStaticMeshRenderer() = default;

	auto FStaticMeshRenderer::EnsureMaterialSamplers_RenderThread(
		const FMaterialRenderBinding& MaterialBinding
	) -> bool
	{
		return SurfaceMaterials.Ensure_RenderThread(
			MaterialBinding, ESurfaceMaterialPass::GBuffer);
	}

	auto FStaticMeshRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView,
		bool bPrepareLitOpaqueForward
	) -> FGeometryResolutionResult
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(), "StaticMesh resource preparation must occur before the scene render pass.");
		ResolvedView.Draws.resize(PreparedView.GetNumSections());
		ResolvedView.Observations.ResourcePreparationAttemptedDraws =
			PreparedView.GetNumSections();
		ForEachBasePassBucket(PreparedView, [this, &PreparedView, &ResolvedView, bPrepareLitOpaqueForward](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedStaticMeshDraw& Item : Bucket)
			{
				FMaterialRenderBinding MaterialBinding;
				if (!ResolvePreparedMaterialBinding(Item.Material, MaterialBinding,
						"StaticMeshMaterialBinding"))
					continue;
				auto& Record = ResolvedView.Draws[Item.ResolvedIndex];
				Record.MaterialBinding = std::move(MaterialBinding);
				const FMaterialRenderBinding& StoredBinding =
					*Record.MaterialBinding;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Item);
				const bool bNeedsForwardPipeline =
					Pass == EMeshBasePass::Translucent
					|| bPrepareLitOpaqueForward
					|| Item.Material.PipelineIdentity.ShaderMap.ShadingModel
						   != EMaterialShadingModel::Lit;
				const bool bReady = Primitive != nullptr
					&& (bNeedsForwardPipeline
						? EnsureSectionResources_RenderThread(*Primitive, Item,
							StoredBinding)
						: EnsureMaterialSamplers_RenderThread(StoredBinding));
				Record.bReady = bReady;
				ResolvedView.Observations.ResourcePreparationSuccessfulDraws +=
					bReady ? 1u : 0u;
			}
		});
		return FinalizeResourcePreparation(ResolvedView);
	}

	auto FStaticMeshRenderer::PrepareHybridRetainedResources_RenderThread(
		const FPreparedStaticMeshView& PreparedView,
		const FResolvedStaticMeshView& ResolvedView
	) -> bool
	{
		check(IsInRenderingThread());
		bool bReady = true;
		ForEachBasePassBucket(PreparedView, [this, &PreparedView,
			&ResolvedView, &bReady](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				if (Pass != EMeshBasePass::Translucent
					&& Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit)
					continue;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const FMaterialRenderBinding* MaterialBinding =
					ResolvedView.GetMaterialBinding(Draw);
				bReady = Primitive != nullptr && MaterialBinding != nullptr
						 && EnsureSectionResources_RenderThread(
							 *Primitive, Draw, *MaterialBinding, false, true
						 )
						 && bReady;
			}
		});
		return bReady;
	}

	auto FStaticMeshRenderer::PrepareShadowResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView
	) -> FGeometryResolutionResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		check(PreparedView.Translucent.empty());
		ResolvedView.Draws.resize(PreparedView.GetNumSections());
		ResolvedView.Observations.ResourcePreparationAttemptedDraws =
			PreparedView.GetNumSections();
		ForEachShadowBucket(PreparedView, [this, &PreparedView, &ResolvedView](const auto& Bucket) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				FMaterialRenderBinding MaterialBinding;
				if (!ResolvePreparedMaterialBinding(Draw.Material, MaterialBinding,
						"StaticMeshShadowMaterialBinding"))
					continue;
				auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
				Record.MaterialBinding = std::move(MaterialBinding);
				const FMaterialRenderBinding& StoredBinding =
					*Record.MaterialBinding;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const bool bReady = Primitive != nullptr
					&& EnsureSectionResources_RenderThread(*Primitive, Draw,
						StoredBinding, true);
				Record.bReady = bReady;
				ResolvedView.Observations.ResourcePreparationSuccessfulDraws +=
					bReady ? 1u : 0u;
			}
		});
		return FinalizeResourcePreparation(ResolvedView);
	}

	auto FStaticMeshRenderer::ExecuteShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& ShadowView,
		const FRHIUniformBufferRange& FallbackLighting,
		const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView
	) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		bool bComplete = true;
		ForEachShadowBucket(PreparedView, [this, &CommandList, &ShadowView,
			&FallbackLighting, &PreparedView, &ResolvedView,
			&bComplete](const auto& Bucket) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				++ResolvedView.Observations.AttemptedDraws;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && ResolvedView.IsReady(Draw)
					&& DrawSection_RenderThread(
						CommandList, ShadowView, FallbackLighting,
						ERenderMode::Unlit, *Primitive, Draw, ResolvedView, true
					))
					++ResolvedView.Observations.SuccessfulDraws;
				else
				{
					bComplete = false;
					++ResolvedView.Observations.RejectedDraws;
				}
			}
		});
		FinalizeExecution(ResolvedView, PreparedView.GetNumSections(), false);
		return bComplete;
	}

	auto FStaticMeshRenderer::EnsureSectionResources_RenderThread(
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		const FMaterialRenderBinding& MaterialBinding,
		bool bShadowDepth,
		bool bHybridRetained
	) -> bool
	{
		check(IsInRenderingThread());
		if (Primitive.VertexFactory == nullptr)
		{
			return false;
		}
		const FMaterialRenderData& Material = Item.Material;
		const FLocalVertexFactory& VertexFactory = *Primitive.VertexFactory;

		using FShaderMapResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		const FMeshShaderMapKey ShaderMapKey{
			.Material = Material.PipelineIdentity.ShaderMap,
			.VertexDomain = Primitive.VertexDomain
		};
		auto& ShaderMapCache = bShadowDepth ? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderMapEntry = ShaderMapCache.FindOrAdd(ShaderMapKey);
		FState::FShaderMapPayload* ShaderMapPayload =
			ShaderMapEntry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[this, &Material, Domain = Primitive.VertexDomain,
				 bShadowDepth]() -> FShaderMapResult {
					const FMaterialShaderMapIdentity& Identity =
						Material.PipelineIdentity.ShaderMap;
					FShaderCompileOptions CompileOptions;
					CompileOptions.bForceRecompile =
						Coordinator.ShouldForceShaderRecompile_RenderThread();
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_BLEND_MODE",
						std::to_string(static_cast<uint8>(Identity.BlendMode))
					);
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_SHADING_MODEL",
						std::to_string(static_cast<uint8>(Identity.ShadingModel))
					);
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
						std::to_string(std::bit_cast<uint32>(
							Identity.OpacityMaskThreshold
						))
					);
					if (bShadowDepth
						&& Identity.BlendMode != EMaterialBlendMode::Masked)
					{
						CompileOptions.Macros.emplace_back(
							"DURIN_OPAQUE_SHADOW_DEPTH", "1"
						);
					}
					if (Domain == EVertexDeformationDomain::Spline)
						CompileOptions.Macros.emplace_back("DURIN_SPLINE_MESH", "1");
					FShaderType& VertexShaderType = Domain == EVertexDeformationDomain::Spline ? FSplineMeshVertexShader::StaticType() : FStaticMeshVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FSurfaceFragmentShader::StaticType();
					FShaderType& ShadowFragmentShaderType =
						FSurfaceMaskedShadowFragmentShader::StaticType();
					FShaderType& OpaqueShadowFragmentShaderType =
						FSurfaceOpaqueShadowFragmentShader::StaticType();
					std::vector<const FShaderType*> ShaderTypes{&VertexShaderType};
					if (!bShadowDepth)
						ShaderTypes.push_back(&FragmentShaderType);
					else if (Identity.BlendMode == EMaterialBlendMode::Masked)
						ShaderTypes.push_back(&ShadowFragmentShaderType);
					else
						ShaderTypes.push_back(&OpaqueShadowFragmentShaderType);
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage
						))
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderCompile,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								std::move(ErrorMessage),
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual
							)
						);
					}
					FShader* VertexShader = ShaderMap->GetShader(&VertexShaderType);
					auto* FragmentShader = !bShadowDepth ? static_cast<FSurfaceFragmentShader*>(
															   ShaderMap->GetShader(&FragmentShaderType)
														   ) :
														   nullptr;
					auto* ShadowFragmentShader =
						bShadowDepth && Identity.BlendMode == EMaterialBlendMode::Masked ? static_cast<FSurfaceMaskedShadowFragmentShader*>(
																							   ShaderMap->GetShader(&ShadowFragmentShaderType)
																						   ) :
																						   nullptr;
					auto* OpaqueShadowFragmentShader =
						bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked ? static_cast<FSurfaceOpaqueShadowFragmentShader*>(
																							   ShaderMap->GetShader(&OpaqueShadowFragmentShaderType)
																						   ) :
																						   nullptr;
					if (VertexShader == nullptr
						|| (!bShadowDepth && FragmentShader == nullptr))
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderBinding,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								"Compiled shader map did not contain both typed shaders.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual
							)
						);
					if (bShadowDepth
						&& Identity.BlendMode == EMaterialBlendMode::Masked
						&& ShadowFragmentShader == nullptr)
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderBinding,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								"Compiled masked shader map did not contain the shadow fragment shader.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual
							)
						);
					}
					if (bShadowDepth
						&& Identity.BlendMode != EMaterialBlendMode::Masked
						&& OpaqueShadowFragmentShader == nullptr)
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderBinding,
								"StaticMeshShaderMap", GetIdentityText(Identity),
								"Compiled shader map did not contain the opaque shadow fragment shader.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual
							)
						);
					FState::FShaderMapPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					if (Domain == EVertexDeformationDomain::Spline)
						Candidate.SplineVertexShader = TShaderRef<FSplineMeshVertexShader>(
							static_cast<FSplineMeshVertexShader*>(VertexShader), Candidate.ShaderMap.get()
						);
					else
						Candidate.VertexShader = TShaderRef<FStaticMeshVertexShader>(
							static_cast<FStaticMeshVertexShader*>(VertexShader), Candidate.ShaderMap.get()
						);
					if (FragmentShader != nullptr)
						Candidate.FragmentShader = TShaderRef<FSurfaceFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get()
						);
					if (ShadowFragmentShader != nullptr)
						Candidate.ShadowFragmentShader =
							TShaderRef<FSurfaceMaskedShadowFragmentShader>(
								ShadowFragmentShader, Candidate.ShaderMap.get()
							);
					if (OpaqueShadowFragmentShader != nullptr)
						Candidate.OpaqueShadowFragmentShader =
							TShaderRef<FSurfaceOpaqueShadowFragmentShader>(
								OpaqueShadowFragmentShader, Candidate.ShaderMap.get()
							);
					if ((Domain == EVertexDeformationDomain::Spline ? Candidate.SplineVertexShader.GetRHIShader(false) : Candidate.VertexShader.GetRHIShader(false)) == nullptr
						|| (!bShadowDepth
							&& Candidate.FragmentShader.GetRHIShader(false) == nullptr)
						|| (bShadowDepth
							&& Identity.BlendMode == EMaterialBlendMode::Masked
							&& Candidate.ShadowFragmentShader.GetRHIShader(false) == nullptr)
						|| (bShadowDepth
							&& Identity.BlendMode != EMaterialBlendMode::Masked
							&& Candidate.OpaqueShadowFragmentShader.GetRHIShader(false) == nullptr))
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::RHIResource,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								"RHI shader creation returned null.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Device
									| ERenderResourceGenerationDependency::Manual
							)
						);
					}
					return FShaderMapResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic
			);
		if (ShaderMapPayload == nullptr)
		{
			return false;
		}

		using FPipelineResult =
			TRenderResourceCreateResult<FState::FPipelinePayload>;
		FEffectiveMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey) : Item.PipelineKey;
		EffectivePipelineKey.bHybridRetained =
			!bShadowDepth && bHybridRetained;
		auto& PipelineCache = bShadowDepth ? State->ShadowPipelines : State->Pipelines;
		auto& PipelineEntry = PipelineCache.FindOrAdd(EffectivePipelineKey);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader =
			ShaderMapEntry.Slot.GetPayloadGeneration().Shader;
		FState::FPipelinePayload* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&PipelineEntry, ShaderMapPayload, &EffectivePipelineKey,
			 bShadowDepth,
			 &VertexFactory]() -> FPipelineResult {
				const FEffectiveMeshPipelineKey& Identity = EffectivePipelineKey;
				FState::FPipelinePayload Candidate;
				Candidate.ShaderMap = ShaderMapPayload->ShaderMap;
				Candidate.VertexShader = ShaderMapPayload->VertexShader;
				Candidate.SplineVertexShader = ShaderMapPayload->SplineVertexShader;
				Candidate.FragmentShader = ShaderMapPayload->FragmentShader;
				Candidate.ShadowFragmentShader =
					ShaderMapPayload->ShadowFragmentShader;
				Candidate.OpaqueShadowFragmentShader =
					ShaderMapPayload->OpaqueShadowFragmentShader;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = bShadowDepth ? RenderTargetLayouts::MakeDirectionalShadowDepth() : (Identity.bHybridRetained ? RenderTargetLayouts::MakeHybridRetainedForward() : RenderTargetLayouts::MakeSceneTargets());
				Initializer.BoundShaders.VertexShader = Identity.VertexDomain == EVertexDeformationDomain::Spline ? Candidate.SplineVertexShader.GetRHIShader() : Candidate.VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader = bShadowDepth ? (Identity.Material.ShaderMap.BlendMode
																				  == EMaterialBlendMode::Masked ?
																			  Candidate.ShadowFragmentShader.GetRHIShader() :
																			  Candidate.OpaqueShadowFragmentShader.GetRHIShader()) :
																		 Candidate.FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration = VertexFactory.GetDeclaration();
				Initializer.RasterizerState = Identity.Rasterizer;
				Initializer.DepthStencilState = Identity.Depth;
				if (!bShadowDepth)
				{
					Initializer.ColorBlendStates[0] = Identity.ColorBlend;
					if (Identity.Material.ShaderMap.BlendMode
						== EMaterialBlendMode::Translucent)
					{
						Initializer.ColorBlendStates[1].ColorWriteMask =
							ERHIColorWriteMask::None;
					}
				}
				Initializer.PipelineLayout =
					Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						FName(std::format(
							"StaticMeshPipeline_{}", PipelineEntry.Index
						)),
						Initializer
					);
				if (Candidate.PipelineState == nullptr)
				{
					return FPipelineResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::GraphicsPipeline,
							"StaticMeshPipeline",
							GetIdentityText(Identity),
							"Graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual
						)
					);
				}
				return FPipelineResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic
		);
		if (Pipeline == nullptr)
		{
			return false;
		}

		const ESurfaceMaterialPass SurfacePass = bShadowDepth
			? (Item.PipelineKey.Material.ShaderMap.BlendMode
					== EMaterialBlendMode::Masked
				? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::OpaqueShadow)
			: ESurfaceMaterialPass::Forward;
		return SurfaceMaterials.Ensure_RenderThread(
			MaterialBinding, SurfacePass);
	}

	auto FStaticMeshRenderer::Execute_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRHIUniformBufferRange& Lighting,
		ERenderMode RenderMode,
		const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView
	) -> void
	{
		check(IsInRenderingThread());
		checkf(CommandList.IsInsideRenderPass(), "StaticMesh execution requires the owning scene render pass.");
		if (RenderMode != ERenderMode::Unlit
			&& RenderMode != ERenderMode::Lit)
			return;
		ForEachBasePassBucket(PreparedView, [&](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedStaticMeshDraw& Item : Bucket)
			{
				++ResolvedView.Observations.AttemptedDraws;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Item);
				const bool bBucketMatches = Item.Pass == Pass;
				const bool bSortKeyMatchesPass = Item.SortKey.Pipeline[0]
												 == static_cast<uint32>(Pass);
				checkf(bBucketMatches, "StaticMesh prepared bucket does not match its pass.");
				checkf(bSortKeyMatchesPass, "StaticMesh prepared sort key does not match its bucket.");
				const bool bComplete = Primitive != nullptr
									   && Primitive->PrimitiveId != InvalidPrimitiveSceneId
									   && Primitive->LOD != nullptr
									   && Primitive->VertexFactory != nullptr
									   && Item.Section != nullptr
									   && std::isfinite(Item.TranslucentDistanceSquared)
									   && Item.ShaderMapIdentity
											  == Item.Material.PipelineIdentity.ShaderMap
									   && Item.PipelineKey.Material
											  == Item.Material.PipelineIdentity;
				checkf(bComplete, "StaticMesh execution requires one complete prepared section.");
				if (!bBucketMatches || !bSortKeyMatchesPass || !bComplete
					|| !ResolvedView.IsReady(Item))
				{
					++ResolvedView.Observations.RejectedDraws;
					continue;
				}
				if (DrawSection_RenderThread(
						CommandList, View, Lighting, RenderMode, *Primitive,
						Item, ResolvedView
					))
				{
					++ResolvedView.Observations.SuccessfulDraws;
				}
				else
				{
					++ResolvedView.Observations.RejectedDraws;
				}
			}
		});
		FinalizeExecution(ResolvedView, PreparedView.GetNumSections());
	}

	auto FStaticMeshRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshDraw& Item, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView, bool bHybridRetained
	) -> void
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		++ResolvedView.Observations.AttemptedDraws;
		const FPreparedStaticMeshPrimitive* Primitive =
			PreparedView.GetPrimitive(Item);
		const bool bComplete = Primitive != nullptr
							   && Primitive->PrimitiveId != InvalidPrimitiveSceneId
							   && Primitive->LOD != nullptr && Primitive->VertexFactory != nullptr
							   && Item.Section != nullptr && Item.Pass == Pass
							   && Item.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
							   && Item.ShaderMapIdentity == Item.Material.PipelineIdentity.ShaderMap
							   && Item.PipelineKey.Material == Item.Material.PipelineIdentity;
		if (!bComplete || !ResolvedView.IsReady(Item))
		{
			++ResolvedView.Observations.RejectedDraws;
			return;
		}
		if (DrawSection_RenderThread(CommandList, View, Lighting, RenderMode,
			*Primitive, Item, ResolvedView, false, bHybridRetained))
			++ResolvedView.Observations.SuccessfulDraws;
		else
			++ResolvedView.Observations.RejectedDraws;
	}

	auto FStaticMeshRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView
	) -> void
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
			return;
		const auto& Bucket = GetBasePassBucket(PreparedView, Pass);
		for (const FPreparedStaticMeshDraw& Draw : Bucket)
			ExecutePreparedDraw_RenderThread(CommandList, View, Lighting,
				RenderMode, Pass, Draw, PreparedView, ResolvedView);
	}

	auto FStaticMeshRenderer::FinalizeExecution_RenderThread(
		FResolvedStaticMeshView& ResolvedView
	) -> void
	{
		check(IsInRenderingThread());
		FinalizeExecution(ResolvedView, ResolvedView.Observations.AttemptedDraws);
	}

	auto FStaticMeshRenderer::ExecuteGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FGBufferRenderer& GBuffer,
		const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView
	) -> FGeometryExecutionResult
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		bool bComplete = true;
		bool bRenderedGeometry = false;
		auto RecordFamily = [](FResolvedStaticMeshView& Resolved,
							   const FPreparedStaticMeshDraw& Draw,
							   size_t FStaticMeshRenderObservations::* Local,
							   size_t FStaticMeshRenderObservations::* Spline) {
			++(Resolved.Observations.*(
				Draw.PipelineKey.VertexDomain == EVertexDeformationDomain::Spline
					? Spline : Local));
		};
		for (const FPreparedStaticMeshDraw& Draw : PreparedView.Translucent)
		{
			++ResolvedView.Observations.GBufferSkippedDraws;
			RecordFamily(ResolvedView, Draw, &FStaticMeshRenderObservations::GBufferLocalSkippedDraws, &FStaticMeshRenderObservations::GBufferSplineSkippedDraws);
		}
		ForEachShadowBucket(PreparedView, [this, &CommandList, &View, &GBuffer, &PreparedView, &ResolvedView, &RecordFamily, &bComplete, &bRenderedGeometry](const auto& Bucket) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					++ResolvedView.Observations.GBufferSkippedDraws;
					RecordFamily(ResolvedView, Draw, &FStaticMeshRenderObservations::GBufferLocalSkippedDraws, &FStaticMeshRenderObservations::GBufferSplineSkippedDraws);
					continue;
				}
				++ResolvedView.Observations.GBufferAttemptedDraws;
				RecordFamily(ResolvedView, Draw, &FStaticMeshRenderObservations::GBufferLocalAttemptedDraws, &FStaticMeshRenderObservations::GBufferSplineAttemptedDraws);
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && ResolvedView.IsReady(Draw)
					&& DrawGBufferSection_RenderThread(
						CommandList, View, GBuffer, *Primitive, Draw,
						ResolvedView
					))
				{
					bRenderedGeometry = true;
					++ResolvedView.Observations.GBufferSuccessfulDraws;
					RecordFamily(ResolvedView, Draw, &FStaticMeshRenderObservations::GBufferLocalSuccessfulDraws, &FStaticMeshRenderObservations::GBufferSplineSuccessfulDraws);
				}
				else
				{
					bComplete = false;
					++ResolvedView.Observations.GBufferRejectedDraws;
					RecordFamily(ResolvedView, Draw, &FStaticMeshRenderObservations::GBufferLocalRejectedDraws, &FStaticMeshRenderObservations::GBufferSplineRejectedDraws);
				}
			}
		});
		check(ResolvedView.Observations.GBufferAttemptedDraws == ResolvedView.Observations.GBufferSuccessfulDraws + ResolvedView.Observations.GBufferRejectedDraws);
		check(ResolvedView.Observations.GBufferAttemptedDraws == ResolvedView.Observations.GBufferLocalAttemptedDraws + ResolvedView.Observations.GBufferSplineAttemptedDraws);
		return {bComplete, bRenderedGeometry,
			ResolvedView.Observations.GBufferAttemptedDraws,
			ResolvedView.Observations.GBufferSuccessfulDraws,
			ResolvedView.Observations.GBufferRejectedDraws,
			ResolvedView.Observations.GBufferSkippedDraws};
	}

	auto FStaticMeshRenderer::DrawGBufferSection_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FGBufferRenderer& GBuffer,
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		const FResolvedStaticMeshView& ResolvedView
	) -> bool
	{
		if (Primitive.LOD == nullptr || Primitive.VertexFactory == nullptr
			|| Item.Section == nullptr)
		{
			return false;
		}
		const FLocalVertexFactory& VertexFactory = *Primitive.VertexFactory;
		const FVertexDeclarationRHIRef VertexDeclaration(
			VertexFactory.GetDeclaration()
		);
		FGBufferRenderer::FPipeline* Pipeline =
			GBuffer.EnsurePipeline_RenderThread({.Material = Item.PipelineKey.Material, .Rasterizer = Item.PipelineKey.Rasterizer, .Depth = Item.PipelineKey.Depth, .VertexDeclaration = VertexDeclaration, .VertexDomain = Primitive.VertexDomain == EVertexDeformationDomain::Spline ? EGBufferVertexDomain::Spline : EGBufferVertexDomain::Local});
		if (Pipeline == nullptr) return false;

		FStaticMeshTransformUniform TransformUniform;
		TransformUniform.LocalToClip = Math::TransposeToFloat(
			View.ViewProjectionMatrix * Primitive.LocalToWorld
		);
		TransformUniform.LocalToWorld = Math::TransposeToFloat(Primitive.LocalToWorld);
		TransformUniform.NormalToWorld = Math::TransposeToFloat(
			Math::Transpose(Math::Inverse(Primitive.LocalToWorld))
		);
		TransformUniform.TransformParams.x = Math::LinearDeterminant(
			FMatrix4f(Primitive.LocalToWorld)) < 0.0f ?
												 -1.0f :
												 1.0f;
		const FRHIUniformBufferRange TransformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&TransformUniform, sizeof(TransformUniform)
			);
		const FSplineMeshUniform SplineUniform = MakeSplineMeshUniform(
			Primitive.VertexDomain == EVertexDeformationDomain::Spline ? Primitive.SplineDynamicData.Params : FSplineMeshParams{}
		);
		const FRHIUniformBufferRange SplineBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&SplineUniform, sizeof(SplineUniform)
			);
		FResolvedSurfaceMaterial SurfaceMaterial;
		const FMaterialRenderBinding* MaterialBinding =
			ResolvedView.GetMaterialBinding(Item);
		if (MaterialBinding == nullptr || !SurfaceMaterials.Resolve_RenderThread(
				*MaterialBinding, ESurfaceMaterialPass::GBuffer, true,
				View.Settings.Mode.bEnableSpecularAA,
				nullptr, nullptr, SurfaceMaterial)) return false;
		const FSurfaceMaterialUniform& MaterialUniform = SurfaceMaterial.Uniform;
		const FRHIUniformBufferRange MaterialBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&MaterialUniform, sizeof(MaterialUniform)
			);

		const FGBufferRenderer::FVertexParameters VertexParameters{
			.Transform = TransformBuffer,
			.SplineMesh = SplineBuffer
		};
		FGBufferRenderer::FFragmentParameters FragmentParameters;
		FragmentParameters.Material = MaterialBuffer;
		FragmentParameters.Textures = SurfaceMaterial.Textures;
		FragmentParameters.Samplers = SurfaceMaterial.Samplers;
		if (!GBuffer.BindPipeline_RenderThread(
				CommandList, *Pipeline, VertexParameters, FragmentParameters
			))
		{
			return false;
		}
		VertexFactory.BindStreams(CommandList);
		CommandList.BindIndexBuffer(Primitive.LOD->IndexBuffer.GetRHI(), 0);
		CommandList.DrawIndexed(
			Item.Section->IndexCount, Item.Section->FirstIndex, 0
		);
		return true;
	}

	auto FStaticMeshRenderer::DrawSection_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRHIUniformBufferRange& Lighting,
		ERenderMode RenderMode,
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		const FResolvedStaticMeshView& ResolvedView,
		bool bShadowDepth,
		bool bHybridRetained
	) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		check(Primitive.LOD != nullptr && Primitive.VertexFactory != nullptr && Item.Section != nullptr);
		const FStaticMeshLODResources& LOD = *Primitive.LOD;
		const FStaticMeshSection& Section = *Item.Section;
		const FMatrix& LocalToWorld = Primitive.LocalToWorld;
		const FMaterialRenderData& Material = Item.Material;
		const FMaterialRenderBinding* MaterialBinding =
			ResolvedView.GetMaterialBinding(Item);
		if (MaterialBinding == nullptr) return false;
		const FLocalVertexFactory& VertexFactory = *Primitive.VertexFactory;
		FStaticMeshTransformUniform TransformUniform;
		TransformUniform.LocalToClip = Math::TransposeToFloat(
			View.ViewProjectionMatrix * LocalToWorld
		);
		TransformUniform.LocalToWorld =
			Math::TransposeToFloat(LocalToWorld);
		TransformUniform.NormalToWorld = Math::TransposeToFloat(
			Math::Transpose(Math::Inverse(LocalToWorld))
		);
		const float TransformDeterminant = Math::LinearDeterminant(FMatrix4f(LocalToWorld));
		TransformUniform.TransformParams.x =
			TransformDeterminant < 0.0f ? -1.0f : 1.0f;
		const FRHIUniformBufferRange TransformUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&TransformUniform,
				sizeof(TransformUniform)
			);
		const FSplineMeshUniform SplineUniform = MakeSplineMeshUniform(
			Primitive.VertexDomain == EVertexDeformationDomain::Spline ? Primitive.SplineDynamicData.Params : FSplineMeshParams{}
		);
		const FRHIUniformBufferRange SplineUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&SplineUniform, sizeof(SplineUniform));

		VertexFactory.BindStreams(CommandList);
		CommandList.BindIndexBuffer(LOD.IndexBuffer.GetRHI(), 0);
		FEffectiveMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey) : Item.PipelineKey;
		EffectivePipelineKey.bHybridRetained =
			!bShadowDepth && bHybridRetained;
		auto* PipelineEntry = bShadowDepth ? State->ShadowPipelines.Find(EffectivePipelineKey) : State->Pipelines.Find(EffectivePipelineKey);
		FState::FPipelinePayload* Pipeline = PipelineEntry != nullptr ? PipelineEntry->Slot.GetPayload() : nullptr;
		if (Pipeline == nullptr)
		{
			return false;
		}

		CommandList.SetGraphicsPipelineState(*Pipeline->PipelineState);
		if (bShadowDepth)
		{
			const FRHIRasterizerState Rasterizer =
				MakeShadowRasterizerState(Item.PipelineKey.Rasterizer);
			CommandList.SetDepthBias(Rasterizer.DepthBiasConstantFactor, Rasterizer.DepthBiasClamp, Rasterizer.DepthBiasSlopeFactor);
		}

		if (Primitive.VertexDomain == EVertexDeformationDomain::Spline)
		{
			FSplineMeshVertexShader::FParameters Parameters;
			Parameters.Transform = TransformUniformBuffer;
			Parameters.SplineMesh = SplineUniformBuffer;
			SetShaderParameters(CommandList, Pipeline->SplineVertexShader, Parameters);
		}
		else
		{
			FStaticMeshVertexShader::FParameters Parameters;
			Parameters.Transform = TransformUniformBuffer;
			SetShaderParameters(CommandList, Pipeline->VertexShader, Parameters);
		}
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				   != EMaterialBlendMode::Masked)
		{
			return ExecuteMeshSurfacePass_RenderThread(
				CommandList, ESurfaceMaterialPass::OpaqueShadow, Lighting,
				nullptr, {}, Pipeline->FragmentShader,
				Pipeline->ShadowFragmentShader,
				[&] { CommandList.DrawIndexed(
					Section.IndexCount, Section.FirstIndex, 0); });
		}
		const bool bMaskedShadow = bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				== EMaterialBlendMode::Masked;
		FResolvedSurfaceMaterial SurfaceMaterial;
		if (!SurfaceMaterials.Resolve_RenderThread(
				*MaterialBinding,
				bMaskedShadow ? ESurfaceMaterialPass::MaskedShadow
					: ESurfaceMaterialPass::Forward,
				RenderMode == ERenderMode::Lit
					&& Material.PipelineIdentity.ShaderMap.ShadingModel
						== EMaterialShadingModel::Lit,
				View.Settings.Mode.bEnableSpecularAA,
				ResolvedView.DirectionalShadowTexture,
				ResolvedView.DirectionalShadowSampler,
				SurfaceMaterial)) return false;
		const FRHIUniformBufferRange MaterialUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&SurfaceMaterial.Uniform,
				sizeof(SurfaceMaterial.Uniform)
			);
		return ExecuteMeshSurfacePass_RenderThread(
			CommandList,
			bMaskedShadow ? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::Forward,
			Lighting, &SurfaceMaterial, MaterialUniformBuffer,
			Pipeline->FragmentShader, Pipeline->ShadowFragmentShader,
			[&] { CommandList.DrawIndexed(
				Section.IndexCount, Section.FirstIndex, 0); });
	}

	auto FStaticMeshRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}
} // namespace Durin
