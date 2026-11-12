#include "Profiling/Profiling.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/StaticMeshSectionResources.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/SceneViewPreparation.h"
#include "Renderers/SurfaceMaterial.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialParameterCollection.h"

#include <typeinfo>

namespace Durin
{
	using namespace RendererPrivate;
	namespace
	{

		auto FinalizeResourcePreparation(
			FResolvedStaticMeshView& ResolvedView
		) -> FGeometryResolutionResult
		{
			auto& Observations = ResolvedView.Observations;
			Observations.ResourcePreparationRejectedDraws =
				Observations.ResourcePreparationAttemptedDraws
				- Observations.ResourcePreparationSuccessfulDraws;
			check(
				Observations.ResourcePreparationAttemptedDraws
				== Observations.ResourcePreparationSuccessfulDraws
					   + Observations.ResourcePreparationRejectedDraws
			);
			return {
				.Status = Observations.ResourcePreparationRejectedDraws == 0
					? EGeometryResolutionStatus::Complete
					: EGeometryResolutionStatus::Partial,
				.AttemptedDraws = Observations.ResourcePreparationAttemptedDraws,
				.ResolvedDraws = Observations.ResourcePreparationSuccessfulDraws,
				.RejectedDraws = Observations.ResourcePreparationRejectedDraws
			};
		}

		auto FinalizeExecution(
			FResolvedStaticMeshView& ResolvedView,
			size_t ExpectedDraws,
			bool bExpectAllDraws = true
		) -> void
		{
			const auto& Observations = ResolvedView.Observations;
			check(
				Observations.AttemptedDraws
				== Observations.SuccessfulDraws + Observations.RejectedDraws
			);
			check(!bExpectAllDraws || Observations.AttemptedDraws == ExpectedDraws);
		}
	}



	struct FStaticMeshRenderer::FState
	{

		uint64 ReleaseEpoch = 0;

		FRenderResourceGeneration DrawGeneration;
		bool bPersistentDraws = [] {
			const char* Setting = std::getenv("DURIN_MESH_DRAW_CACHE");
			return !Setting || std::string_view(Setting) != "off";
		}();
		static auto GetPassKey(const FPreparedStaticMeshDraw& Draw, bool Shadow = false, bool Hybrid = false) -> FMeshPassKey
		{
			return {Shadow ? EMeshPipelinePass::ShadowDepth : !Hybrid ? EMeshPipelinePass::Forward
				: Draw.Command->Material.PlanningPassIdentity.ShaderMap.BlendMode == EMaterialBlendMode::Translucent
					? EMeshPipelinePass::SortedTranslucency : EMeshPipelinePass::HybridRetainedForward};
		}
		static auto MaterialPass(const FMeshPassPipeline& Pipeline) -> ESurfaceMaterialPass
		{ return Pipeline.Pass == EMeshPipelinePass::ShadowDepth ? ESurfaceMaterialPass::MaskedShadow
			: Pipeline.Pass == EMeshPipelinePass::GBuffer ? ESurfaceMaterialPass::GBuffer : ESurfaceMaterialPass::Forward; }
		auto IsCacheable(const FPreparedStaticMeshDraw& Draw, const FPreparedStaticMeshPrimitive& Primitive) const -> bool
		{
			return bPersistentDraws && !Primitive.DrawResources.expired() && Primitive.GeometryRecord && Primitive.VertexDomain == EVertexDeformationDomain::Local;
		}
		FCachedMeshDrawCommandStorage DrawCommands;
		auto ResetDrawCommands() -> void
		{
			DrawCommands.Reset();
		}

		using FShaderMapPayload = FMeshPassShaders;

