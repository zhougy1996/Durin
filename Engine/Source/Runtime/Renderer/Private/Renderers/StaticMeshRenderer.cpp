#include "Profiling/Profiling.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/StaticMeshDrawExecution.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialParameterCollection.h"

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
		struct FPrimitiveState
		{
			FMatrix LocalToWorld;
			FMatrix WorldToLocal;
			FMatrix4f NormalToWorld;
			FVector3 BoundsCenter;
			auto operator==(const FPrimitiveState&) const -> bool = default;
		};
		static auto CapturePrimitiveState(const FPreparedStaticMeshPrimitive& Primitive) -> FPrimitiveState
		{
			return {Primitive.LocalToWorld, Primitive.WorldToLocal, Primitive.NormalToWorld, Primitive.BoundsCenter};
		}
		// A complete stable draw, independent of the owning camera/cascade pass.
		struct FMeshDrawCommand
		{
			std::shared_ptr<const FMeshDrawDescription> Description;
			std::shared_ptr<const FVertexFactoryInputBinding> Input;
			FRHIUniformBufferRange PrimitiveUniform;
			FResolvedMeshDrawRecord Record;
			FPrimitiveState PrimitiveState;
			std::array<std::optional<FResolvedStaticMeshView::FMaterialUniform>, 3> Materials;
		};
		struct FPrimitiveUniform
		{
			FPrimitiveState Value;
			FRHIUniformBufferRange Range;
			uint64 LastUsed = 0;
		};
		struct FMaterialUniform
		{
			FMaterialRenderRepresentation Owner;
			FRHIUniformBufferRange Range;
			uint64 LastUsed = 0;
		};
		struct FCommandEntry
		{
			std::shared_ptr<const FMeshDrawCommand> Command;
			uint64 LastUsed = 0;
		};
		// Immutable ranges retain their source, so different worlds and in-flight frames
		// never observe another snapshot's data, even when their version numbers match.
		struct FCollectionUniform
		{
			std::shared_ptr<const FMaterialParameterCollectionSnapshot> Snapshot;
			FMaterialParameterCollectionLayout Defaults;
			FRHIUniformBufferRange Range;
			uint64 LastUsed = 0;
		};
		uint64 CacheSerial = 0;
		template<typename TCache>
		static auto TrimCache(TCache& Cache) -> void
		{
			if (Cache.size() <= 512) return;
			const auto Oldest = std::ranges::min_element(Cache, {}, [](const auto& Entry) { return Entry.second.LastUsed; });
			Cache.erase(Oldest);
		}
		FRenderResourceGeneration DrawGeneration;
		bool bPersistentDraws = [] {
			const char* Setting = std::getenv("DURIN_MESH_DRAW_CACHE");
			return !Setting || std::string_view(Setting) != "off";
		}();
		std::map<std::pair<uint64, uint64>, FPrimitiveUniform> PrimitiveUniforms;
		std::map<uint64, FMaterialUniform> MaterialUniforms;
		std::map<std::tuple<FGuid, bool, uint64>, FCollectionUniform> CollectionUniforms;
		using FDrawKey = std::array<uint64, 7>;
		static auto MakeDrawKey(const FPreparedStaticMeshDraw& Draw,
			const FPreparedStaticMeshPrimitive& Primitive, const FResolvedMeshDrawRecord& Record) -> FDrawKey
		{
			return {reinterpret_cast<uint64>(Draw.Command.get()), Primitive.PrimitiveId.Value, Primitive.BatchId,
				reinterpret_cast<uint64>(Record.Pipeline.get()), reinterpret_cast<uint64>(Record.HybridPipeline.get()),
				reinterpret_cast<uint64>(Record.GBufferPipeline.get()), reinterpret_cast<uint64>(Record.GBufferGPUCullingPipeline.get())};
		}
		auto IsCacheable(const FPreparedStaticMeshDraw& Draw, const FPreparedStaticMeshPrimitive& Primitive) const -> bool
		{
			return bPersistentDraws && Primitive.GeometryRecord && Primitive.VertexDomain == EVertexDeformationDomain::Local;
		}
		std::map<FDrawKey, FCommandEntry> DrawCommands;
		auto ResetDrawCommands() -> void
		{
			DrawCommands.clear();
			PrimitiveUniforms.clear();
			MaterialUniforms.clear();
			CollectionUniforms.clear();
			CacheSerial = 0;
		}

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
						Material.CompiledProgram.get(),
						CompileOptions, ShaderMap);
					if (ShaderResult.Status == EShaderBuildStatus::Canceled) return FShaderMapResult::Canceled();
				if (!ShaderResult)
					{
						return FShaderMapResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::ShaderCompile,
								"StaticMeshShaderMap",
								GetIdentityText(Identity),
								ShaderResult.Diagnostic,
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
							? RenderTargetLayouts::MakeGraphSortedTranslucency()
							: RenderTargetLayouts::MakeGraphHybridRetainedForward())
						: RenderTargetLayouts::MakeGraphSceneTargets());
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
		FScene* Scene, const FSceneView& View, const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView, bool bProductionDeferred,
		bool bGBuffer, bool bShadow) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareMeshUniformGroups");
		check(!CommandList.IsInsideRenderPass());
		const auto Generation = Coordinator.GetGeneration_RenderThread();
		if (State->DrawGeneration != Generation)
		{
			State->ResetDrawCommands();
			State->DrawGeneration = Generation;
		}
		ResolvedView.PrimitiveUniforms.clear();
		ResolvedView.PrimitiveUniforms.resize(PreparedView.Primitives.size());
		ResolvedView.PersistentCommandsReused.assign(PreparedView.GetNumSections(), false);
		ResolvedView.PersistentCollectionBindingsChanged.assign(PreparedView.GetNumSections(), false);
		ResolvedView.MaterialUniforms.clear();
		ResolvedView.MaterialUniforms.resize(PreparedView.MaterialUniformGroups.size());
		ResolvedView.Observations.SurfaceMaterialPreparations = 0;
		std::vector<std::array<bool, 3>> Requests(PreparedView.MaterialUniformGroups.size());
		std::vector<std::vector<FRHIUniformBufferRange>> GroupCollections(PreparedView.MaterialUniformGroups.size());
		std::map<std::pair<FGuid, uint64>, FRHIUniformBufferRange> CollectionUniforms;
		ForEachBasePassBucket(PreparedView, [&](const auto& Bucket, EMeshBasePass Pass) {
			for (const auto& Draw : Bucket)
			{
				if (!ResolvedView.IsReady(Draw)) continue;
				auto& Required = Requests[Draw.MaterialUniformIndex];
				if (bShadow) Required[2] = true;
				else
				{
					const bool bDeferredDraw = Draw.Command->bSupportsGBuffer && Pass != EMeshBasePass::Translucent
						&& Draw.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel == EMaterialShadingModel::Lit;
					Required[0] |= !bProductionDeferred || !bDeferredDraw;
					Required[1] |= bGBuffer && bDeferredDraw;
				}
			}
		});
		// Resolve and validate current world data before any command hit. Snapshot identity,
		// rather than its world-local version, owns the immutable allocation cache key.
		for (uint32 Group = 0; Group < Requests.size(); ++Group)
		{
			if (!std::ranges::any_of(Requests[Group], [](bool Required) { return Required; })) continue;
			const auto& Draw = PreparedView.GetDraw(PreparedView.MaterialUniformGroups[Group].RepresentativeDraw);
			const auto Program = Draw.Command->Material.CompiledProgram;
			if (!Program) continue;
			for (const auto& Required : Program->ActiveCollections)
			{
				const auto Snapshot = Scene ? Scene->GetMaterialParameterCollection_RenderThread(Required.CollectionId) : nullptr;
				if (Snapshot && !Snapshot->Layout.HasCompatibleSchema(Required)) return false;
				const FByteView Payload = Snapshot ? FByteView(Snapshot->Payload) : FByteView(Required.DefaultPayload);
				if (Payload.size() != Required.UniformLayout.UniformPayloadSize) return false;
				// Default payloads can differ across retained programs with the same
				// Collection ID. Only live world snapshots share by Collection alone.
				const auto LocalKey = std::make_pair(Required.CollectionId, Snapshot
					? reinterpret_cast<uint64>(Snapshot.get()) : reinterpret_cast<uint64>(&Required));
				auto Local = CollectionUniforms.find(LocalKey);
				if (Local == CollectionUniforms.end())
				{
					const auto Key = std::make_tuple(Required.CollectionId, static_cast<bool>(Snapshot),
						Snapshot ? reinterpret_cast<uint64>(Snapshot.get()) : FXxHash64::HashBuffer(Payload).HashValue);
					FRHIUniformBufferRange Range;
					if (State->bPersistentDraws)
						if (const auto Existing = State->CollectionUniforms.find(Key); Existing != State->CollectionUniforms.end()
							&& (Snapshot || (Existing->second.Defaults.HasCompatibleSchema(Required)
								&& Existing->second.Defaults.DefaultPayload == Required.DefaultPayload)))
						{
							Range = Existing->second.Range;
							Existing->second.LastUsed = ++State->CacheSerial;
						}
					if (!Range.Buffer)
					{
						Range = CommandList.CreateUniformBufferRange(Payload.data(), Payload.size());
						if (!Range.Buffer) return false;
						if (State->bPersistentDraws)
						{
							State->CollectionUniforms.insert_or_assign(Key, FState::FCollectionUniform{
								Snapshot, Snapshot ? FMaterialParameterCollectionLayout{} : Required, Range, ++State->CacheSerial});
							FState::TrimCache(State->CollectionUniforms);
						}
					}
					Local = CollectionUniforms.emplace(LocalKey, std::move(Range)).first;
				}
				GroupCollections[Group].push_back(Local->second);
			}
		}
		// Probe complete stable commands before constructing uniforms or resolving surfaces.
		// Pipelines already reflect the current pass policy and resource generation.
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			if (!ResolvedView.IsReady(Draw) || !Primitive || !State->IsCacheable(Draw, *Primitive)) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			const auto Existing = State->DrawCommands.find(FState::MakeDrawKey(Draw, *Primitive, Record));
			if (Existing == State->DrawCommands.end()) continue;
			const auto& Cached = *Existing->second.Command;
			if (Cached.Input != Primitive->CollectedBinding
				|| Cached.PrimitiveState != FState::CapturePrimitiveState(*Primitive)) continue;
			const auto& Required = Requests[Draw.MaterialUniformIndex];
			bool bCurrent = true;
			for (uint32 Pass = 0; Pass < 3; ++Pass)
				if (Required[Pass] && (!Cached.Materials[Pass]
					|| !SurfaceMaterials.AreStableResourcesCurrent_RenderThread(Cached.Materials[Pass]->Surface)))
					bCurrent = false;
			if (!bCurrent) continue;
			Record = Cached.Record;
			ResolvedView.PersistentCommandsReused[Draw.ResolvedIndex] = true;
			ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex] = Cached.PrimitiveUniform;
			for (uint32 Pass = 0; Pass < 3; ++Pass)
			{
				auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				if (!Required[Pass]) continue;
				const auto& CurrentCollections = GroupCollections[Draw.MaterialUniformIndex];
				const bool bSameCollections = std::ranges::equal(Cached.Materials[Pass]->CollectionUniforms, CurrentCollections,
					[](const auto& A, const auto& B) { return A.Buffer == B.Buffer && A.Offset == B.Offset && A.Size == B.Size; });
				ResolvedView.PersistentCollectionBindingsChanged[Draw.ResolvedIndex] =
					ResolvedView.PersistentCollectionBindingsChanged[Draw.ResolvedIndex] || !bSameCollections;
				if (Material) continue;
				Material = Cached.Materials[Pass];
				Material->CollectionUniforms = CurrentCollections;
				if (!SurfaceMaterials.RefreshPassResources_RenderThread(
					Pass == 0 ? ESurfaceMaterialPass::Forward : ESurfaceMaterialPass::GBuffer,
					ResolvedView.DirectionalShadowTexture, ResolvedView.DirectionalShadowSampler, Material->Surface)) return false;
			}
			Existing->second.LastUsed = ++State->CacheSerial;
		}
		for (uint32 Index = 0; Index < PreparedView.Primitives.size(); ++Index)
		{
			if (ResolvedView.PrimitiveUniforms[Index].Buffer)
			{
				++ResolvedView.Observations.PrimitiveUniformReuses;
				continue;
			}
			const auto& Primitive = PreparedView.Primitives[Index];
			const auto Value = FState::CapturePrimitiveState(Primitive);
			const auto Key = std::make_pair(Primitive.PrimitiveId.Value, Primitive.BatchId);
			const bool bCacheable = State->bPersistentDraws && Primitive.GeometryRecord
				&& Primitive.VertexDomain == EVertexDeformationDomain::Local;
			const auto Existing = State->PrimitiveUniforms.find(Key);
			FRHIUniformBufferRange Range;
			if (bCacheable && Existing != State->PrimitiveUniforms.end() && Existing->second.Value == Value)
			{
				Range = Existing->second.Range;
				Existing->second.LastUsed = ++State->CacheSerial;
				++ResolvedView.Observations.PrimitiveUniformReuses;
			}
			else
			{
				const auto Uniform = BuildMeshTransformUniform(Primitive);
				Range = CommandList.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
				if (!Range.Buffer) return false;
				++ResolvedView.Observations.PrimitiveUniformUploads;
				if (bCacheable)
				{
					State->PrimitiveUniforms.insert_or_assign(Key, FState::FPrimitiveUniform{Value, Range, ++State->CacheSerial});
					FState::TrimCache(State->PrimitiveUniforms);
				}
			}
			ResolvedView.PrimitiveUniforms[Index] = std::move(Range);
		}
		for (uint32 Pass = 0; Pass < 3; ++Pass)
		{
			if (!std::ranges::any_of(Requests, [Pass](const auto& Required) { return Required[Pass]; })) continue;
			ResolvedView.ViewUniforms[Pass] = PrepareMeshViewUniform(CommandList, View,
				Pass == 1 || (Pass == 0 && View.Settings.Mode.RenderMode == ERenderMode::Lit));
			if (!ResolvedView.ViewUniforms[Pass].Buffer) return false;
		}
		for (uint32 Group = 0; Group < Requests.size(); ++Group)
		{
			bool bNeedsPreparation = false;
			for (uint32 Pass = 0; Pass < 3; ++Pass)
				bNeedsPreparation |= Requests[Group][Pass] && !ResolvedView.MaterialUniforms[Group][Pass];
			if (!bNeedsPreparation) continue;
			if (!std::ranges::any_of(Requests[Group], [](bool bRequired) {
				return bRequired;
			})) continue;
			const auto& Draw = PreparedView.GetDraw(PreparedView.MaterialUniformGroups[Group].RepresentativeDraw);
			const auto* Binding = ResolvedView.GetMaterialBinding(Draw);
			const auto& RequiredCollections = GroupCollections[Group];
			FRHIUniformBufferRange SharedMaterialUniform;
			const uint64 MaterialId = Draw.Command->Material.Representation.GetRecordId();
			if (State->bPersistentDraws && MaterialId != 0)
				if (const auto Cached = State->MaterialUniforms.find(MaterialId); Cached != State->MaterialUniforms.end())
				{
					SharedMaterialUniform = Cached->second.Range;
					Cached->second.LastUsed = ++State->CacheSerial;
				}
			for (uint32 Pass = 0; Pass < 3; ++Pass)
			{
				if (!Requests[Group][Pass] || ResolvedView.MaterialUniforms[Group][Pass]) continue;
				FPreparedStaticMeshSurfaceMaterial Material;
				++ResolvedView.Observations.SurfaceMaterialPreparations;
				if (!FStaticMeshSurfaceMaterialPreparer(CommandList, SurfaceMaterials, Binding)
					.Prepare(Pass == 0 ? ESurfaceMaterialPass::Forward : Pass == 1 ? ESurfaceMaterialPass::GBuffer : ESurfaceMaterialPass::MaskedShadow,
						Pass == 0 ? ResolvedView.DirectionalShadowTexture : nullptr,
						Pass == 0 ? ResolvedView.DirectionalShadowSampler : nullptr, Material, SharedMaterialUniform)) return false;
				if (!SharedMaterialUniform.Buffer)
				{
					SharedMaterialUniform = Material.Uniform;
					++ResolvedView.Observations.MaterialUniformUploads;
					if (State->bPersistentDraws && MaterialId != 0)
					{
						State->MaterialUniforms.insert_or_assign(MaterialId,
							FState::FMaterialUniform{Draw.Command->Material.Representation, SharedMaterialUniform, ++State->CacheSerial});
						FState::TrimCache(State->MaterialUniforms);
					}
				}
				ResolvedView.MaterialUniforms[Group][Pass] = FResolvedStaticMeshView::FMaterialUniform{
					std::move(Material.Surface), Material.Uniform, RequiredCollections};
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
		std::map<std::tuple<uint32, uint32, uint32, FRHIShader*>, FBindings> Batches;
		std::map<std::pair<uint32, FRHIShader*>, FBindings> PassBatches;
		std::map<std::tuple<uint32, uint32, uint32, FRHIShader*>, std::shared_ptr<const FRHIShaderParameterBatch>> VertexBatches;
		ResolvedView.Observations.PreparedSurfaceBindingBatches = 0;
		ResolvedView.Observations.SurfaceBindingBatchReuses = 0;
		for (uint32 Index = 0; Index < ResolvedView.Draws.size(); ++Index)
		{
			if (ResolvedView.PersistentCommandsReused[Index] && !ResolvedView.PersistentCollectionBindingsChanged[Index]) continue;
			auto& Record = ResolvedView.Draws[Index];
			Record.VertexBindings.reset();
			Record.HybridVertexBindings.reset();
			Record.GBufferVertexBindings.reset();
			Record.SurfaceBindings.reset();
			Record.HybridSurfaceBindings.reset();
			Record.GBufferBindings.reset();
			Record.GBufferGPUCullingBindings.reset();
		}
		auto Prepare = [&](const FPreparedStaticMeshDraw& Draw, uint32 Pass, FRHIShader* Shader,
			const FCompiledSurfaceBindingLayout& Layout, FBindings& Out) -> bool {
			const bool bUsesPrimitive = std::ranges::any_of(Layout.GetEntries(), [](const auto& Entry) {
				return Entry.Source == FCompiledSurfaceBindingLayout::ESource::Primitive;
			});
			const auto Key = std::make_tuple(Draw.MaterialUniformIndex, Pass,
				bUsesPrimitive ? Draw.PrimitiveIndex : std::numeric_limits<uint32>::max(), Shader);
			if (const auto Existing = Batches.find(Key); Existing != Batches.end())
			{
				Out = Existing->second;
				++ResolvedView.Observations.SurfaceBindingBatchReuses;
				return true;
			}
			const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
			if (!Material || Draw.PrimitiveIndex >= ResolvedView.PrimitiveUniforms.size()) return false;
			auto Candidate = std::make_shared<FPreparedSurfaceMaterialBindings>();
			if (!PrepareCompiledSurfaceMaterial(Shader, Layout.ForSet(1), Material->Surface, Material->Uniform,
				Lighting, {}, ResolvedView.ViewUniforms[Pass],
				ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex],
				Material->CollectionUniforms, *Candidate)) return false;
			Out = std::move(Candidate);
			Batches.emplace(Key, Out);
			++ResolvedView.Observations.PreparedSurfaceBindingBatches;
			return true;
		};
		auto PreparePass = [&](const FPreparedStaticMeshDraw& Draw, uint32 Pass, FRHIShader* Shader,
			const FCompiledSurfaceBindingLayout& Layout, FBindings& Out) -> bool {
			const auto Key = std::make_pair(Pass, Shader);
			if (const auto Existing = PassBatches.find(Key); Existing != PassBatches.end())
			{
				Out = Existing->second;
				return true;
			}
			const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
			if (!Material) return false;
			auto Candidate = std::make_shared<FPreparedSurfaceMaterialBindings>();
			if (!PrepareCompiledSurfaceMaterial(Shader, Layout.ForSet(0), Material->Surface, {},
				Lighting, {}, ResolvedView.ViewUniforms[Pass], {}, {}, *Candidate)) { DURIN_ERROR("Mesh pass binding failed: pass={}, valid={}, entries={}", Pass, Layout.IsValid(), Layout.ForSet(0).GetEntries().size()); return false; }
			Out = std::move(Candidate);
			PassBatches.emplace(Key, Out);
			return true;
		};
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			if (!ResolvedView.IsReady(Draw)) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			if (!Primitive || !Primitive->CollectedBinding) return false;
			const uint32 CommandPass = bShadow ? 2u : Record.GBufferPipeline ? 1u : 0u;
			const auto& CommandMaterial = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][CommandPass];
			if (!CommandMaterial) { DURIN_ERROR("Mesh command has no material slot {}", CommandPass); return false; }
			const bool bCacheable = State->IsCacheable(Draw, *Primitive);
			const auto CommandKey = FState::MakeDrawKey(Draw, *Primitive, Record);
			auto PreparePasses = [&]() {
				auto Vertex = [&](const std::shared_ptr<const FMeshVertexShaderBinding>& Shader, uint32 Pass, FBindings& Out) {
					if (!Shader || !Shader->GetReflection()) return false;
					if (const auto Existing = PassBatches.find({Pass, Shader->GetRHIShader(false)}); Existing != PassBatches.end())
					{
						Out = Existing->second;
						return true;
					}
					auto Reflection = *Shader->GetReflection();
					std::erase_if(Reflection.ResourceBindings, [](const auto& Binding) { return Binding.SetIndex != 0; });
					return PreparePass(Draw, Pass, Shader->GetRHIShader(false), FCompiledSurfaceBindingLayout(Reflection), Out);
				};
				if (Record.Pipeline && !Vertex(Record.Pipeline->VertexShader, bShadow ? 2u : 0u, Record.VertexPassBindings)) return false;
				if (Record.HybridPipeline && !Vertex(Record.HybridPipeline->VertexShader, 0u, Record.HybridVertexPassBindings)) return false;
				if (bShadow)
				{
					if (Draw.Command->Pass == EMeshBasePass::Masked)
					{
						const auto& Shader = Record.Pipeline->ShadowFragmentShader;
						if (!PreparePass(Draw, 2u, Shader.GetRHIShader(false), Shader.GetShader()->GetSurfaceLayout(), Record.FragmentPassBindings)) return false;
					}
					return true;
				}
				for (bool bHybrid : {false, true})
				{
					const auto& Pipeline = bHybrid ? Record.HybridPipeline : Record.Pipeline;
					if (!Pipeline || !ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][0]) continue;
					const auto& Shader = Pipeline->FragmentShader;
					if (!PreparePass(Draw, 0u, Shader.GetRHIShader(false), Shader.GetShader()->GetSurfaceLayout(),
						bHybrid ? Record.HybridFragmentPassBindings : Record.FragmentPassBindings)) return false;
				}
				if (Record.GBufferPipeline)
				{
					if (!GBuffer || !Vertex(GBuffer->GetVertexBinding(*Record.GBufferPipeline, *Primitive->CollectedBinding),
						1u, Record.GBufferVertexPassBindings)) return false;
					if (!PreparePass(Draw, 1u, GBuffer->GetFragmentShader(*Record.GBufferPipeline),
						GBuffer->GetSurfaceLayout(*Record.GBufferPipeline), Record.GBufferFragmentPassBindings)) return false;
				}
				return true;
			};
			if (ResolvedView.PersistentCommandsReused[Draw.ResolvedIndex])
			{
				++ResolvedView.Observations.PersistentCommandHits;
				if (!ResolvedView.PersistentCollectionBindingsChanged[Draw.ResolvedIndex])
				{
					if (!PreparePasses()) return false;
					continue;
				}
			}
			else ++ResolvedView.Observations.PersistentCommandMisses;
			auto PublishCommand = [&]() {
				if (!PreparePasses()) return false;
				if (!bCacheable) return true;
				auto Command = std::make_shared<FState::FMeshDrawCommand>();
				Command->Description = Draw.Command;
				Command->Input = Primitive->CollectedBinding;
				Command->PrimitiveUniform = ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex];
				Command->Record = Record;
				Command->PrimitiveState = FState::CapturePrimitiveState(*Primitive);
				Command->Materials = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex];
				Command->Record.VertexPassBindings.reset();
				Command->Record.HybridVertexPassBindings.reset();
				Command->Record.GBufferVertexPassBindings.reset();
				Command->Record.FragmentPassBindings.reset();
				Command->Record.HybridFragmentPassBindings.reset();
				Command->Record.GBufferFragmentPassBindings.reset();
				State->DrawCommands.insert_or_assign(CommandKey, FState::FCommandEntry{std::move(Command), ++State->CacheSerial});
				FState::TrimCache(State->DrawCommands);
				return true;
			};
			auto PrepareVertex = [&](const std::shared_ptr<const FMeshVertexShaderBinding>& Shader, uint32 Pass,
				std::shared_ptr<const FRHIShaderParameterBatch>& Out) -> bool {
				if (!Shader || !Shader->GetRHIShader(false)) return false;
				const auto Key = std::make_tuple(Draw.PrimitiveIndex, Draw.MaterialUniformIndex, Pass, Shader->GetRHIShader(false));
				if (const auto Existing = VertexBatches.find(Key); Existing != VertexBatches.end())
				{
					Out = Existing->second;
					return true;
				}
				const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				if (!Material) return false;
				Out = PrepareMeshVertexMaterial(*Shader,
					Shader->Prepare(CommandList, ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex], *Primitive->CollectedBinding),
					Material->Surface, Material->Uniform, {}, Material->CollectionUniforms, true);
				if (!Out) { DURIN_ERROR("Mesh stable vertex binding failed: pass={}", Pass); return false; }
				VertexBatches.emplace(Key, Out);
				return true;
			};
			if (Record.Pipeline && !PrepareVertex(Record.Pipeline->VertexShader, bShadow ? 2u : 0u, Record.VertexBindings)) return false;
			if (Record.HybridPipeline && !PrepareVertex(Record.HybridPipeline->VertexShader, 0u, Record.HybridVertexBindings)) return false;
			if (Record.GBufferPipeline && (!GBuffer || !PrepareVertex(
				GBuffer->GetVertexBinding(*Record.GBufferPipeline, *Primitive->CollectedBinding), 1u, Record.GBufferVertexBindings))) return false;
			if (bShadow)
			{
				if (Draw.Command->Pass != EMeshBasePass::Masked)
				{
					if (!PublishCommand()) return false;
					continue;
				}
				if (!Record.Pipeline || !Record.Pipeline->ShadowFragmentShader) return false;
				const auto& Shader = Record.Pipeline->ShadowFragmentShader;
				if (!Prepare(Draw, 2, Shader.GetRHIShader(false), Shader.GetShader()->GetSurfaceLayout(), Record.SurfaceBindings)) return false;
				if (!PublishCommand()) return false;
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
			if (Record.GBufferGPUCullingPipeline)
			{
				if (!GBuffer || !Prepare(Draw, 1,
					GBuffer->GetFragmentShader(*Record.GBufferGPUCullingPipeline),
					GBuffer->GetSurfaceLayout(*Record.GBufferGPUCullingPipeline),
					Record.GBufferGPUCullingBindings))
					Record.GBufferGPUCullingPipeline.reset();
			}
			if (!PublishCommand()) return false;
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
		FResolvedStaticMeshView& ResolvedView,
		const std::vector<uint32>* IndirectArgumentByResolvedDraw,
		FRHIBuffer* IndirectArguments,
		FRHIBuffer* VisibleInstances,
		FRHIBuffer* InstanceTransforms
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
		ForEachShadowBucket(PreparedView, [this, &CommandList, &View, &GBuffer, &PreparedView, &ResolvedView, &RecordFamily, &bComplete, &bRenderedGeometry, &BindingGroup, IndirectArgumentByResolvedDraw, IndirectArguments, VisibleInstances, InstanceTransforms](const auto& Bucket) {
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
				const uint32 IndirectIndex = IndirectArgumentByResolvedDraw
					&& Draw.ResolvedIndex < IndirectArgumentByResolvedDraw->size()
					? (*IndirectArgumentByResolvedDraw)[Draw.ResolvedIndex]
					: UINT32_MAX;
				if (IndirectArgumentByResolvedDraw
					&& IndirectIndex == UINT32_MAX - 1)
				{
					bRenderedGeometry = true;
					++ResolvedView.Observations.GBufferSuccessfulDraws;
					RecordFamily(ResolvedView, Draw,
						&FStaticMeshRenderObservations::GBufferLocalSuccessfulDraws,
						&FStaticMeshRenderObservations::GBufferSplineSuccessfulDraws);
					continue;
				}
				if (Primitive != nullptr && ResolvedView.IsReady(Draw)
					&& DrawGBufferSection_RenderThread(
						CommandList, View, GBuffer, *Primitive, Draw,
						ResolvedView, BindingGroup,
						IndirectIndex != UINT32_MAX ? IndirectArguments : nullptr,
						static_cast<uint64>(IndirectIndex)
							* sizeof(FRHIDrawIndexedIndirectArguments),
						VisibleInstances, InstanceTransforms
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
		FResolvedStaticMeshView& ResolvedView,
		bool bPrepareGPUCulling) -> bool
	{
		for (auto& Record : ResolvedView.Draws)
		{
			Record.GBufferPipeline.reset();
			Record.GBufferGPUCullingPipeline.reset();
		}
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
				if (bPrepareGPUCulling && Primitive->VertexDomain
					== EVertexDeformationDomain::Local
					&& Item.Command->Geometry.bIndexed)
				{
					auto GPUPipeline = GBuffer.EnsurePipeline_RenderThread({.Material = Item.Command->PipelineKey.Material, .CompiledProgram = Item.Command->Material.CompiledProgram, .Rasterizer = Item.Command->PipelineKey.Rasterizer, .Depth = Item.Command->PipelineKey.Depth, .VertexDeclaration = Geometry.GetVertexDeclaration(), .FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey, .bGPUCulling = true, .Topology = Item.Command->PipelineKey.Topology});
					ResolvedView.Draws[Item.ResolvedIndex]
						.GBufferGPUCullingPipeline = std::move(GPUPipeline);
				}
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
		const FResolvedStaticMeshView& ResolvedView,
		FMeshDrawBindingGroup& BindingGroup,
		FRHIBuffer* IndirectArguments,
		uint64 IndirectArgumentOffset,
		FRHIBuffer* VisibleInstances,
		FRHIBuffer* InstanceTransforms
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.DrawGBufferSection");
		const FStaticMeshGeometryBinding Geometry(Primitive, Item);
		if (!Geometry.IsValid())
		{
			return false;
		}
		const auto& Record = ResolvedView.Draws[Item.ResolvedIndex];
		const bool bGPUCulling = IndirectArguments != nullptr;
		const auto& Pipeline = bGPUCulling
			? Record.GBufferGPUCullingPipeline : Record.GBufferPipeline;
		const auto& FragmentBindings = bGPUCulling
			? Record.GBufferGPUCullingBindings : Record.GBufferBindings;
		if (!Pipeline || !FragmentBindings) return false;
		std::shared_ptr<const FRHIShaderParameterBatch> GPUVertexBindings;
		const auto& VertexBindings = bGPUCulling
			? GPUVertexBindings : Record.GBufferVertexBindings;
		if (bGPUCulling)
		{
			if (VisibleInstances == nullptr || InstanceTransforms == nullptr)
				return false;
			const auto Vertex = GBuffer.GetVertexBinding(
				*Pipeline, *Primitive.CollectedBinding);
			if (!Vertex) return false;
			GPUVertexBindings = Vertex->PrepareGPUCulling(
				{VisibleInstances, 0, VisibleInstances->GetSize()},
				{InstanceTransforms, 0, InstanceTransforms->GetSize()});
			const auto& Material = ResolvedView.MaterialUniforms[Item.MaterialUniformIndex][1];
			if (!Material) return false;
			GPUVertexBindings = PrepareMeshVertexMaterial(*Vertex, std::move(GPUVertexBindings),
				Material->Surface, Material->Uniform, ResolvedView.ViewUniforms[1], Material->CollectionUniforms);
			if (!GPUVertexBindings) return false;
		}

		if (!BindingGroup.Apply(Pipeline.get(), VertexBindings.get(), FragmentBindings.get(), [&] {
			if (!GBuffer.BindPipeline_RenderThread(CommandList, *Pipeline, VertexBindings, *FragmentBindings)) return false;
			if (!bGPUCulling && (!Record.GBufferVertexPassBindings || !Record.GBufferVertexPassBindings->Bind(CommandList))) return false;
			return Record.GBufferFragmentPassBindings && Record.GBufferFragmentPassBindings->Bind(CommandList);
		})) return false;
		Geometry.Bind(CommandList);
		if (IndirectArguments != nullptr)
			CommandList.DrawIndexedIndirect(IndirectArguments, IndirectArgumentOffset);
		else
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
			const auto& VertexPass = bHybridRetained ? Record.HybridVertexPassBindings : Record.VertexPassBindings;
			const auto& FragmentPass = bHybridRetained ? Record.HybridFragmentPassBindings : Record.FragmentPassBindings;
			if (!VertexPass || !VertexPass->Bind(CommandList)) return false;
			return !FragmentBindings || (FragmentBindings->Bind(CommandList)
				&& FragmentPass && FragmentPass->Bind(CommandList));
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
		State->ResetDrawCommands();
		check(IsInRenderingThread());
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}
} // namespace Durin
