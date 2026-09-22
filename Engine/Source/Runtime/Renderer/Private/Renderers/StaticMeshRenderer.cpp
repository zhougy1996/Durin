#include "Profiling/Profiling.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/StaticMeshDrawExecution.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"

namespace Durin
{
	using namespace RendererPrivate;

	struct FResolvedMeshPipeline
	{
		FMaterialShaderMap ShaderMap;
		std::shared_ptr<const FMeshVertexShaderBinding> VertexShader;
		TMaterialShaderRef<FSurfaceFragmentShader> FragmentShader;
		TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader>
			OpaqueShadowFragmentShader;
		TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
		FGraphicsPipelineStateRHIRef PipelineState;
	};

	struct FStaticMeshRenderer::FState
	{
		struct FShaderMapPayload
		{
			FMaterialShaderMap ShaderMap;
			std::shared_ptr<const FMeshVertexShaderBinding> VertexShader;
			TMaterialShaderRef<FSurfaceFragmentShader> FragmentShader;
			TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
		};

		using FPipelinePayload = std::shared_ptr<const FResolvedMeshPipeline>;

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
		ResolvedView.Draws.assign(PreparedView.GetNumSections(), {});
		ResolvedView.PrimitiveUniforms.clear();
		ResolvedView.MaterialUniforms.clear();
		ResolvedView.ViewUniforms = {};
		ResolvedView.Observations.PrimitiveUniformUploads = 0;
		ResolvedView.Observations.MaterialUniformUploads = 0;
		ResolvedView.Observations.ResourcePreparationAttemptedDraws =
			PreparedView.GetNumSections();
		ForEachBasePassBucket(PreparedView, [this, &PreparedView, &ResolvedView, bPrepareLitOpaqueForward](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedStaticMeshDraw& Item : Bucket)
			{
				FMaterialRenderBinding MaterialBinding;
				if (!ResolvePreparedMaterialBinding(Item.Command->Material, MaterialBinding,
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
					|| !Item.Command->bSupportsGBuffer
					|| Item.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel
						   != EMaterialShadingModel::Lit;
				const bool bReady = Primitive != nullptr
					&& (bNeedsForwardPipeline
						? EnsureSectionResources_RenderThread(*Primitive, Item,
							StoredBinding, Record.Pipeline)
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
		FResolvedStaticMeshView& ResolvedView
	) -> bool
	{
		check(IsInRenderingThread());
		bool bReady = true;
		for (auto& Record : ResolvedView.Draws) Record.HybridPipeline.reset();
		ForEachBasePassBucket(PreparedView, [this, &PreparedView,
			&ResolvedView, &bReady](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				if (Pass != EMeshBasePass::Translucent
					&& Draw.Command->bSupportsGBuffer
					&& Draw.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit)
					continue;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const FMaterialRenderBinding* MaterialBinding =
					ResolvedView.GetMaterialBinding(Draw);
				bReady = Primitive != nullptr && MaterialBinding != nullptr
						 && EnsureSectionResources_RenderThread(
							 *Primitive, Draw, *MaterialBinding,
							 ResolvedView.Draws[Draw.ResolvedIndex].HybridPipeline, false, true
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
		ResolvedView.Draws.assign(PreparedView.GetNumSections(), {});
		ResolvedView.PrimitiveUniforms.clear();
		ResolvedView.MaterialUniforms.clear();
		ResolvedView.ViewUniforms = {};
		ResolvedView.Observations.PrimitiveUniformUploads = 0;
		ResolvedView.Observations.MaterialUniformUploads = 0;
		ResolvedView.Observations.ResourcePreparationAttemptedDraws =
			PreparedView.GetNumSections();
		ForEachShadowBucket(PreparedView, [this, &PreparedView, &ResolvedView](const auto& Bucket) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				FMaterialRenderBinding MaterialBinding;
				if (!ResolvePreparedMaterialBinding(Draw.Command->Material, MaterialBinding,
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
						StoredBinding, Record.Pipeline, true);
				Record.bReady = bReady;
				ResolvedView.Observations.ResourcePreparationSuccessfulDraws +=
					bReady ? 1u : 0u;
			}
		});
		return FinalizeResourcePreparation(ResolvedView);
	}

	auto FStaticMeshRenderer::RecordShadow(FRHICommandList& CommandList,
		const FPreparedStaticMeshView& PreparedView, const FResolvedStaticMeshView& ResolvedView)
		-> FStaticMeshRenderObservations
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RecordShadow");
		check(CommandList.IsInsideRenderPass());
		FMeshDrawBindingGroup BindingGroup;
		FStaticMeshRenderObservations Observations;
		Observations.bRecordedOnWorker = !IsInRenderingThread();
		ForEachShadowBucket(PreparedView, [&](const auto& Bucket) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				++Observations.AttemptedDraws;
				const auto* Primitive = PreparedView.GetPrimitive(Draw);
				if (Primitive && ResolvedView.IsReady(Draw)
					&& RecordPreparedSection(CommandList, *Primitive, Draw, ResolvedView, true, false, &BindingGroup))
					++Observations.SuccessfulDraws;
				else ++Observations.RejectedDraws;
			}
		});
		check(Observations.AttemptedDraws == Observations.SuccessfulDraws + Observations.RejectedDraws);
		return Observations;
	}
	auto FStaticMeshRenderer::EnsureSectionResources_RenderThread(
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		const FMaterialRenderBinding& MaterialBinding,
		std::shared_ptr<const FResolvedMeshPipeline>& OutPipeline,
		bool bShadowDepth,
		bool bHybridRetained
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.EnsureSectionResources");
		OutPipeline.reset();
		check(IsInRenderingThread());
		if (Primitive.CollectedBinding == nullptr)
		{
			return false;
		}
		const FMaterialRenderData& Material = Item.Command->Material;
		const FVertexFactoryInputBinding& VertexFactory = *Primitive.CollectedBinding;
		const auto Factory = FindMeshVertexFactory(Item.Command->PipelineKey.FactoryKey);
		const uint32 MeshPass = bShadowDepth ? MaterialMeshPassShadow : MaterialMeshPassForward;
		if (!Factory || Factory->GetLayoutKey() != Item.Command->PipelineKey.LayoutKey || !Factory->GetShaderType(MeshPass)) return false;

		using FShaderMapResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		const FMeshShaderMapKey ShaderMapKey{
			.Material = Material.PlanningPassIdentity.ShaderMap,
			.VertexDomain = Primitive.VertexDomain,
			.FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey
		};
		auto& ShaderMapCache = bShadowDepth ? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderMapEntry = ShaderMapCache.FindOrAddBounded(
			ShaderMapKey, MaterialShaderMapCacheEntryBudget);
		FState::FShaderMapPayload* ShaderMapPayload =
			ShaderMapEntry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[this, &Material, Factory, MeshPass,
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
					FShaderType& VertexShaderType = *Factory->GetShaderType(MeshPass);
					FShaderType& FragmentShaderType =
						FSurfaceFragmentShader::StaticType();
					FShaderType& ShadowFragmentShaderType =
						FSurfaceMaskedShadowFragmentShader::StaticType();
					FShaderType& OpaqueShadowFragmentShaderType =
						FSurfaceOpaqueShadowFragmentShader::StaticType();
					FMaterialShaderMap ShaderMap;

					const bool bOpaqueShadow = bShadowDepth
						&& Identity.BlendMode != EMaterialBlendMode::Masked;
					const FShaderType& SelectedFragmentType = bOpaqueShadow
						? OpaqueShadowFragmentShaderType
						: bShadowDepth ? ShadowFragmentShaderType : FragmentShaderType;
					auto ShaderResult = InitializeMaterialShaderMap(
						VertexShaderType, SelectedFragmentType,
						Factory->GetType(),
						bShadowDepth ? MaterialMeshPassShadow : MaterialMeshPassForward,
						Identity,
						Coordinator.GetGeneration_RenderThread(),
						bOpaqueShadow ? nullptr : Material.CompiledProgram.get(),
						CompileOptions, ShaderMap);
					if (!ShaderResult)
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderCompile,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								std::move(ShaderResult.error()),
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::Manual
							)
						);
					}
					FState::FShaderMapPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader = Factory->Resolve(Candidate.ShaderMap, MeshPass);
					if (!bShadowDepth)
						Candidate.FragmentShader =
							TMaterialShaderRef<FSurfaceFragmentShader>(Candidate.ShaderMap);
					if (bShadowDepth && Identity.BlendMode == EMaterialBlendMode::Masked)
						Candidate.ShadowFragmentShader =
							TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader>(Candidate.ShaderMap);
					if (bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked)
						Candidate.OpaqueShadowFragmentShader =
							TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader>(Candidate.ShaderMap);
					if ((!Candidate.VertexShader || Candidate.VertexShader->GetRHIShader(false) == nullptr)
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
								ERenderResourceCreateErrorReason::ShaderCreationFailed,
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
			bShadowDepth ? MakeShadowPipelineKey(Item.Command->PipelineKey) : Item.Command->PipelineKey;
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
				FResolvedMeshPipeline Candidate;
				Candidate.ShaderMap = ShaderMapPayload->ShaderMap;
				Candidate.VertexShader = ShaderMapPayload->VertexShader;
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
				Initializer.BoundShaders.VertexShader = Candidate.VertexShader->GetRHIShader();
				Initializer.BoundShaders.FragmentShader = bShadowDepth ? (Identity.Material.ShaderMap.BlendMode
																				  == EMaterialBlendMode::Masked ?
																			  Candidate.ShadowFragmentShader.GetRHIShader() :
																			  Candidate.OpaqueShadowFragmentShader.GetRHIShader()) :
																		 Candidate.FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration = VertexFactory.Declaration;
				Initializer.PrimitiveTopology = Identity.Topology;
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
					FRenderPipelineRequestScope::Graphics(
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
							ERenderResourceCreateErrorReason::PipelineCreationFailed,
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual
						)
					);
				}
				return FPipelineResult::Success(std::make_shared<const FResolvedMeshPipeline>(std::move(Candidate)));
			},
			ReportRendererResourceCreateDiagnostic
		);
		if (Pipeline == nullptr)
		{
			return false;
		}