		using FPipelinePayload = std::shared_ptr<const FMeshPassPipeline>;

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
		for (auto& Record : ResolvedView.Draws) {
			Record.Pipelines.erase({EMeshPipelinePass::HybridRetainedForward});
			Record.Pipelines.erase({EMeshPipelinePass::SortedTranslucency});
		}
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
						Record.Pipelines[FState::GetPassKey(Draw, false, true)], false, true);
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
				if (Found->second.Pipeline) Record.Pipelines[Found->second.Pipeline->GetPassKey()] = Found->second.Pipeline;
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
			? EnsureSectionResources_RenderThread(*Primitive, Draw, *Record.MaterialBinding, Record.Pipelines[FState::GetPassKey(Draw, bShadowDepth)], bShadowDepth)
			: EnsureMaterialSamplers_RenderThread(*Record.MaterialBinding));

		if (bReady && bRetained && Generation == Coordinator.GetGeneration_RenderThread() &&
			ReleaseEpoch == State->ReleaseEpoch)
		{
			// Ensure may have created a sampler or evicted another material's slot.
			ResourceContext->Synchronize(
				this, Generation, ReleaseEpoch, SurfaceMaterials.GetResourceRevision_RenderThread());
			ResourceContext->Entries.emplace(
				Key, FStaticMeshSectionResourceContext::FEntry{
						 Draw.Command, *Record.MaterialBinding, Policy == EStaticMeshSectionResourcePolicy::SamplersOnly
						? nullptr : Record.Pipelines.at(FState::GetPassKey(Draw, bShadowDepth))});
		}
		return bReady;
	}

	auto FStaticMeshRenderer::RecordShadow(FRHICommandList& CommandList,
		const FPreparedStaticMeshView& PreparedView, const FResolvedStaticMeshView& ResolvedView)
		-> FStaticMeshRenderObservations
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RecordShadow");
		check(CommandList.IsInsideRenderPass());
		FMeshDrawCommandStateCache BindingGroup;
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
		std::shared_ptr<const FMeshPassPipeline>& OutPipeline,
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
					ERenderResourceCreateErrorCategory FailureCategory;
					const auto Result = Processor.PrepareShaders(Batch, Coordinator.GetGeneration_RenderThread(), Material.CompiledProgram.get(), Options, Candidate, &FailureCategory);
					if (Result.Status == EShaderBuildStatus::Pending) return FShaderMapResult::Pending();
					if (Result.Status == EShaderBuildStatus::Canceled) return FShaderMapResult::Canceled();
					if (!Result) return FShaderMapResult::Failure(MakeRendererResourceCreateError(
						FailureCategory, "StaticMeshShaderMap",
						GetIdentityText(Material.PlanningPassIdentity.ShaderMap), Result.Error,
						ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual
						| (FailureCategory == ERenderResourceCreateErrorCategory::RHIResource
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
				FMeshPassPipeline Candidate;
				const auto Preparation = Processor.PreparePipeline(Batch, FName(std::format("StaticMeshPipeline_{}", PipelineEntry.Index)),
					*ShaderMapPayload, VertexFactory.Declaration, Candidate);
				if (!Preparation) return FPipelineResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::GraphicsPipeline, "StaticMeshPipeline", GetIdentityText(EffectivePipelineKey),
					ERenderResourceCreateErrorReason::PipelinePreparationRejected,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual
				));
				return FPipelineResult::Success(std::make_shared<const FMeshPassPipeline>(std::move(Candidate)));
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
		if (PreparedView.RenderFrame != GRenderFrameCounterRenderThread) return false;
		ResolvedView.Observations.SurfaceMaterialPreparations = 0;
		std::vector<std::set<ESurfaceMaterialPass>> Requests(PreparedView.MaterialUniformGroups.size());
		std::vector<std::vector<FRHIUniformBufferRange>> GroupCollections(PreparedView.MaterialUniformGroups.size());
		std::map<const FUniformBuffer*, FRHIUniformBufferRange> CollectionUniforms;
		ForEachBasePassBucket(PreparedView, [&](const auto& Bucket, EMeshBasePass Pass) {
			for (const auto& Draw : Bucket)
			{
				if (!ResolvedView.IsReady(Draw)) continue;
				auto& Required = Requests[Draw.MaterialUniformIndex];
				if (bShadow) Required.insert(ESurfaceMaterialPass::MaskedShadow);
				else
				{
					const bool bDeferredDraw = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer);
					if (!bProductionDeferred || !bDeferredDraw) Required.insert(ESurfaceMaterialPass::Forward);
					if (bGBuffer && bDeferredDraw) Required.insert(ESurfaceMaterialPass::GBuffer);
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
		// Resolve world values and program defaults before selecting cached commands.
		for (uint32 Group = 0; Group < Requests.size(); ++Group)
		{
			if (Requests[Group].empty()) continue;
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
					const auto& Material = Draw.Command->Material;
					const auto Cache = Material.UniformExpressionCache ? Material.UniformExpressionCache : Material.Representation.GetUniformExpressionCache();
					FUniformBuffer* Buffer;
					if (Snapshot)
					{
						auto& Owned = Scene->ParameterCollectionUniformBuffers[Required.CollectionId];
						if (!Owned) Owned = FUniformBuffer::Create();
						Buffer = Owned.get();
					}
					else Buffer = &Cache->GetParameterCollectionUniformBuffer(Program, Required.CollectionId);
					auto Local = CollectionUniforms.find(Buffer);
					if (Local == CollectionUniforms.end())
					{
						const bool Uniform = Buffer->Update(CommandList, Payload);
						if (!Uniform) return false;
						Local = CollectionUniforms.emplace(Buffer, Buffer->GetRange()).first;
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
		std::vector<FRHIUniformBufferRange> GroupUniforms(Requests.size());
		for (uint32 Group = 0; Group < Requests.size(); ++Group)
		{
			if (Requests[Group].empty()) continue;
			const auto& Material = PreparedView.GetDraw(PreparedView.MaterialUniformGroups[Group].RepresentativeDraw).Command->Material;
			if (Material.Representation.GetUniformPayload().empty()) continue;
			const auto Cache = Material.UniformExpressionCache ? Material.UniformExpressionCache : Material.Representation.GetUniformExpressionCache();
			const bool Changed = !std::ranges::equal(Cache->UniformBuffer->GetData(), Material.Representation.GetUniformPayload());
			if (!Cache->UpdateUniformBuffer(CommandList, Material.Representation)) { if (!bAllowPartial) return false; RejectGroup(Group); continue; }
			if (Changed) ++ResolvedView.Observations.MaterialUniformUploads;
			GroupUniforms[Group] = Cache->UniformBuffer->GetRange();
		}
		std::map<std::tuple<uint64, ESurfaceMaterialPass, const void*>, bool> MaterialResourceValidity;
		// Probe each selected pass independently before constructing uniforms.
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			if (!ResolvedView.IsReady(Draw) || !Primitive || !State->IsCacheable(Draw, *Primitive)) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			Record.Commands = {};
			Record.SelectedCachedCommands.clear();
			bool AllHit = true, Any = false;
			for (const auto& [PassKey, Pipeline] : Record.Pipelines)
			{
				if (!Pipeline) continue;
				Any = true;
				const auto Existing = State->DrawCommands.Find(PassKey, FMeshDrawCommand::MakeKey(Draw.Command.get(), Primitive->PrimitiveId, Primitive->BatchId, *Pipeline));
				if (!Existing) { AllHit = false; continue; }
				const auto& Cached = *Existing;
				if (Cached.Input != Primitive->CollectedBinding.get()) { AllHit = false; continue; }
				const auto Pass = FState::MaterialPass(*Pipeline);
				const auto ResourceKey = std::make_tuple(Draw.Command->Material.Representation.GetRecordId(), Pass,
					Cached.FragmentBindings ? static_cast<const void*>(&Cached.FragmentBindings) : static_cast<const void*>(&Cached));
				auto [ResourceValidity, NewMaterial] = MaterialResourceValidity.try_emplace(ResourceKey, false);
				if (NewMaterial)
				{
					const auto& Current = Draw.Command->Material.Representation;
					ResourceValidity->second = Cached.MaterialLayout == Current.GetLayout()
						&& std::ranges::equal(Cached.MaterialResources, Current.GetResources(), [](auto* A, const auto& B) { return A == B.GetReference(); })
						&& std::ranges::equal(Cached.MaterialSamplers, Current.GetSamplers())
						&& std::ranges::equal(Cached.MaterialFallbacks, Current.GetTextureFallbacks())
						&& (!GroupUniforms[Draw.MaterialUniformIndex].Buffer || Cached.Material.Uniform.Buffer == GroupUniforms[Draw.MaterialUniformIndex].Buffer)
						&& SurfaceMaterials.AreStableResourcesCurrent_RenderThread(Cached.Material.Surface, Current);
				}
				if (!ResourceValidity->second) { AllHit = false; continue; }
				const auto& CurrentCollections = GroupCollections[Draw.MaterialUniformIndex];
				const bool SameCollections = std::ranges::equal(Cached.Material.CollectionUniforms, CurrentCollections,
					[](const auto& A, const auto& B) { return A.Buffer == B.Buffer && A.Offset == B.Offset && A.Size == B.Size; });
				ResolvedView.PersistentCollectionBindingsChanged[Draw.ResolvedIndex] =
					ResolvedView.PersistentCollectionBindingsChanged[Draw.ResolvedIndex] || !SameCollections;
				if (SameCollections) Record.SelectedCachedCommands.emplace(PassKey, Existing);

				// Primitive contents are selected below, independently of cached commands.
				auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				if (!Material && Requests[Draw.MaterialUniformIndex].contains(Pass))
				{
					Material = Cached.Material; Material->CollectionUniforms = CurrentCollections;
					if (GroupUniforms[Draw.MaterialUniformIndex].Buffer) Material->Uniform = GroupUniforms[Draw.MaterialUniformIndex];
					if (!SurfaceMaterials.RefreshPassResources_RenderThread(Pass,
						ResolvedView.Environment, ResolvedView.DirectionalShadowTexture, ResolvedView.DirectionalShadowSampler, Material->Surface))
					{
						if (!bAllowPartial) return false;
						RejectGroup(Draw.MaterialUniformIndex); AllHit = false; break;
					}
				}
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
			const auto Uniform = BuildMeshTransformUniform(Primitive);
			FRHIUniformBufferRange Range;
			if (State->bPersistentDraws && !Primitive.DrawResources.expired())
			{
				auto& Buffer = Primitive.DrawResources.lock()->GetPrimitiveUniformBuffer(Primitive.BatchId);
				const bool Changed = !std::ranges::equal(Buffer.GetData(), std::as_bytes(std::span(&Uniform, 1)));
				if (Buffer.Update(CommandList, std::as_bytes(std::span(&Uniform, 1))))
				{
					if (Changed) ++ResolvedView.Observations.PrimitiveUniformUploads;
					else ++ResolvedView.Observations.PrimitiveUniformReuses;
					Range = Buffer.GetRange();
				}
			}
			else
			{
				Range = CommandList.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
				++ResolvedView.Observations.PrimitiveUniformUploads;
			}
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
			ResolvedView.PrimitiveUniforms[Index] = std::move(Range);
		}
		for (const auto Pass : {ESurfaceMaterialPass::Forward, ESurfaceMaterialPass::GBuffer, ESurfaceMaterialPass::MaskedShadow})
		{
			if (!std::ranges::any_of(Requests, [Pass](const auto& Required) { return Required.contains(Pass); })) continue;
			ResolvedView.ViewUniforms[Pass] = PrepareMeshViewUniform(CommandList, View,
				Pass == ESurfaceMaterialPass::GBuffer || (Pass == ESurfaceMaterialPass::Forward && View.Settings.Mode.RenderMode == ERenderMode::Lit));
			if (!ResolvedView.ViewUniforms[Pass].Buffer) return false;
		}
		for (uint32 Group = 0; Group < Requests.size(); ++Group)
		{
			bool bNeedsPreparation = false;
			for (const auto Pass : {ESurfaceMaterialPass::Forward, ESurfaceMaterialPass::GBuffer, ESurfaceMaterialPass::MaskedShadow})
				bNeedsPreparation |= Requests[Group].contains(Pass) && !ResolvedView.MaterialUniforms[Group][Pass];
			if (!bNeedsPreparation) continue;
			if (Requests[Group].empty()) continue;
			const auto& Draw = PreparedView.GetDraw(PreparedView.MaterialUniformGroups[Group].RepresentativeDraw);
			const auto* Binding = ResolvedView.GetMaterialBinding(Draw);
			const auto& RequiredCollections = GroupCollections[Group];
			FRHIUniformBufferRange SharedMaterialUniform = GroupUniforms[Group];

			for (const auto Pass : {ESurfaceMaterialPass::Forward, ESurfaceMaterialPass::GBuffer, ESurfaceMaterialPass::MaskedShadow})
			{
				if (!Requests[Group].contains(Pass) || ResolvedView.MaterialUniforms[Group][Pass]) continue;
				FPreparedSurfaceMaterial Material;
				++ResolvedView.Observations.SurfaceMaterialPreparations;
				if (!SurfaceMaterials.Prepare_RenderThread(CommandList, Binding, Pass, ResolvedView.Environment,
						Pass == ESurfaceMaterialPass::Forward ? ResolvedView.DirectionalShadowTexture : nullptr,
						Pass == ESurfaceMaterialPass::Forward ? ResolvedView.DirectionalShadowSampler : nullptr, Material, SharedMaterialUniform)) {
						if (!bAllowPartial) return false;
						RejectGroup(Group);
						break;
					}
				if (!SharedMaterialUniform.Buffer)
				{
					SharedMaterialUniform = Material.Uniform;
					++ResolvedView.Observations.MaterialUniformUploads;

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
		const FRHIUniformBufferRange& Lighting, bool bShadow, FRHIStorageBufferRange LocalLights) -> bool
	{
		check(IsInRenderingThread());
		ResolvedView.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();
		for (auto& Record : ResolvedView.Draws) { Record.Commands.clear(); Record.VisibleCommands.clear(); }
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
			for (auto& [PassKey, Pipeline] : Record.Pipelines)
			{
				if (!Pipeline) continue;
				const auto Pass = FState::MaterialPass(*Pipeline);
				const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				bool Ready = Material.has_value();
				std::string Error = Ready ? std::string{} : "Missing prepared material uniform.";
				const auto& Processor = FPassProcessorManager::GetMeshPassProcessor(PassKey.Pass);
				const FMeshPassProcessorBatch Batch{Draw.Command->Material.PlanningPassIdentity, *Factory,
					{Draw.Command->PipelineKey.Rasterizer, Draw.Command->PipelineKey.Depth, Draw.Command->PipelineKey.ColorBlend},
					Draw.Command->PipelineKey.Topology, {}, Draw.Command->bSupportsGBuffer, PassKey.bGPUCulling};
				std::shared_ptr<const FMeshDrawCommand> Cached;
				if (Ready)
				{
					if (Cacheable)
						if (const auto Selected = Record.SelectedCachedCommands.find(PassKey); Selected != Record.SelectedCachedCommands.end())
							Cached = Selected->second;
					// Cache eviction may replace a logical uniform object without changing values.
					if (Cached && Cached->PrimitiveUniform.Buffer != ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex].Buffer) Cached.reset();
					const FMeshDrawCommandBuildInput Input{CommandList, *Primitive, Draw, Pipeline, *Material,
						ResolvedView.PrimitiveUniforms[Draw.PrimitiveIndex], ResolvedView.ViewUniforms[Pass], Lighting, {}, LocalLights};
					if (Cacheable && !Cached)
					{
						FCachedPassMeshDrawListContext Context(State->DrawCommands, PassKey, Cached, *Primitive->DrawResources.lock());
						const auto Result = Processor.AddMeshBatch(Batch, Input, BindingCache, Context); Ready = static_cast<bool>(Result);
						if (!Ready) Error = Result.Error;
					}
					if (Ready && Cached && !PassKey.bGPUCulling)
					{
						auto Group = BindingCache.PassGroups.find(Pipeline.get());
						if (Group == BindingCache.PassGroups.end())
						{
							auto PassBindings = std::make_shared<FMeshPassBindings>();
							const auto Result = Processor.PreparePassBindings(Input, BindingCache, *PassBindings);
							Ready = static_cast<bool>(Result);
							if (!Ready) Error = Result.Error;
							else Group = BindingCache.PassGroups.emplace(Pipeline.get(), std::move(PassBindings)).first;
						}
						if (Ready)
						{
							const auto Bias = bShadow && Draw.RasterBias ? *Draw.RasterBias : Cached->DepthBias;
							Record.VisibleCommands[PassKey] = ResolvedView.DrawCommandStorage->PublishVisible(Cached,
								Group->second, Bias);
						}
					}
					else if (Ready)
					{
						FMeshDrawCommandHandle Receipt;
						FDynamicPassMeshDrawListContext Context(*ResolvedView.DrawCommandStorage, PassKey, Receipt);
						const auto Result = Processor.AddMeshBatch(Batch, Input, BindingCache, Context, Cached); Ready = static_cast<bool>(Result);
						if (!Ready) Error = Result.Error;
						else Record.Commands.insert_or_assign(PassKey, Receipt);
					}
				}
				if (!Ready)
				{
					if (PassKey.bGPUCulling) { Pipeline.reset(); continue; }
					DURIN_ERROR("Mesh pass binding collection failed: pass={}, error={}", static_cast<uint8>(PassKey.Pass), Error);
					Record.bReady = false; AllReady = false; break;
				}
			}
		}
		for (auto& Record : ResolvedView.Draws) Record.SelectedCachedCommands.clear();
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
		FMeshDrawCommandStateCache BindingGroup;
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
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedStaticMeshDraw& Item, const FPreparedStaticMeshView& PreparedView, FResolvedStaticMeshView& ResolvedView, bool bHybridRetained, FMeshDrawCommandStateCache* BindingGroup
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
		FMeshDrawCommandStateCache BindingGroup;
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
		FMeshDrawCommandStateCache BindingGroup;
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
					if (IndirectIndex != UINT32_MAX)
						++ResolvedView.Observations.GBufferIndirectSubmissions;
					else if (Draw.Command->PipelineKey.VertexDomain != EVertexDeformationDomain::Spline)
						++ResolvedView.Observations.GBufferLocalDirectSubmissions;
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
			Record.Pipelines.erase({EMeshPipelinePass::GBuffer});
			Record.Pipelines.erase({EMeshPipelinePass::GBuffer, true});
		}
		bool Ready = true;
		ForEachShadowBucket(PreparedView, [&](const auto& Bucket) {
			for (const auto& Item : Bucket)
			{
				if (!FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Item.Command->Material.PlanningPassIdentity, {}, Item.Command->bSupportsGBuffer)) continue;
				const auto* Primitive = PreparedView.GetPrimitive(Item);
				if (!Primitive) { ResolvedView.Draws[Item.ResolvedIndex].bReady = false; Ready = false; continue; }
				const auto Declaration = ResolveMeshVertexDeclaration(*Primitive, Item);
				if (!Declaration) { ResolvedView.Draws[Item.ResolvedIndex].bReady = false; Ready = false; continue; }
				auto Pipeline = GBuffer.EnsurePipeline_RenderThread({.Material = Item.Command->PipelineKey.Material, .CompiledProgram = Item.Command->Material.CompiledProgram, .Rasterizer = Item.Command->PipelineKey.Rasterizer, .Depth = Item.Command->PipelineKey.Depth, .VertexDeclaration = Declaration, .FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey, .Topology = Item.Command->PipelineKey.Topology});
				ResolvedView.Draws[Item.ResolvedIndex].bReady = ResolvedView.IsReady(Item) && Pipeline != nullptr;
				Ready = Pipeline && Ready;
				ResolvedView.Draws[Item.ResolvedIndex].Pipelines[{EMeshPipelinePass::GBuffer}] = FGBufferRenderer::GetPipelineResources(Pipeline);
				if (bPrepareGPUCulling && Primitive->VertexDomain
					== EVertexDeformationDomain::Local
					&& Item.Command->Geometry.bIndexed
					&& (!Item.Command->Material.CompiledProgram
						|| (Item.Command->Material.CompiledProgram->GetShaderMap().RequiredStages & MaterialShaderEntryBit(EMaterialShaderEntry::GPUCullingVertex))))
				{
					auto GPUPipeline = GBuffer.EnsurePipeline_RenderThread({.Material = Item.Command->PipelineKey.Material, .CompiledProgram = Item.Command->Material.CompiledProgram, .Rasterizer = Item.Command->PipelineKey.Rasterizer, .Depth = Item.Command->PipelineKey.Depth, .VertexDeclaration = Declaration, .FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey, .bGPUCulling = true, .Failure = Failure, .Topology = Item.Command->PipelineKey.Topology});
					ResolvedView.Draws[Item.ResolvedIndex].Pipelines[{EMeshPipelinePass::GBuffer, true}] = FGBufferRenderer::GetPipelineResources(GPUPipeline);
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
		FMeshDrawCommandStateCache& BindingGroup,
		FRHIBuffer* IndirectArguments,
		uint64 IndirectArgumentOffset,
		FRHIBuffer* VisibleInstances,
		FRHIBuffer* InstanceTransforms
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.DrawGBufferSection");
		const auto& Record = ResolvedView.Draws[Item.ResolvedIndex];
		const bool bGPUCulling = IndirectArguments != nullptr;
		if (!bGPUCulling)
			if (const auto* Visible = ResolvedView.GetVisibleMeshDrawCommand(Item, {EMeshPipelinePass::GBuffer}))
				return Visible->SubmitDraw(CommandList, &BindingGroup);
		auto Command = ResolvedView.GetMeshDrawCommand(Item, {EMeshPipelinePass::GBuffer, bGPUCulling});
		if (!Command) return false;
		if (bGPUCulling)
		{
			if (!VisibleInstances || !InstanceTransforms) return false;
			const auto Factory = FindMeshVertexFactory(Command->Pipeline->FactoryKey); if (!Factory) return false;
			const FMeshPassProcessorBatch Batch{Item.Command->Material.PlanningPassIdentity, *Factory,
				{Item.Command->PipelineKey.Rasterizer, Item.Command->PipelineKey.Depth, Item.Command->PipelineKey.ColorBlend},
				Item.Command->PipelineKey.Topology, {}, Item.Command->bSupportsGBuffer, true};
			FMeshDrawCommandHandle Completed;
			FDynamicPassMeshDrawListContext Context(*ResolvedView.DrawCommandStorage, Command->Pipeline->GetPassKey(), Completed,
				Record.Commands.at(Command->Pipeline->GetPassKey()));
			const auto Result = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).BuildGPUCullingCommand(
				Batch, *Command, {VisibleInstances, 0, VisibleInstances->GetSize()},
				{InstanceTransforms, 0, InstanceTransforms->GetSize()}, IndirectArguments, IndirectArgumentOffset, Context);
			if (!Result) return false;
			Command = ResolvedView.DrawCommandStorage->GetCommand(Completed);
		}
		return Command->SubmitDraw(CommandList, &BindingGroup);
	}

	auto FStaticMeshRenderer::RecordPreparedSection(
		FRHICommandList& CommandList,
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Item,
		const FResolvedStaticMeshView& ResolvedView,
		bool bShadowDepth,
		bool bHybridRetained, FMeshDrawCommandStateCache* BindingGroup
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.DrawSection");
		check(CommandList.IsInsideRenderPass());
		if (Item.ResolvedIndex >= ResolvedView.Draws.size()) return false;
		const auto PassKey = FState::GetPassKey(Item, bShadowDepth, bHybridRetained);
		if (const auto* Visible = ResolvedView.GetVisibleMeshDrawCommand(Item, PassKey)) return Visible->SubmitDraw(CommandList, BindingGroup);
		const auto Command = ResolvedView.GetMeshDrawCommand(Item, FState::GetPassKey(Item, bShadowDepth, bHybridRetained));
		return Command && Command->SubmitDraw(CommandList, BindingGroup);

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
