#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/StaticMeshDrawExecution.h"
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
			FMaterialShaderMap ShaderMap;
			TMaterialShaderRef<FStaticMeshVertexShader> VertexShader;
			TMaterialShaderRef<FSplineMeshVertexShader> SplineVertexShader;
			TMaterialShaderRef<FSurfaceFragmentShader> FragmentShader;
			TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
		};

		struct FPipelinePayload
		{
			FMaterialShaderMap ShaderMap;
			TMaterialShaderRef<FStaticMeshVertexShader> VertexShader;
			TMaterialShaderRef<FSplineMeshVertexShader> SplineVertexShader;
			TMaterialShaderRef<FSurfaceFragmentShader> FragmentShader;
			TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
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
					|| Item.Material.PlanningPassIdentity.ShaderMap.ShadingModel
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
					&& Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
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
			.Material = Material.PlanningPassIdentity.ShaderMap,
			.VertexDomain = Primitive.VertexDomain
		};
		auto& ShaderMapCache = bShadowDepth ? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderMapEntry = ShaderMapCache.FindOrAddBounded(
			ShaderMapKey, MaterialShaderMapCacheEntryBudget);
		FState::FShaderMapPayload* ShaderMapPayload =
			ShaderMapEntry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[this, &Material, Domain = Primitive.VertexDomain,
				 bShadowDepth]() -> FShaderMapResult {
					const FMaterialShaderMapIdentity& Identity =
						Material.PlanningPassIdentity.ShaderMap;
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
					FMaterialShaderMap ShaderMap;
					std::string ErrorMessage;
					const bool bOpaqueShadow = bShadowDepth
						&& Identity.BlendMode != EMaterialBlendMode::Masked;
					const FShaderType& SelectedFragmentType = bOpaqueShadow
						? OpaqueShadowFragmentShaderType
						: bShadowDepth ? ShadowFragmentShaderType : FragmentShaderType;
					const bool bInitialized = InitializeMaterialShaderMap(
						VertexShaderType, SelectedFragmentType,
						Domain == EVertexDeformationDomain::Spline
							? GetSplineVertexFactoryShaderType()
							: GetLocalVertexFactoryShaderType(),
						bShadowDepth ? MaterialMeshPassShadow : MaterialMeshPassForward,
						Identity,
						Coordinator.GetGeneration_RenderThread(),
						bOpaqueShadow ? nullptr : Material.CompiledProgram.get(),
						CompileOptions, ShaderMap, ErrorMessage);
					if (!bInitialized)
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
					FState::FShaderMapPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					if (Domain == EVertexDeformationDomain::Spline)
						Candidate.SplineVertexShader =
							TMaterialShaderRef<FSplineMeshVertexShader>(Candidate.ShaderMap);
					else
						Candidate.VertexShader =
							TMaterialShaderRef<FStaticMeshVertexShader>(Candidate.ShaderMap);
					if (!bShadowDepth)
						Candidate.FragmentShader =
							TMaterialShaderRef<FSurfaceFragmentShader>(Candidate.ShaderMap);
					if (bShadowDepth && Identity.BlendMode == EMaterialBlendMode::Masked)
						Candidate.ShadowFragmentShader =
							TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader>(Candidate.ShaderMap);
					if (bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked)
						Candidate.OpaqueShadowFragmentShader =
							TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader>(Candidate.ShaderMap);
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
		auto& PipelineEntry = PipelineCache.FindOrAddBounded(
			EffectivePipelineKey, MaterialPipelineCacheEntryBudget);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader =
			ShaderMapPayload->ShaderMap.GetGeneration().Shader;
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
				Initializer.RenderTargetLayout = bShadowDepth
					? RenderTargetLayouts::MakeDirectionalShadowDepth()
					: (Identity.bHybridRetained
						? (Identity.Material.ShaderMap.BlendMode
								== EMaterialBlendMode::Translucent
							? RenderTargetLayouts::MakeHybridSortedTranslucency()
							: RenderTargetLayouts::MakeHybridRetainedForward())
						: RenderTargetLayouts::MakeSceneTargets());
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
				Initializer.PipelineLayout = Candidate.ShaderMap.GetPipelineLayout();
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
											  == Item.Material.PlanningPassIdentity.ShaderMap
									   && Item.PipelineKey.Material
											  == Item.Material.PlanningPassIdentity;
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
							   && Item.ShaderMapIdentity == Item.Material.PlanningPassIdentity.ShaderMap
							   && Item.PipelineKey.Material == Item.Material.PlanningPassIdentity;
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
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
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
		const FStaticMeshGeometryBinding Geometry(Primitive, Item);
		if (!Geometry.IsValid())
		{
			return false;
		}
		FGBufferRenderer::FPipeline* Pipeline =
			GBuffer.EnsurePipeline_RenderThread({.Material = Item.PipelineKey.Material, .CompiledProgram = Item.Material.CompiledProgram, .Rasterizer = Item.PipelineKey.Rasterizer, .Depth = Item.PipelineKey.Depth, .VertexDeclaration = Geometry.GetVertexDeclaration(), .VertexDomain = Primitive.VertexDomain == EVertexDeformationDomain::Spline ? EGBufferVertexDomain::Spline : EGBufferVertexDomain::Local});
		if (Pipeline == nullptr) return false;

		const FStaticMeshPrimitiveUniformBindings PrimitiveUniforms =
			FStaticMeshPrimitiveUniformPreparer(CommandList, View).Prepare(Primitive);
		const FMaterialRenderBinding* MaterialBinding =
			ResolvedView.GetMaterialBinding(Item);
		FPreparedStaticMeshSurfaceMaterial Material;
		if (!FStaticMeshSurfaceMaterialPreparer(
				CommandList, SurfaceMaterials, MaterialBinding
			).Prepare(ESurfaceMaterialPass::GBuffer, true,
				View.Settings.Mode.bEnableSpecularAA, nullptr, nullptr, Material))
		{
			return false;
		}

		const FGBufferRenderer::FVertexParameters VertexParameters{
			.Transform = PrimitiveUniforms.Transform,
			.SplineMesh = PrimitiveUniforms.SplineMesh
		};
		FGBufferRenderer::FFragmentParameters FragmentParameters;
		FragmentParameters.Material = Material.Uniform;
		FragmentParameters.Textures = Material.Surface.Textures;
		FragmentParameters.Samplers = Material.Surface.Samplers;
		if (!GBuffer.BindPipeline_RenderThread(
				CommandList, *Pipeline, VertexParameters, FragmentParameters
			))
		{
			return false;
		}
		Geometry.Bind(CommandList);
		Geometry.DrawIndexed(CommandList);
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
		const FStaticMeshGeometryBinding Geometry(Primitive, Item);
		check(Geometry.IsValid());
		const FMaterialRenderData& Material = Item.Material;
		const FMaterialRenderBinding* MaterialBinding =
			ResolvedView.GetMaterialBinding(Item);
		const FStaticMeshSurfaceMaterialPreparer MaterialPreparer(
			CommandList, SurfaceMaterials, MaterialBinding
		);
		if (!MaterialPreparer.IsValid()) return false;
		const FStaticMeshPrimitiveUniformBindings PrimitiveUniforms =
			FStaticMeshPrimitiveUniformPreparer(CommandList, View).Prepare(Primitive);

		Geometry.Bind(CommandList);
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
			Parameters.Transform = PrimitiveUniforms.Transform;
			Parameters.SplineMesh = PrimitiveUniforms.SplineMesh;
			SetShaderParameters(CommandList, Pipeline->SplineVertexShader, Parameters);
		}
		else
		{
			FStaticMeshVertexShader::FParameters Parameters;
			Parameters.Transform = PrimitiveUniforms.Transform;
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
				[&] { Geometry.DrawIndexed(CommandList); });
		}
		const bool bMaskedShadow = bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				== EMaterialBlendMode::Masked;
		FPreparedStaticMeshSurfaceMaterial PreparedMaterial;
		if (!MaterialPreparer.Prepare(
				bMaskedShadow ? ESurfaceMaterialPass::MaskedShadow
					: ESurfaceMaterialPass::Forward,
				RenderMode == ERenderMode::Lit
					&& Material.PlanningPassIdentity.ShaderMap.ShadingModel
						== EMaterialShadingModel::Lit,
				View.Settings.Mode.bEnableSpecularAA,
				ResolvedView.DirectionalShadowTexture,
				ResolvedView.DirectionalShadowSampler,
				PreparedMaterial)) return false;
		return ExecuteMeshSurfacePass_RenderThread(
			CommandList,
			bMaskedShadow ? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::Forward,
			Lighting, &PreparedMaterial.Surface, PreparedMaterial.Uniform,
			Pipeline->FragmentShader, Pipeline->ShadowFragmentShader,
			[&] { Geometry.DrawIndexed(CommandList); });
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
