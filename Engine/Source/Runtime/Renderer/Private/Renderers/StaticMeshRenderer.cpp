#include "Profiling/Profiling.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/StaticMeshSectionResources.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/StaticMeshDrawExecution.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialParameterCollection.h"

#include <typeinfo>

namespace Durin
{
	using namespace RendererPrivate;

	struct FResolvedMeshPipeline : FMeshPassPipeline {};

	struct FStaticMeshRenderer::FState
	{
		using FPrimitiveState = FMeshDrawPrimitiveState;
		static auto CapturePrimitiveState(const FPreparedStaticMeshPrimitive& Primitive) -> FPrimitiveState
		{ return FPrimitiveState::Capture(Primitive); }
		using FMeshDrawCommand = RendererPrivate::FMeshDrawCommand;

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
		uint64 ReleaseEpoch = 0;
		template<typename TCache>
		static auto TrimCache(TCache& Cache, size_t Limit = 512) -> void
		{
			if (Cache.size() <= Limit) return;
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
		using FDrawKey = FMeshDrawCommand::FKey;
		static auto GetPipeline(const FResolvedMeshDrawRecord& Record, size_t Slot) -> std::shared_ptr<const FMeshPassPipeline>
		{
			if (Slot == 0) return Record.Pipeline;
			if (Slot == 1) return Record.HybridPipeline;
			return FGBufferRenderer::GetPipelineResources(Slot == 2 ? Record.GBufferPipeline : Record.GBufferGPUCullingPipeline);
		}
		static auto MaterialSlot(const FMeshPassPipeline& Pipeline) -> uint32
		{ return Pipeline.Pass == EMeshPipelinePass::ShadowDepth ? 2u : Pipeline.Pass == EMeshPipelinePass::GBuffer ? 1u : 0u; }
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

		using FShaderMapPayload = FMeshPassShaders;

		using FPipelinePayload = std::shared_ptr<const FResolvedMeshPipeline>;

		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload, FMaterialResourceCacheKeyHash>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload, FMaterialResourceCacheKeyHash>
			ShadowShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FEffectiveMeshPipelineKey,
			FPipelinePayload, FMaterialResourceCacheKeyHash>
			Pipelines{
				ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device
			};
		TRendererResourceSlotCache<
			FEffectiveMeshPipelineKey,
			FPipelinePayload, FMaterialResourceCacheKeyHash>
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

	auto FStaticMeshRenderer::PrepareResources_RenderThread(FRHICommandListImmediate& CommandList,
		const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView,
		bool bPrepareLitOpaqueForward, FStaticMeshSectionResourceContext* ResourceContext) -> FGeometryResolutionResult
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
		ForEachBasePassBucket(PreparedView, [this, &PreparedView, &ResolvedView, bPrepareLitOpaqueForward,
												ResourceContext](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedStaticMeshDraw& Item : Bucket)
			{
				auto& Record = ResolvedView.Draws[Item.ResolvedIndex];
				const auto* Primitive = PreparedView.GetPrimitive(Item);
				const bool bNeedsForwardPipeline =
					Pass == EMeshBasePass::Translucent
					|| bPrepareLitOpaqueForward
					|| !FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Item.Command->Material.PlanningPassIdentity, {}, Item.Command->bSupportsGBuffer);
				const bool bReady = PrepareSectionResources_RenderThread(
					Primitive, Item, Record,
					bNeedsForwardPipeline ? EStaticMeshSectionResourcePolicy::Forward : EStaticMeshSectionResourcePolicy::SamplersOnly,
					ResourceContext);
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
				if (FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer))
					continue;
				const FPreparedStaticMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const FMaterialRenderBinding* MaterialBinding =
					ResolvedView.GetMaterialBinding(Draw);
				auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
				const bool bDrawReady = Primitive != nullptr && MaterialBinding != nullptr
					&& EnsureSectionResources_RenderThread(*Primitive, Draw, *MaterialBinding,
						Record.HybridPipeline, false, true);
				Record.bReady = Record.bReady && bDrawReady;
				bReady = bDrawReady && bReady;
			}
		});
		return bReady;
	}

	auto FStaticMeshRenderer::PrepareShadowResources_RenderThread(FRHICommandListImmediate& CommandList,
		const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView,
		FStaticMeshSectionResourceContext* ResourceContext) -> FGeometryResolutionResult
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
		ForEachShadowBucket(PreparedView, [this, &PreparedView, &ResolvedView, ResourceContext](const auto& Bucket) {
			for (const FPreparedStaticMeshDraw& Draw : Bucket)
			{
				auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
				const auto* Primitive = PreparedView.GetPrimitive(Draw);
				const bool bReady =
					PrepareSectionResources_RenderThread(
						Primitive, Draw, Record, EStaticMeshSectionResourcePolicy::Shadow, ResourceContext);
				Record.bReady = bReady;
				ResolvedView.Observations.ResourcePreparationSuccessfulDraws +=
					bReady ? 1u : 0u;
			}
		});
		return FinalizeResourcePreparation(ResolvedView);
	}

	// Only renderer-owned ordinary StaticMesh capture may reuse stable resources.
	// Published local geometry alone does not opt dynamic providers into this path.
	auto FStaticMeshRenderer::PrepareSectionResources_RenderThread(const FPreparedStaticMeshPrimitive* Primitive,
		const FPreparedStaticMeshDraw& Draw, FResolvedMeshDrawRecord& Record, EStaticMeshSectionResourcePolicy Policy,
		FStaticMeshSectionResourceContext* ResourceContext) -> bool
	{
		const bool bRetained =
			ResourceContext && Primitive && Primitive->bRetainedStaticMesh &&
			Primitive->VertexDomain == EVertexDeformationDomain::Local && Primitive->GeometryRecord &&
			Primitive->GeometryRecord == Draw.Command->GeometryOwner && Primitive->CollectedBinding &&
			Primitive->CollectedBinding == Primitive->GeometryRecord->GetBinding() &&
			Primitive->CollectedBinding->Declaration && !Primitive->CollectedBinding->Streams.empty() &&
			typeid(*Primitive->CollectedBinding) == typeid(FStaticMeshBatchBinding);
		FStaticMeshSectionResourceContext::FKey Key{};
		FRenderResourceGeneration Generation;
		uint64 ReleaseEpoch = 0;
		if (bRetained)
		{
			Generation = Coordinator.GetGeneration_RenderThread();
			ReleaseEpoch = State->ReleaseEpoch;
			ResourceContext->Synchronize(
				this, Generation, ReleaseEpoch, SurfaceMaterials.GetResourceRevision_RenderThread());
			Key = {Draw.Command.get(), Policy};
			if (const auto Found = ResourceContext->Entries.find(Key); Found != ResourceContext->Entries.end())
			{
				Record.MaterialBinding = Found->second.Material;
				Record.Pipeline = Found->second.Pipeline;
				return true;
			}
		}
		const bool bShadowDepth = Policy == EStaticMeshSectionResourcePolicy::Shadow;
		FMaterialRenderBinding MaterialBinding;
		if (!ResolvePreparedMaterialBinding(Draw.Command->Material, MaterialBinding,
				bShadowDepth ? "StaticMeshShadowMaterialBinding" : "StaticMeshMaterialBinding"))
			return false;
		Record.MaterialBinding = std::move(MaterialBinding);
		const bool bReady = Primitive && (Policy != EStaticMeshSectionResourcePolicy::SamplersOnly
			? EnsureSectionResources_RenderThread(*Primitive, Draw, *Record.MaterialBinding, Record.Pipeline, bShadowDepth)
			: EnsureMaterialSamplers_RenderThread(*Record.MaterialBinding));

		if (bReady && bRetained && Generation == Coordinator.GetGeneration_RenderThread() &&
			ReleaseEpoch == State->ReleaseEpoch)
		{
			// Ensure may have created a sampler or evicted another material's slot.
			ResourceContext->Synchronize(
				this, Generation, ReleaseEpoch, SurfaceMaterials.GetResourceRevision_RenderThread());
			ResourceContext->Entries.emplace(
				Key, FStaticMeshSectionResourceContext::FEntry{
						 Draw.Command, *Record.MaterialBinding, Record.Pipeline});
		}
		return bReady;
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
		if (!Factory || Factory->GetLayoutKey() != Item.Command->PipelineKey.LayoutKey) return false;
		const auto Pass = bShadowDepth ? EMeshPipelinePass::ShadowDepth
			: !bHybridRetained ? EMeshPipelinePass::Forward
			: Material.PlanningPassIdentity.ShaderMap.BlendMode == EMaterialBlendMode::Translucent
				? EMeshPipelinePass::SortedTranslucency : EMeshPipelinePass::HybridRetainedForward;
		const auto& Processor = FPassProcessorManager::GetMeshPassProcessor(Pass);
		const FMeshPassProcessorBatch Batch{Material.PlanningPassIdentity, *Factory,
			{Item.Command->PipelineKey.Rasterizer, Item.Command->PipelineKey.Depth, Item.Command->PipelineKey.ColorBlend},
			Item.Command->PipelineKey.Topology, {}, Item.Command->bSupportsGBuffer};

		using FShaderMapResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		const FMeshShaderMapKey ShaderMapKey{
			.Material = Material.PlanningPassIdentity.ShaderMap,
			.VertexDomain = Primitive.VertexDomain,
			.FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey
		};
		auto& ShaderMapCache = bShadowDepth ? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderMapEntry = ShaderMapCache.FindOrAddBounded(
			ShaderMapKey, MaterialShaderMapCacheEntryBudget, MaterialShaderMapCacheByteBudget);
		FState::FShaderMapPayload* ShaderMapPayload =
			ShaderMapEntry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[this, &Material, &Processor, &Batch]() -> FShaderMapResult {
					FState::FShaderMapPayload Candidate;
					FShaderCompileOptions Options;
					Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
					FMeshPassDrawListContext Context(Coordinator.GetGeneration_RenderThread(), Material.CompiledProgram.get(), Options, Candidate);
					Processor.AddMeshBatch(Batch, Context);
					const auto& Result = Context.GetResult();
					if (Result.Status == EShaderBuildStatus::Pending) return FShaderMapResult::Pending();
					if (Result.Status == EShaderBuildStatus::Canceled) return FShaderMapResult::Canceled();
					if (!Result) return FShaderMapResult::Failure(MakeRendererResourceCreateError(
						Context.GetFailureCategory(), "StaticMeshShaderMap",
						GetIdentityText(Material.PlanningPassIdentity.ShaderMap), Result.Error,
						ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual
						| (Context.GetFailureCategory() == ERenderResourceCreateErrorCategory::RHIResource
							? ERenderResourceGenerationDependency::Device : ERenderResourceGenerationDependency::None)));
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
			EffectivePipelineKey, MaterialPipelineCacheEntryBudget, MaterialPipelineCacheByteBudget);
		FRenderResourceGeneration PipelineGeneration =
			Coordinator.GetGeneration_RenderThread();
		PipelineGeneration.Shader =
			ShaderMapPayload->ShaderMap.GetGeneration().Shader;
		FState::FPipelinePayload* Pipeline = PipelineEntry.Slot.Resolve(
			PipelineGeneration,
			[&PipelineEntry, ShaderMapPayload, &EffectivePipelineKey, &VertexFactory, &Processor, &Batch]() -> FPipelineResult {
				FResolvedMeshPipeline Candidate;
				FMeshPassDrawListContext Context(FName(std::format("StaticMeshPipeline_{}", PipelineEntry.Index)),
					*ShaderMapPayload, VertexFactory.Declaration, Candidate);
				Processor.AddMeshBatch(Batch, Context);
				if (!Context.GetResult()) return FPipelineResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::GraphicsPipeline, "StaticMeshPipeline", GetIdentityText(EffectivePipelineKey),
					ERenderResourceCreateErrorReason::PipelineCreationFailed,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual));
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
		bool bGBuffer, bool bShadow, bool bAllowPartial) -> bool
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
					const bool bDeferredDraw = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer);
					Required[0] |= !bProductionDeferred || !bDeferredDraw;
					Required[1] |= bGBuffer && bDeferredDraw;
				}
			}
		});
		auto RejectGroup = [&](uint32 Group) {
			Requests[Group] = {};
			for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
			{
				const auto& Draw = PreparedView.GetDraw(Index);
				if (Draw.MaterialUniformIndex == Group) ResolvedView.Draws[Draw.ResolvedIndex].bReady = false;
			}
		};
		// Resolve and validate current world data before any command hit. Snapshot identity,
		// rather than its world-local version, owns the immutable allocation cache key.
		for (uint32 Group = 0; Group < Requests.size(); ++Group)
		{
			if (!std::ranges::any_of(Requests[Group], [](bool Required) { return Required; })) continue;
			const auto& Draw = PreparedView.GetDraw(PreparedView.MaterialUniformGroups[Group].RepresentativeDraw);
			const auto Program = Draw.Command->Material.CompiledProgram;
			if (!Program) continue;
			auto PrepareCollections = [&]() -> bool {
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
				return true;
			};
			if (!PrepareCollections())
			{
				DURIN_ERROR("Material collection uniform preparation failed for group {}", Group);
				if (!bAllowPartial) return false;
				RejectGroup(Group);
			}
		}
		// Probe each selected pass independently before constructing uniforms.
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			if (!ResolvedView.IsReady(Draw) || !Primitive || !State->IsCacheable(Draw, *Primitive)) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			Record.Commands = {};
			bool AllHit = true, Any = false;
			for (size_t Slot = 0; Slot < Record.Commands.size(); ++Slot)
			{
				const auto Pipeline = FState::GetPipeline(Record, Slot); if (!Pipeline) continue;
				Any = true;
				const auto Existing = State->DrawCommands.find(FMeshDrawCommand::MakeKey(Draw.Command.get(), Primitive->PrimitiveId, Primitive->BatchId, *Pipeline));
				if (Existing == State->DrawCommands.end()) { AllHit = false; continue; }
				const auto& Cached = *Existing->second.Command;
				if (Cached.Input != Primitive->CollectedBinding || Cached.PrimitiveState != FState::CapturePrimitiveState(*Primitive)
					|| !SurfaceMaterials.AreStableResourcesCurrent_RenderThread(Cached.Material.Surface))
				{ AllHit = false; continue; }
				const uint32 Pass = FState::MaterialSlot(*Pipeline);
				const auto& CurrentCollections = GroupCollections[Draw.MaterialUniformIndex];
				const bool SameCollections = std::ranges::equal(Cached.Material.CollectionUniforms, CurrentCollections,
					[](const auto& A, const auto& B) { return A.Buffer == B.Buffer && A.Offset == B.Offset && A.Size == B.Size; });
				ResolvedView.PersistentCollectionBindingsChanged[Draw.ResolvedIndex] =
					ResolvedView.PersistentCollectionBindingsChanged[Draw.ResolvedIndex] || !SameCollections;
				if (SameCollections) Record.Commands[Slot] = Existing->second.Command;
				ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex] = Cached.PrimitiveUniform;
				auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				if (!Material && Requests[Draw.MaterialUniformIndex][Pass])
				{
					Material = Cached.Material; Material->CollectionUniforms = CurrentCollections;
					if (!SurfaceMaterials.RefreshPassResources_RenderThread(Pass == 0 ? ESurfaceMaterialPass::Forward
						: Pass == 1 ? ESurfaceMaterialPass::GBuffer : ESurfaceMaterialPass::MaskedShadow,
						ResolvedView.Environment, ResolvedView.DirectionalShadowTexture, ResolvedView.DirectionalShadowSampler, Material->Surface))
					{
						if (!bAllowPartial) return false;
						RejectGroup(Draw.MaterialUniformIndex); AllHit = false; break;
					}
				}
				Existing->second.LastUsed = ++State->CacheSerial;
			}
			ResolvedView.PersistentCommandsReused[Draw.ResolvedIndex] = Any && AllHit;
		}
		std::vector<bool> NeededPrimitives(PreparedView.Primitives.size());
		for (uint32 DrawIndex = 0; DrawIndex < PreparedView.GetNumSections(); ++DrawIndex)
		{
			const auto& Draw = PreparedView.GetDraw(DrawIndex);
			if (ResolvedView.IsReady(Draw)) NeededPrimitives[Draw.PrimitiveIndex] = true;
		}
		for (uint32 Index = 0; Index < PreparedView.Primitives.size(); ++Index)
		{
			if (ResolvedView.PrimitiveUniforms[Index].Buffer)
			{
				++ResolvedView.Observations.PrimitiveUniformReuses;
				continue;
			}
			if (!NeededPrimitives[Index]) continue;
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
				if (!Range.Buffer)
				{
					DURIN_ERROR("Primitive uniform allocation failed for component {}", Primitive.PrimitiveId.Value);
					if (!bAllowPartial) return false;
					for (uint32 DrawIndex = 0; DrawIndex < PreparedView.GetNumSections(); ++DrawIndex)
					{
						const auto& Draw = PreparedView.GetDraw(DrawIndex);
						if (Draw.PrimitiveIndex == Index) ResolvedView.Draws[Draw.ResolvedIndex].bReady = false;
					}
					continue;
				}
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
				if (!FStaticMeshSurfaceMaterialPreparer(CommandList, SurfaceMaterials, Binding, ResolvedView.Environment)
					.Prepare(Pass == 0 ? ESurfaceMaterialPass::Forward : Pass == 1 ? ESurfaceMaterialPass::GBuffer : ESurfaceMaterialPass::MaskedShadow,
						Pass == 0 ? ResolvedView.DirectionalShadowTexture : nullptr,
						Pass == 0 ? ResolvedView.DirectionalShadowSampler : nullptr, Material, SharedMaterialUniform)) {
						if (!bAllowPartial) return false;
						RejectGroup(Group);
						break;
					}
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
		FMeshDrawCommandBindingCache BindingCache;
		bool AllReady = true;
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			if (!ResolvedView.IsReady(Draw)) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			const auto Factory = FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
			if (!Primitive || !Primitive->CollectedBinding || !Factory) { Record.bReady = false; AllReady = false; continue; }
			if (ResolvedView.PersistentCommandsReused[Draw.ResolvedIndex]) ++ResolvedView.Observations.PersistentCommandHits;
			else ++ResolvedView.Observations.PersistentCommandMisses;
			const bool Cacheable = State->IsCacheable(Draw, *Primitive);
			for (size_t Slot = 0; Slot < Record.Commands.size(); ++Slot)
			{
				const auto Pipeline = FState::GetPipeline(Record, Slot); if (!Pipeline) { Record.Commands[Slot].reset(); continue; }
				const uint32 Pass = FState::MaterialSlot(*Pipeline);
				const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				bool Ready = Material.has_value();
				std::string Error = Ready ? std::string{} : "Missing prepared material uniform.";
				const auto& Processor = FPassProcessorManager::GetMeshPassProcessor(Pipeline->Pass);
				const FMeshPassProcessorBatch Batch{Draw.Command->Material.PlanningPassIdentity, *Factory,
					{Draw.Command->PipelineKey.Rasterizer, Draw.Command->PipelineKey.Depth, Draw.Command->PipelineKey.ColorBlend},
					Draw.Command->PipelineKey.Topology, {}, Draw.Command->bSupportsGBuffer, Pipeline->bGPUCulling};
				std::shared_ptr<const FMeshDrawCommand> Cached = Record.Commands[Slot], Dynamic;
				if (Cached && !Cached->bCached) Cached.reset();
				if (Ready)
				{
					const FMeshDrawCommandBuildInput Input{CommandList, *Primitive, Draw, Pipeline, *Material,
						ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex], ResolvedView.ViewUniforms[Pass], Lighting, {}, Pass};
					if (Cacheable && !Cached)
					{
						FCachedPassMeshDrawListContext Context(Input, BindingCache, Cached);
						Processor.AddMeshBatch(Batch, Context); Ready = static_cast<bool>(Context.GetResult());
						if (!Ready) Error = Context.GetResult().Error;
					}
					if (Ready)
					{
						FDynamicPassMeshDrawListContext Context(Input, BindingCache, Dynamic, Cached);
						Processor.AddMeshBatch(Batch, Context); Ready = static_cast<bool>(Context.GetResult());
						if (!Ready) Error = Context.GetResult().Error;
					}
				}
				if (!Ready)
				{
					Record.Commands[Slot].reset();
					if (Slot == 3) { Record.GBufferGPUCullingPipeline.reset(); continue; }
					DURIN_ERROR("Mesh pass binding collection failed: pass={}, error={}", static_cast<uint8>(Pipeline->Pass), Error);
					Record.bReady = false; AllReady = false; break;
				}
				Record.Commands[Slot] = std::move(Dynamic);
				if (Cached)
				{
					const auto Key = Cached->GetKey();
					State->DrawCommands.insert_or_assign(Key, FState::FCommandEntry{std::move(Cached), ++State->CacheSerial});
					// Preserve the former budget of up to four passes for 512 draws.
					FState::TrimCache(State->DrawCommands, 4 * 512);
				}
			}
		}
		ResolvedView.Observations.PreparedSurfaceBindingBatches = BindingCache.PreparedSurfaceBindingBatches;
		ResolvedView.Observations.SurfaceBindingBatchReuses = BindingCache.SurfaceBindingBatchReuses;
		return AllReady;
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
				if (!FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer))
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
		bool bPrepareGPUCulling, EGPUCullingFailure Failure) -> bool
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
				if (!FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Item.Command->Material.PlanningPassIdentity, {}, Item.Command->bSupportsGBuffer)) continue;
				const auto* Primitive = PreparedView.GetPrimitive(Item);
				if (!Primitive) { ResolvedView.Draws[Item.ResolvedIndex].bReady = false; Ready = false; continue; }
				const FStaticMeshGeometryBinding Geometry(*Primitive, Item);
				if (!Geometry.IsValid()) { ResolvedView.Draws[Item.ResolvedIndex].bReady = false; Ready = false; continue; }
				auto Pipeline = GBuffer.EnsurePipeline_RenderThread({.Material = Item.Command->PipelineKey.Material, .CompiledProgram = Item.Command->Material.CompiledProgram, .Rasterizer = Item.Command->PipelineKey.Rasterizer, .Depth = Item.Command->PipelineKey.Depth, .VertexDeclaration = Geometry.GetVertexDeclaration(), .FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey, .Topology = Item.Command->PipelineKey.Topology});
				ResolvedView.Draws[Item.ResolvedIndex].bReady = ResolvedView.IsReady(Item) && Pipeline != nullptr;
				Ready = Pipeline && Ready;
				ResolvedView.Draws[Item.ResolvedIndex].GBufferPipeline = std::move(Pipeline);
				if (bPrepareGPUCulling && Primitive->VertexDomain
					== EVertexDeformationDomain::Local
					&& Item.Command->Geometry.bIndexed
					&& (!Item.Command->Material.CompiledProgram
						|| (Item.Command->Material.CompiledProgram->GetShaderMap().RequiredStages & MaterialShaderEntryBit(EMaterialShaderEntry::GPUCullingVertex))))
				{
					auto GPUPipeline = GBuffer.EnsurePipeline_RenderThread({.Material = Item.Command->PipelineKey.Material, .CompiledProgram = Item.Command->Material.CompiledProgram, .Rasterizer = Item.Command->PipelineKey.Rasterizer, .Depth = Item.Command->PipelineKey.Depth, .VertexDeclaration = Geometry.GetVertexDeclaration(), .FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey, .bGPUCulling = true, .Failure = Failure, .Topology = Item.Command->PipelineKey.Topology});
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
		auto Command = Record.Commands[bGPUCulling ? 3 : 2];
		if (!Command) return false;
		if (bGPUCulling)
		{
			if (!VisibleInstances || !InstanceTransforms) return false;
			const auto Factory = FindMeshVertexFactory(Command->Pipeline->FactoryKey); if (!Factory) return false;
			const FMeshPassProcessorBatch Batch{Item.Command->Material.PlanningPassIdentity, *Factory,
				{Item.Command->PipelineKey.Rasterizer, Item.Command->PipelineKey.Depth, Item.Command->PipelineKey.ColorBlend},
				Item.Command->PipelineKey.Topology, {}, Item.Command->bSupportsGBuffer, true};
			FDynamicPassMeshDrawListContext Context(Command, {VisibleInstances, 0, VisibleInstances->GetSize()},
				{InstanceTransforms, 0, InstanceTransforms->GetSize()}, Command);
			FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).AddMeshBatch(Batch, Context);
			if (!Context.GetResult()) return false;
		}
		if (!BindingGroup.Apply(Command->Pipeline.get(), Command->VertexBindings.get(), Command->FragmentBindings.get(),
			[&] { return Command->SubmitBindings(CommandList); })) return false;
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


		const auto& Command = Record.Commands[bHybridRetained ? 1 : 0];
		if (!Command) return false;
		FMeshDrawBindingGroup SingleDrawGroup;
		auto& Group = BindingGroup ? *BindingGroup : SingleDrawGroup;
		if (!Group.Apply(Command->Pipeline.get(), Command->VertexBindings.get(), Command->FragmentBindings.get(),
			[&] { return Command->SubmitBindings(CommandList); })) return false;
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
		++State->ReleaseEpoch;
		State->ResetDrawCommands();
		check(IsInRenderingThread());
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}

} // namespace Durin
