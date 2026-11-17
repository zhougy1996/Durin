#include "Profiling/Profiling.h"
#include "Renderers/StaticMeshRenderer.h"
#include "GPUScene.h"
#include "SceneInfo.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/InstanceCulling.h"
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
#include "RHI.h"
#include "RHICapabilities.h"

namespace Durin
{
	using namespace RendererPrivate;
	namespace
	{

		auto ReduceGBufferMeshDrawCommandStats(const FMeshDrawCommandPassStats& Statistics,
			FStaticMeshRenderObservations& Observations, std::span<const uint8> Families) -> void
		{
			for (const auto& Draw : Statistics.DrawData)
			{
				require(Draw.SourceDrawIndex < Families.size());
				const bool bSpline = Families[Draw.SourceDrawIndex] != 0;
				(bSpline ? Observations.GBufferSplineAttemptedDraws : Observations.GBufferLocalAttemptedDraws) += Draw.NumSourceDraws;
				if (Draw.bSucceeded) (bSpline ? Observations.GBufferSplineSuccessfulDraws : Observations.GBufferLocalSuccessfulDraws) += Draw.NumSourceDraws;
				else (bSpline ? Observations.GBufferSplineRejectedDraws : Observations.GBufferLocalRejectedDraws) += Draw.NumSourceDraws;
				if (!bSpline && !Draw.bIndirect) Observations.GBufferLocalDirectSubmissions += Draw.NativeSubmissions;
			}
		}

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
		{ return Pipeline.GetInitializer().Pass == EMeshPipelinePass::ShadowDepth ? ESurfaceMaterialPass::MaskedShadow
			: Pipeline.GetInitializer().Pass == EMeshPipelinePass::GBuffer ? ESurfaceMaterialPass::GBuffer : ESurfaceMaterialPass::Forward; }
		auto IsCacheable(const FPreparedStaticMeshDraw& Draw, const FPreparedStaticMeshPrimitive& Primitive) const -> bool
		{
			return bPersistentDraws && Primitive.SceneInfo.Resolve() && Primitive.GeometryRecord && Primitive.VertexDomain == EVertexDeformationDomain::Local;
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
		ResolvedView.FallbackPrimitiveUniforms.clear();
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
		ResolvedView.FallbackPrimitiveUniforms.clear();
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
		const auto& Pass = std::as_const(ResolvedView).GetMeshDrawCommandPass({EMeshPipelinePass::ShadowDepth});
		const auto Result = Pass.Draw(CommandList, {}, &BindingGroup);
		Observations.AttemptedDraws = Result.Attempted; Observations.SuccessfulDraws = Result.Successful;
		Observations.RejectedDraws = Result.Rejected; Observations.NativeDrawSubmissions = Result.NativeSubmissions;
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
				std::shared_ptr<const FMeshPassPipeline> Candidate;
				const auto Preparation = Processor.PreparePipeline(Batch, FName(std::format("StaticMeshPipeline_{}", PipelineEntry.Index)),
					*ShaderMapPayload, VertexFactory.Declaration, Candidate);
				if (!Preparation) return FPipelineResult::Failure(MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::GraphicsPipeline, "StaticMeshPipeline", GetIdentityText(EffectivePipelineKey),
					ERenderResourceCreateErrorReason::PipelinePreparationRejected,
					ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual
				));
				return FPipelineResult::Success(std::move(Candidate));
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
		ResolvedView.CachedDrawLists = nullptr;
		if (Scene)
		{
			Scene->GPUScene->UpdateCPUData();
			ResolvedView.GPUScene = Scene->GPUScene.get();
			Scene->UpdateStaticMeshes();
			if (!Scene->CachedDrawLists) Scene->CachedDrawLists = std::make_shared<FCachedMeshDrawCommandStorage>();
			auto& DrawLists = *Scene->CachedDrawLists;
			if (DrawLists.Generation != Generation) { DrawLists.Reset(); DrawLists.Generation = Generation; }
			ResolvedView.CachedDrawLists = &DrawLists;
		}
		ResolvedView.FallbackPrimitiveUniforms.clear();
		ResolvedView.PersistentCommandsReused.assign(PreparedView.GetNumSections(), false);
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
			const auto& Draw = PreparedView.GetDraw(PreparedView.MaterialUniformGroups[Group].RepresentativeDraw);
			const auto* Binding = ResolvedView.GetMaterialBinding(Draw);
			if (!Binding || !SurfaceMaterials.UpdateUniformExpressionCache_RenderThread(CommandList, *Cache, Material.Representation, *Binding, GroupCollections[Group])) { if (!bAllowPartial) return false; RejectGroup(Group); continue; }
			if (Changed) ++ResolvedView.Observations.MaterialUniformUploads;
			GroupUniforms[Group] = Cache->UniformBuffer->GetRange();
		}
		// Probe each selected pass independently before constructing uniforms.
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			if (!ResolvedView.IsReady(Draw) || !Primitive || (!ResolvedView.CachedDrawLists || !State->IsCacheable(Draw, *Primitive))) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			Record.SelectedCachedCommands.clear();
			bool AllHit = true, Any = false;
			for (const auto& [PassKey, Pipeline] : Record.Pipelines)
			{
				if (!Pipeline) continue;
				Any = true;
				const auto Owner = Primitive->SceneInfo.Resolve(); if (!Owner) { AllHit = false; continue; }
				ResolvedView.CachedDrawLists->Attach(*Owner);
				const auto Pass = FState::MaterialPass(*Pipeline);
				auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				FResolvedStaticMeshView::FMaterialUniform CachedMaterial;
				const auto Existing = ResolvedView.CachedDrawLists->FindCachedCommand(FCachedMeshDrawCommandStorage::MakeKey(Draw.Command.get(), Primitive->PrimitiveId,
					Primitive->BatchId, *Pipeline, GroupUniforms[Draw.MaterialUniformIndex].Buffer), *Owner,
					Material ? nullptr : &CachedMaterial, Primitive->CollectedBinding.get());
				if (!Existing) { AllHit = false; continue; }
				Record.SelectedCachedCommands.emplace(PassKey, Existing);

				if (!Material && Requests[Draw.MaterialUniformIndex].contains(Pass))
				{
					Material = std::move(CachedMaterial); Material->CollectionUniforms = GroupCollections[Draw.MaterialUniformIndex];
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
			if (!ResolvedView.IsReady(Draw) || !Draw.Command) continue;
			const auto Factory = FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
			if (!Factory || !Factory->SupportsGPUScene()) NeededPrimitives[Draw.PrimitiveIndex] = true;
		}
		for (uint32 Index = 0; Index < PreparedView.Primitives.size(); ++Index)
		{
			if (!NeededPrimitives[Index]) continue;
			const auto& Primitive = PreparedView.Primitives[Index];
			const auto Uniform = BuildFallbackPrimitiveUniform(Primitive);
			FRHIUniformBufferRange Range;
			if (State->bPersistentDraws && Primitive.SceneInfo.Resolve())
			{
				auto& Buffer = Primitive.SceneInfo.Resolve()->GetPrimitiveUniformBuffer(Primitive.BatchId);
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
			ResolvedView.FallbackPrimitiveUniforms.emplace(Index, std::move(Range));
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
		if (!ResolvedView.GPUSceneDynamicContext || ResolvedView.GPUScenePrimitives.size() != PreparedView.Primitives.size()) return false;
		for (const auto& Primitive : ResolvedView.GPUScenePrimitives)
			if (const auto& Buffers = Primitive.Buffers; Buffers
				&& (Buffers->DynamicContext != ResolvedView.GPUSceneDynamicContext
					|| !Buffers->SceneData[0] || !Buffers->SceneData[1] || !Buffers->InstanceIds)) return false;
		ResolvedView.MeshDrawCommandPasses.clear();
		ResolvedView.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();

		ResolvedView.DynamicInstancingInstanceIds = {};
		FMeshDrawCommandBindingCache BindingCache;
		struct FPassBuildScope
		{
			FPassBuildScope(FCachedMeshDrawCommandStorage* CachedStorage, FDynamicMeshDrawCommandStorage& Storage,
				FParallelMeshDrawCommandPass& Pass, FMeshDrawCommandBindingCache& Cache)
				: DynamicContext(Storage, Pass, Cache)
			{ if (CachedStorage) CachedContext.emplace(*CachedStorage, Pass.GetPass(), Cache); }
			auto GetProcessor(EMeshPipelinePass Pass, bool bCached) -> FMeshPassProcessor&
			{
				auto& Processor = bCached ? CachedProcessor : DynamicProcessor;
				if (!Processor) Processor = FPassProcessorManager::CreateMeshPassProcessor(Pass,
					bCached ? static_cast<FMeshPassDrawListContext*>(&*CachedContext) : &DynamicContext);
				require(Processor); return *Processor;
			}
			std::optional<FCachedPassMeshDrawListContext> CachedContext;
			FDynamicPassMeshDrawListContext DynamicContext;
			std::unique_ptr<FMeshPassProcessor> CachedProcessor, DynamicProcessor;
		};
		std::map<FMeshPassKey, std::unique_ptr<FPassBuildScope>> BuildScopes;
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
			const bool Cacheable = ResolvedView.CachedDrawLists && State->IsCacheable(Draw, *Primitive);
			for (auto& [PassKey, Pipeline] : Record.Pipelines)
			{
				if (!Pipeline) continue;
				const auto Pass = FState::MaterialPass(*Pipeline);
				const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				bool Ready = Material.has_value();
				std::string Error = Ready ? std::string{} : "Missing prepared material uniform.";
				auto& Scope = BuildScopes[PassKey];
				if (!Scope) Scope = std::make_unique<FPassBuildScope>(ResolvedView.CachedDrawLists,
					*ResolvedView.DrawCommandStorage, ResolvedView.GetMeshDrawCommandPass(PassKey), BindingCache);
				const FMeshPassProcessorBatch Batch{Draw.Command->Material.PlanningPassIdentity, *Factory, {Draw.Command->PipelineKey.Rasterizer, Draw.Command->PipelineKey.Depth, Draw.Command->PipelineKey.ColorBlend, PassKey.Pass == EMeshPipelinePass::GBuffer && Pipeline->GetInitializer().Description.RenderTargets.NumColorRenderTargets == 5}, Draw.Command->PipelineKey.Topology, {}, Draw.Command->bSupportsGBuffer};
				std::shared_ptr<const FMeshDrawCommand> Cached;
				FCachedMeshDrawCommand CachedSelection;
				if (Ready)
				{
					if (Cacheable)
						if (const auto Selected = Record.SelectedCachedCommands.find(PassKey); Selected != Record.SelectedCachedCommands.end())
							CachedSelection = Selected->second;
					Cached = CachedSelection.Command;
					auto& Processor = Scope->GetProcessor(PassKey.Pass, Cacheable && !Cached);
					FMeshDrawCommandBuildInput Input{CommandList, *Primitive, Draw, Pipeline, *Material,
						ResolvedView.GetFallbackPrimitiveUniform(Draw.PrimitiveIndex), ResolvedView.ViewUniforms[Pass], Lighting, {}, LocalLights};
					const auto& SceneData = ResolvedView.GPUScenePrimitives[Draw.PrimitiveIndex];
					if (Factory->SupportsGPUScene() && SceneData.Buffers)
					{ Input.GPUScene = SceneData.Buffers->GetShaderParameters(); Input.InstanceIdOffset = SceneData.InstanceIdOffset; }

					if (Cacheable && !Cached)
					{
						const auto Result = Processor.AddMeshBatch(Batch, Input); Ready = static_cast<bool>(Result);
						if (!Ready) Error = Result.Error;
						if (Ready) CachedSelection = ResolvedView.CachedDrawLists->FindCachedCommand(
							FCachedMeshDrawCommandStorage::MakeKey(Draw.Command.get(), Primitive->PrimitiveId, Primitive->BatchId,
								*Pipeline, Material->Uniform.Buffer), *Primitive->SceneInfo.Resolve());
						if (Ready) { Cached = CachedSelection.Command; Ready = static_cast<bool>(Cached); if (!Ready) Error = "Published cached mesh command could not be selected."; }
					}
					if (Ready && Cached)
					{
						auto Group = BindingCache.PassGroups.find({Pipeline.get(), Input.GPUScene.GPUSceneInstanceIds.Buffer});
						if (Group == BindingCache.PassGroups.end())
						{
							auto PassBindings = std::make_shared<FMeshPassBindings>();
							const auto Result = Processor.PreparePassBindings(Input, BindingCache, *PassBindings);
							Ready = static_cast<bool>(Result);
							if (!Ready) Error = Result.Error;
							else Group = BindingCache.PassGroups.emplace(std::make_pair(Pipeline.get(), Input.GPUScene.GPUSceneInstanceIds.Buffer), std::move(PassBindings)).first;
						}
						if (Ready)
						{
							const auto Bias = bShadow && Draw.RasterBias ? *Draw.RasterBias : Cached->DepthBias;
							ResolvedView.GetMeshDrawCommandPass(PassKey).AddCachedMeshDrawCommand(Cached, CachedSelection.Info,
								Group->second, Bias, Input.InstanceIdOffset, Draw.ResolvedIndex);
						}
					}
					else if (Ready)
					{
						const auto Result = Processor.AddMeshBatch(Batch, Input, Cached); Ready = static_cast<bool>(Result);
						if (!Ready) Error = Result.Error;
					}
				}
				if (!Ready)
				{
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

	auto FStaticMeshRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FPreparedStaticMeshDraw& Item, FResolvedStaticMeshView& ResolvedView, FMeshPassKey Pass, FMeshDrawCommandStateCache* BindingGroup
	) -> void
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		FMeshDrawCommandStateCache LocalCache;
		auto* Cache = BindingGroup ? BindingGroup : &LocalCache;
		const uint32 Before = Cache->DrawCalls;
		++ResolvedView.Observations.AttemptedDraws;
		const auto* Visible = ResolvedView.GetVisibleMeshDrawCommand(Item, Pass);
		if (Visible && (Visible->SourceDrawIndex != Item.ResolvedIndex || Visible->SubmitDraw(CommandList, Cache)))
			++ResolvedView.Observations.SuccessfulDraws;
		else ++ResolvedView.Observations.RejectedDraws;
		ResolvedView.Observations.NativeDrawSubmissions += Cache->DrawCalls - Before;
	}

	auto FStaticMeshRenderer::ExecutePass_RenderThread(FRHICommandListImmediate& CommandList,
		FResolvedStaticMeshView& ResolvedView, FMeshPassKey PassKey, EMeshBasePass Bucket, const FSceneView& View) -> void
	{
		check(IsInRenderingThread()); check(CommandList.IsInsideRenderPass());
		const auto& Pass = std::as_const(ResolvedView).GetMeshDrawCommandPass(PassKey);
		const auto Range = Pass.GetBasePassRange(Bucket);
		FParallelCommandListSet ParallelCommandLists(View, CommandList);
		const auto Result = Pass.DispatchDraw(&ParallelCommandLists, CommandList, Range.FirstCommand, Range.NumCommands);
		ResolvedView.Observations.AttemptedDraws += Result.Attempted;
		ResolvedView.Observations.SuccessfulDraws += Result.Successful;
		ResolvedView.Observations.RejectedDraws += Result.Rejected;
		ResolvedView.Observations.NativeDrawSubmissions += Result.NativeSubmissions;
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
		FResolvedStaticMeshView& ResolvedView,
		const FSceneView& View,
		const FMeshDrawCommandOverrideArgs& OverrideArgs
	) -> FGeometryExecutionResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ExecuteGBuffer");
		check(IsInRenderingThread()); check(CommandList.IsInsideRenderPass());
		const auto& Pass = std::as_const(ResolvedView).GetMeshDrawCommandPass({EMeshPipelinePass::GBuffer});
		auto& O = ResolvedView.Observations;
		auto* Statistics = ResolvedView.GBufferPassStats;
		if (Statistics) Statistics->DrawData.clear();
		FParallelCommandListSet ParallelCommandLists(View, CommandList);
		const auto Result = Pass.DispatchDraw(&ParallelCommandLists, CommandList, OverrideArgs, Statistics);
		if (Statistics) ReduceGBufferMeshDrawCommandStats(*Statistics, O, ResolvedView.GBufferSourceFamilies);
		O.GBufferAttemptedDraws += Result.Attempted; O.GBufferSuccessfulDraws += Result.Successful; O.GBufferRejectedDraws += Result.Rejected;
		O.NativeDrawSubmissions += Result.NativeSubmissions; O.GBufferIndirectSubmissions += Result.IndirectSubmissions;
		O.GBufferLocalSkippedDraws += ResolvedView.GBufferSkippedSources[0]; O.GBufferSplineSkippedDraws += ResolvedView.GBufferSkippedSources[1];
		O.GBufferSkippedDraws += ResolvedView.GBufferSkippedSources[0] + ResolvedView.GBufferSkippedSources[1];
		check(O.GBufferAttemptedDraws == O.GBufferSuccessfulDraws + O.GBufferRejectedDraws);
		return {Result.bComplete, Result.bRenderedGeometry, O.GBufferAttemptedDraws, O.GBufferSuccessfulDraws, O.GBufferRejectedDraws, O.GBufferSkippedDraws};
	}

	auto FStaticMeshRenderer::NeedsGBufferModelData(const FPreparedStaticMeshView& View) -> bool
	{
		for (uint32 Index = 0; Index < View.GetNumSections(); ++Index)
		{
			const auto& Draw = View.GetDraw(Index);
			if (Draw.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel == EMaterialShadingModel::ClearCoat
				&& Draw.Command->bSupportsGBuffer) return true;
		}
		return false;
	}

	auto FStaticMeshRenderer::PrepareGBufferPipelines_RenderThread(FGBufferRenderer& GBuffer, const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView) -> bool
	{
		for (auto& Record : ResolvedView.Draws)
		{
			Record.Pipelines.erase({EMeshPipelinePass::GBuffer});
		}
		const bool bModelData = NeedsGBufferModelData(PreparedView);
		const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		if (bModelData && (!Caps || Caps->MaxColorAttachments < 5)) return false;
		bool Ready = true;
		ForEachShadowBucket(PreparedView, [&](const auto& Bucket) {
			for (const auto& Item : Bucket)
			{
				if (!FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::GBuffer).IsMeshRelevant(Item.Command->Material.PlanningPassIdentity, {}, Item.Command->bSupportsGBuffer)) continue;
				const auto* Primitive = PreparedView.GetPrimitive(Item);
				if (!Primitive) { ResolvedView.Draws[Item.ResolvedIndex].bReady = false; Ready = false; continue; }
				const auto Declaration = ResolveMeshVertexDeclaration(*Primitive, Item);
				if (!Declaration) { ResolvedView.Draws[Item.ResolvedIndex].bReady = false; Ready = false; continue; }
				auto Pipeline = GBuffer.EnsurePipeline_RenderThread({.Material = Item.Command->PipelineKey.Material, .CompiledProgram = Item.Command->Material.CompiledProgram, .Rasterizer = Item.Command->PipelineKey.Rasterizer, .Depth = Item.Command->PipelineKey.Depth, .VertexDeclaration = Declaration, .FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey, .Topology = Item.Command->PipelineKey.Topology, .bModelData = bModelData});
				ResolvedView.Draws[Item.ResolvedIndex].bReady = ResolvedView.IsReady(Item) && Pipeline != nullptr;
				Ready = Pipeline && Ready;
				ResolvedView.Draws[Item.ResolvedIndex].Pipelines[{EMeshPipelinePass::GBuffer}] = Pipeline;

			}
		});
		return Ready;
	}

	auto FStaticMeshRenderer::ReleaseResources_RenderThread() -> void
	{
		++State->ReleaseEpoch;
		check(IsInRenderingThread());
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}

} // namespace Durin