		const ESurfaceMaterialPass SurfacePass = bShadowDepth
			? (Item.Command->PipelineKey.Material.ShaderMap.BlendMode
					== EMaterialBlendMode::Masked
				? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::OpaqueShadow)
			: ESurfaceMaterialPass::Forward;
		if (!SurfaceMaterials.Ensure_RenderThread(MaterialBinding, SurfacePass)) return false;
		OutPipeline = *Pipeline;
		return true;
	}

	auto FStaticMeshRenderer::PrepareUniforms_RenderThread(FRHICommandListImmediate& CommandList,
		const FSceneView& View, const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView, bool bProductionDeferred,
		bool bGBuffer, bool bShadow) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareMeshUniformGroups");
		check(!CommandList.IsInsideRenderPass());
		if (!PrepareStaticMeshPrimitiveUniforms(CommandList, View, PreparedView, ResolvedView)) return false;
		ResolvedView.MaterialUniforms.clear();
		ResolvedView.MaterialUniforms.resize(PreparedView.MaterialUniformGroups.size());
		std::vector<std::array<bool, 3>> Requests(PreparedView.MaterialUniformGroups.size());
		ForEachBasePassBucket(PreparedView, [&](const auto& Bucket, EMeshBasePass Pass) {
			for (const auto& Draw : Bucket)
			{
				if (!ResolvedView.IsReady(Draw)) continue;
				auto& Required = Requests[Draw.MaterialUniformIndex];
				if (bShadow) Required[2] |= Pass == EMeshBasePass::Masked;
				else
				{
					const bool bDeferredDraw = Draw.Command->bSupportsGBuffer && Pass != EMeshBasePass::Translucent
						&& Draw.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel == EMaterialShadingModel::Lit;
					Required[0] |= !bProductionDeferred || !bDeferredDraw;
					Required[1] |= bGBuffer && bDeferredDraw;
				}
			}
		});
		for (uint32 Pass = 0; Pass < 3; ++Pass)
		{
			if (!std::ranges::any_of(Requests, [Pass](const auto& Required) { return Required[Pass]; })) continue;
			ResolvedView.ViewUniforms[Pass] = PrepareMeshViewUniform(CommandList, View,
				Pass == 1 || (Pass == 0 && View.Settings.Mode.RenderMode == ERenderMode::Lit));
			if (!ResolvedView.ViewUniforms[Pass].Buffer) return false;
		}
		for (uint32 Group = 0; Group < Requests.size(); ++Group)
		{
			const auto& Draw = PreparedView.GetDraw(PreparedView.MaterialUniformGroups[Group].RepresentativeDraw);
			const auto* Binding = ResolvedView.GetMaterialBinding(Draw);
			FRHIUniformBufferRange SharedMaterialUniform;
			for (uint32 Pass = 0; Pass < 3; ++Pass)
			{
				if (!Requests[Group][Pass]) continue;
				FPreparedStaticMeshSurfaceMaterial Material;
				if (!FStaticMeshSurfaceMaterialPreparer(CommandList, SurfaceMaterials, Binding)
					.Prepare(Pass == 0 ? ESurfaceMaterialPass::Forward : Pass == 1 ? ESurfaceMaterialPass::GBuffer : ESurfaceMaterialPass::MaskedShadow,
						Pass == 0 ? ResolvedView.DirectionalShadowTexture : nullptr,
						Pass == 0 ? ResolvedView.DirectionalShadowSampler : nullptr, Material, SharedMaterialUniform)) return false;
				if (!SharedMaterialUniform.Buffer)
				{
					SharedMaterialUniform = Material.Uniform;
					++ResolvedView.Observations.MaterialUniformUploads;
				}
				ResolvedView.MaterialUniforms[Group][Pass] = FResolvedStaticMeshView::FMaterialUniform{
					std::move(Material.Surface), Material.Uniform};
			}
		}
		DURIN_PROFILE_CPU_ZONE_TEXT(std::format("primitives={} material_groups={} primitive_uploads={} material_uploads={}",
			PreparedView.Primitives.size(), PreparedView.MaterialUniformGroups.size(),
			ResolvedView.Observations.PrimitiveUniformUploads, ResolvedView.Observations.MaterialUniformUploads));
		return true;
	}

	auto FStaticMeshRenderer::PrepareBindings_RenderThread(FRHICommandListImmediate& CommandList, FGBufferRenderer* GBuffer,
		const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView,
		const FRHIUniformBufferRange& Lighting, bool bShadow) -> bool
	{
		check(IsInRenderingThread());
		using FBindings = std::shared_ptr<const FPreparedSurfaceMaterialBindings>;
		std::map<std::tuple<uint32, uint32, FRHIShader*>, FBindings> Batches;
		std::map<std::pair<uint32, FRHIShader*>, std::shared_ptr<const FRHIShaderParameterBatch>> VertexBatches;
		ResolvedView.Observations.PreparedSurfaceBindingBatches = 0;
		ResolvedView.Observations.SurfaceBindingBatchReuses = 0;
		for (auto& Record : ResolvedView.Draws)
		{
			Record.VertexBindings.reset();
			Record.HybridVertexBindings.reset();
			Record.GBufferVertexBindings.reset();
			Record.SurfaceBindings.reset();
			Record.HybridSurfaceBindings.reset();
			Record.GBufferBindings.reset();
		}
		auto Prepare = [&](const FPreparedStaticMeshDraw& Draw, uint32 Pass, FRHIShader* Shader,
			const FCompiledSurfaceBindingLayout& Layout, FBindings& Out) -> bool {
			const auto Key = std::make_tuple(Draw.MaterialUniformIndex, Pass, Shader);
			if (const auto Existing = Batches.find(Key); Existing != Batches.end())
			{
				Out = Existing->second;
				++ResolvedView.Observations.SurfaceBindingBatchReuses;
				return true;
			}
			const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
			if (!Material) return false;
			auto Candidate = std::make_shared<FPreparedSurfaceMaterialBindings>();
			if (!PrepareCompiledSurfaceMaterial(Shader, Layout, Material->Surface, Material->Uniform,
				Lighting, {}, ResolvedView.ViewUniforms[Pass], *Candidate)) return false;
			Out = std::move(Candidate);
			Batches.emplace(Key, Out);
			++ResolvedView.Observations.PreparedSurfaceBindingBatches;
			return true;
		};
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			if (!ResolvedView.IsReady(Draw)) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			if (!Primitive || !Primitive->CollectedBinding) return false;
			auto PrepareVertex = [&](const std::shared_ptr<const FMeshVertexShaderBinding>& Shader,
				std::shared_ptr<const FRHIShaderParameterBatch>& Out) -> bool {
				if (!Shader || !Shader->GetRHIShader(false)) return false;
				const auto Key = std::make_pair(Draw.PrimitiveIndex, Shader->GetRHIShader(false));
				if (const auto Existing = VertexBatches.find(Key); Existing != VertexBatches.end())
				{
					Out = Existing->second;
					return true;
				}
				Out = Shader->Prepare(CommandList, ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex], *Primitive->CollectedBinding);
				if (!Out) return false;
				VertexBatches.emplace(Key, Out);
				return true;
			};
			if (Record.Pipeline && !PrepareVertex(Record.Pipeline->VertexShader, Record.VertexBindings)) return false;
			if (Record.HybridPipeline && !PrepareVertex(Record.HybridPipeline->VertexShader, Record.HybridVertexBindings)) return false;
			if (Record.GBufferPipeline && (!GBuffer || !PrepareVertex(
				GBuffer->GetVertexBinding(*Record.GBufferPipeline, *Primitive->CollectedBinding), Record.GBufferVertexBindings))) return false;
			if (bShadow)
			{
				if (Draw.Command->Pass != EMeshBasePass::Masked) continue;
				if (!Record.Pipeline || !Record.Pipeline->ShadowFragmentShader) return false;
				const auto& Shader = Record.Pipeline->ShadowFragmentShader;
				if (!Prepare(Draw, 2, Shader.GetRHIShader(false), Shader.GetShader()->GetSurfaceLayout(), Record.SurfaceBindings)) return false;
				continue;
			}
			if (ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][0])
			{
				for (bool bHybrid : {false, true})
				{
					const auto& Pipeline = bHybrid ? Record.HybridPipeline : Record.Pipeline;
					if (!Pipeline) continue;
					const auto& Shader = Pipeline->FragmentShader;
					if (!Shader || !Prepare(Draw, 0, Shader.GetRHIShader(false), Shader.GetShader()->GetSurfaceLayout(),
						bHybrid ? Record.HybridSurfaceBindings : Record.SurfaceBindings)) return false;
				}
			}
			if (Record.GBufferPipeline)
			{
				if (!GBuffer || !Prepare(Draw, 1, GBuffer->GetFragmentShader(*Record.GBufferPipeline),
					GBuffer->GetSurfaceLayout(*Record.GBufferPipeline), Record.GBufferBindings)) return false;
			}
		}
		return true;
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
		FMeshDrawBindingGroup BindingGroup;
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
				const bool bBucketMatches = Item.Command->Pass == Pass;
				const bool bSortKeyMatchesPass = Item.Command->SortKey.Pipeline[0]
												 == static_cast<uint32>(Pass);
				checkf(bBucketMatches, "StaticMesh prepared bucket does not match its pass.");
				checkf(bSortKeyMatchesPass, "StaticMesh prepared sort key does not match its bucket.");
				const bool bComplete = Primitive != nullptr
									   && Primitive->PrimitiveId != InvalidPrimitiveComponentId
									   && Primitive->CollectedBinding != nullptr
									   && Primitive->CollectedBinding->Declaration != nullptr
									   && Item.Command->Geometry.ElementCount != 0
									   && std::isfinite(Item.TranslucentSortDepth)
									   && Item.Command->PipelineKey.Material.ShaderMap
											  == Item.Command->Material.PlanningPassIdentity.ShaderMap
									   && Item.Command->PipelineKey.Material
											  == Item.Command->Material.PlanningPassIdentity;
				checkf(bComplete, "StaticMesh execution requires one complete prepared section.");
				if (!bBucketMatches || !bSortKeyMatchesPass || !bComplete
					|| !ResolvedView.IsReady(Item))
				{
					++ResolvedView.Observations.RejectedDraws;
					continue;
				}
				if (RecordPreparedSection(
						CommandList, *Primitive,
						Item, ResolvedView, false, false, &BindingGroup
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
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshDraw& Item, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView, bool bHybridRetained, FMeshDrawBindingGroup* BindingGroup
	) -> void
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		++ResolvedView.Observations.AttemptedDraws;
		const FPreparedStaticMeshPrimitive* Primitive =
			PreparedView.GetPrimitive(Item);
		const bool bComplete = Primitive != nullptr
							   && Primitive->PrimitiveId != InvalidPrimitiveComponentId
							   && Primitive->CollectedBinding != nullptr && Primitive->CollectedBinding->Declaration != nullptr
							   && Item.Command->Geometry.ElementCount != 0 && Item.Command->Pass == Pass
							   && Item.Command->SortKey.Pipeline[0] == static_cast<uint32>(Pass)
							   && Item.Command->PipelineKey.Material.ShaderMap == Item.Command->Material.PlanningPassIdentity.ShaderMap
							   && Item.Command->PipelineKey.Material == Item.Command->Material.PlanningPassIdentity;
		if (!bComplete || !ResolvedView.IsReady(Item))
		{
			++ResolvedView.Observations.RejectedDraws;
			return;
		}
		if (RecordPreparedSection(CommandList,
			*Primitive, Item, ResolvedView, false, bHybridRetained, BindingGroup))
			++ResolvedView.Observations.SuccessfulDraws;
		else
			++ResolvedView.Observations.RejectedDraws;
	}

	auto FStaticMeshRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView
	) -> void
	{
		FMeshDrawBindingGroup BindingGroup;
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
			return;
		const auto& Bucket = GetBasePassBucket(PreparedView, Pass);
		for (const FPreparedStaticMeshDraw& Draw : Bucket)
			ExecutePreparedDraw_RenderThread(CommandList, View, Lighting,
				RenderMode, Pass, Draw, PreparedView, ResolvedView, false, &BindingGroup);
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
		FMeshDrawBindingGroup BindingGroup;
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ExecuteGBuffer");
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		bool bComplete = true;
		bool bRenderedGeometry = false;
		auto RecordFamily = [](FResolvedStaticMeshView& Resolved,
							   const FPreparedStaticMeshDraw& Draw,
							   size_t FStaticMeshRenderObservations::* Local,
							   size_t FStaticMeshRenderObservations::* Spline) {
			++(Resolved.Observations.*(
				Draw.Command->PipelineKey.VertexDomain == EVertexDeformationDomain::Spline
					? Spline : Local));
		};
		for (const FPreparedStaticMeshDraw& Draw : PreparedView.Translucent)
		{
			++ResolvedView.Observations.GBufferSkippedDraws;
			RecordFamily(ResolvedView, Draw, &FStaticMeshRenderObservations::GBufferLocalSkippedDraws, &FStaticMeshRenderObservations::GBufferSplineSkippedDraws);
		}
		ForEachShadowBucket(PreparedView, [this, &CommandList, &View, &GBuffer, &PreparedView, &ResolvedView, &RecordFamily, &bComplete, &bRenderedGeometry, &BindingGroup](const auto& Bucket) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				if (!Draw.Command->bSupportsGBuffer || Draw.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel
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
						ResolvedView, BindingGroup
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

	auto FStaticMeshRenderer::PrepareGBufferPipelines_RenderThread(FGBufferRenderer& GBuffer, const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView) -> bool
	{
		for (auto& Record : ResolvedView.Draws) Record.GBufferPipeline.reset();
		bool Ready = true;
		ForEachShadowBucket(PreparedView, [&](const auto& Bucket) {
			for (const auto& Item : Bucket)
			{
				if (!Item.Command->bSupportsGBuffer || Item.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel != EMaterialShadingModel::Lit) continue;
				const auto* Primitive = PreparedView.GetPrimitive(Item);
				if (!Primitive) { Ready = false; continue; }
				const FStaticMeshGeometryBinding Geometry(*Primitive, Item);
				if (!Geometry.IsValid()) { Ready = false; continue; }
				auto Pipeline = GBuffer.EnsurePipeline_RenderThread({.Material = Item.Command->PipelineKey.Material, .CompiledProgram = Item.Command->Material.CompiledProgram, .Rasterizer = Item.Command->PipelineKey.Rasterizer, .Depth = Item.Command->PipelineKey.Depth, .VertexDeclaration = Geometry.GetVertexDeclaration(), .FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey, .Topology = Item.Command->PipelineKey.Topology});
				Ready = Pipeline && Ready;
				ResolvedView.Draws[Item.ResolvedIndex].GBufferPipeline = std::move(Pipeline);
			}
		});
		return Ready;
	}

	auto FStaticMeshRenderer::DrawGBufferSection_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FGBufferRenderer& GBuffer,
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		const FResolvedStaticMeshView& ResolvedView, FMeshDrawBindingGroup& BindingGroup
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.DrawGBufferSection");
		const FStaticMeshGeometryBinding Geometry(Primitive, Item);
		if (!Geometry.IsValid())
		{
			return false;
		}
		const auto& Record = ResolvedView.Draws[Item.ResolvedIndex];
		const auto& Pipeline = Record.GBufferPipeline;
		if (!Pipeline || !Record.GBufferBindings) return false;

		if (!BindingGroup.Apply(Pipeline.get(), Record.GBufferVertexBindings.get(), Record.GBufferBindings.get(), [&] {
			return GBuffer.BindPipeline_RenderThread(CommandList, *Pipeline,
				Record.GBufferVertexBindings, *Record.GBufferBindings);
		})) return false;
		Geometry.Bind(CommandList);
		Geometry.DrawIndexed(CommandList);
		return true;
	}

	auto FStaticMeshRenderer::RecordPreparedSection(
		FRHICommandList& CommandList,
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		const FResolvedStaticMeshView& ResolvedView,
		bool bShadowDepth,
		bool bHybridRetained, FMeshDrawBindingGroup* BindingGroup
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.DrawSection");
		check(CommandList.IsInsideRenderPass());
		const FStaticMeshGeometryBinding Geometry(Primitive, Item);
		check(Geometry.IsValid());

		Geometry.Bind(CommandList);
		if (Item.ResolvedIndex >= ResolvedView.Draws.size()) return false;
		const auto& Record = ResolvedView.Draws[Item.ResolvedIndex];
		const auto& Pipeline = !bShadowDepth && bHybridRetained
			? Record.HybridPipeline : Record.Pipeline;
		if (Pipeline == nullptr)
		{
			return false;
		}


		const auto& VertexBindings = !bShadowDepth && bHybridRetained ? Record.HybridVertexBindings : Record.VertexBindings;
		const bool bOpaqueShadow = bShadowDepth && Item.Command->PipelineKey.Material.ShaderMap.BlendMode != EMaterialBlendMode::Masked;
		const auto* FragmentBindings = bOpaqueShadow ? nullptr
			: (!bShadowDepth && bHybridRetained ? Record.HybridSurfaceBindings.get() : Record.SurfaceBindings.get());
		if (!VertexBindings || (!bOpaqueShadow && !FragmentBindings)) return false;
		FMeshDrawBindingGroup SingleDrawGroup;
		auto& Group = BindingGroup ? *BindingGroup : SingleDrawGroup;
		if (!Group.Apply(Pipeline.get(), VertexBindings.get(), FragmentBindings, [&] {
			CommandList.SetGraphicsPipelineState(*Pipeline->PipelineState);
			CommandList.SetPreparedShaderParameters(VertexBindings);
			return !FragmentBindings || FragmentBindings->Bind(CommandList);
		})) return false;
		if (bShadowDepth)
		{
			auto Rasterizer = MakeShadowRasterizerState(Item.Command->PipelineKey.Rasterizer);
			if (Item.RasterBias)
			{
				Rasterizer.DepthBiasConstantFactor = (*Item.RasterBias)[0];
				Rasterizer.DepthBiasSlopeFactor = (*Item.RasterBias)[1];
				Rasterizer.DepthBiasClamp = (*Item.RasterBias)[2];
			}
			CommandList.SetDepthBias(Rasterizer.DepthBiasConstantFactor, Rasterizer.DepthBiasClamp, Rasterizer.DepthBiasSlopeFactor);
		}

		Geometry.DrawIndexed(CommandList);
		return true;

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
