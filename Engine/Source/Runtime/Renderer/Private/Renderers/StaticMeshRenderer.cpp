#include "Profiling/Profiling.h"
#include "Renderers/RenderStageTimings.h"
#include "Renderers/StaticMeshRenderer.h"
#include "GPUScene.h"
#include "SceneInfo.h"
#include "Scene.h"
#include "Renderers/SceneRenderingService.h"
#include "RenderPipelineCreation.h"
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
		auto IsCacheable(const FPreparedStaticMeshPrimitive& Primitive) const -> bool
		{
			return bPersistentDraws && Primitive.SceneInfo.Resolve() && Primitive.GeometryRecord && Primitive.VertexDomain == EVertexDeformationDomain::Local;
		}

		using FShaderMapPayload = FCachedMeshPassShaders;

		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload, FMaterialResourceCacheKeyHash>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<
			FMeshShaderMapKey,
			FShaderMapPayload, FMaterialResourceCacheKeyHash>
			ShadowShaderMaps{ERenderResourceGenerationDependency::Shader};
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
					|| !FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, EMeshPipelinePass::GBuffer).IsMeshRelevant(Item.Command->Material.PlanningPassIdentity, {}, Item.Command->bSupportsGBuffer);
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
			Record.Pipelines.erase({EMeshPipelinePass::SingleLayerWaterDepth});
			Record.Pipelines.erase({EMeshPipelinePass::SingleLayerWater});
		}
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			if (FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, EMeshPipelinePass::GBuffer).IsMeshRelevant(Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer))
				continue;
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			const auto* MaterialBinding = ResolvedView.GetMaterialBinding(Draw);
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			const auto Prepare = [&](FMeshPassKey PassKey) -> bool {
				return EnsureSectionResources_RenderThread(*Primitive, Draw, *MaterialBinding, Record.Pipelines[PassKey], FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, PassKey.Pass));
			};
			bool bDrawReady = false;
			if (Primitive && MaterialBinding)
			{
				const bool bWater = Draw.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel
					== EMaterialShadingModel::SingleLayerWater;
				if (bWater) bDrawReady = Prepare({EMeshPipelinePass::SingleLayerWaterDepth}) && Prepare({EMeshPipelinePass::SingleLayerWater});
				else bDrawReady = Prepare(FState::GetPassKey(Draw, false, true));
			}
			Record.bReady = Record.bReady && bDrawReady;
			bReady = bDrawReady && bReady;
		}
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
		const auto* CollectedBinding = Primitive ? Primitive->CollectedBinding.get() : nullptr;
		const bool bRetained =
			ResourceContext && Primitive && Primitive->StaticMeshes &&
			Primitive->VertexDomain == EVertexDeformationDomain::Local && Primitive->GeometryRecord &&
			Primitive->GeometryRecord == Draw.Command->GeometryOwner && Primitive->CollectedBinding &&
			Primitive->CollectedBinding == Primitive->GeometryRecord->GetBinding() &&
			Primitive->CollectedBinding->Declaration && !Primitive->CollectedBinding->Streams.empty() &&
			typeid(*CollectedBinding) == typeid(FStaticMeshBatchBinding);
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
		const auto Pass = FState::GetPassKey(Draw, bShadowDepth);
		const bool bReady = Primitive && (Policy != EStaticMeshSectionResourcePolicy::SamplersOnly
			? EnsureSectionResources_RenderThread(*Primitive, Draw, *Record.MaterialBinding, Record.Pipelines[Pass], FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, Pass.Pass))
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
		const FMeshPassProcessor& Processor
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.EnsureSectionResources");
		const auto Pass = Processor.GetPass();
		const bool bShadowDepth = Pass == EMeshPipelinePass::ShadowDepth;
		RendererPrivate::FRenderStageTimings Timings(false, bShadowDepth ? "ShadowSection" : "MeshSection",
			bShadowDepth && GRenderFrameCounterRenderThread <= 5 ? 0.25 : 20.0);
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
		const FMeshPassProcessorBatch Batch{Material.PlanningPassIdentity, *Factory,
			{Item.Command->PipelineKey.Rasterizer, Item.Command->PipelineKey.Depth, Item.Command->PipelineKey.ColorBlend},
			Item.Command->PipelineKey.Topology, {}, Item.Command->bSupportsGBuffer,
			Item.Command->PipelineKey.Depth.CompareOp == ERHIDepthCompareOp::GreaterOrEqual};

		using FShaderMapResult =
			TRenderResourceCreateResult<FState::FShaderMapPayload>;
		const FMeshShaderMapKey ShaderMapKey{
			.Material = Material.PlanningPassIdentity.ShaderMap,
			.VertexDomain = Primitive.VertexDomain,
			.FactoryKey = Item.Command->PipelineKey.FactoryKey, .LayoutKey = Item.Command->PipelineKey.LayoutKey,
			.FragmentPass = Pass == EMeshPipelinePass::SingleLayerWater || Pass == EMeshPipelinePass::SingleLayerWaterDepth
				? Pass : EMeshPipelinePass::Forward
		};
		Timings.Next("ShaderMapCache");
		auto& ShaderMapCache = bShadowDepth ? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderMapEntry = ShaderMapCache.FindOrAddBounded(
			ShaderMapKey, MaterialShaderMapCacheEntryBudget, MaterialShaderMapCacheByteBudget);
		Timings.Next("PrepareShaders");
		FState::FShaderMapPayload* ShaderMapPayload =
			ShaderMapEntry.Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[this, &Material, &Processor, &Batch, bShadowDepth]() -> FShaderMapResult {
					FState::FShaderMapPayload Candidate;
					FShaderCompileOptions Options;
					Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
					ERenderResourceCreateErrorCategory FailureCategory;
					const auto Result = Processor.PrepareShaders(Batch, Material.CompiledProgram.get(), Options, Candidate, &FailureCategory);
					if (Profiling::AreStartupDiagnosticsEnabled() && bShadowDepth && GRenderFrameCounterRenderThread <= 5)
						DURIN_DEBUG("Shadow shader preparation: frame={} | program={} | status={}",
							GRenderFrameCounterRenderThread, Material.PlanningPassIdentity.ShaderMap.ProgramIdentity.ToString(),
							static_cast<uint32>(Result.Status));
					if (Result.Status == ERenderResourceAvailability::Pending) return FShaderMapResult::Pending();
					if (Result.Status == ERenderResourceAvailability::Canceled) return FShaderMapResult::Canceled();
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

		Timings.Next("PreparePipeline");
		if (!Processor.IsMeshRelevant(Batch.Material, Batch.Participation,
			Batch.bSupportsGBuffer && Factory->GetShaderType(MaterialMeshPassGBuffer) != nullptr)) return false;
		auto Pipeline = Processor.ResolvePipeline(Batch, *ShaderMapPayload, VertexFactory.Declaration);
		if (!Pipeline) return false;
		Timings.Next("SurfaceMaterial");
		const ESurfaceMaterialPass SurfacePass = bShadowDepth
			? (Item.Command->PipelineKey.Material.ShaderMap.BlendMode
					== EMaterialBlendMode::Masked
				? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::OpaqueShadow)
			: ESurfaceMaterialPass::Forward;
		if (!SurfaceMaterials.Ensure_RenderThread(MaterialBinding, SurfacePass)) return false;
		OutPipeline = std::move(Pipeline);
		return true;
	}

	auto FStaticMeshRenderer::SupportsPersistentDraws() const -> bool { return State->bPersistentDraws; }

	namespace
	{
		auto PrepareCollectionUniforms(FRHICommandListImmediate& CommandList, FScene* Scene, const FMaterialRenderData& Material,
			std::map<const FUniformBuffer*, FRHIUniformBufferRange>& CollectionUniforms, std::vector<FRHIUniformBufferRange>& Ranges) -> bool
		{
			const auto& Program = Material.CompiledProgram;
			if (!Program) return true;
			for (const auto& Required : Program->ActiveCollections)
			{
				const auto Snapshot = Scene ? Scene->GetMaterialParameterCollection_RenderThread(Required.CollectionId) : nullptr;
				if (Snapshot && !Snapshot->Layout.HasCompatibleSchema(Required)) return false;
				const FByteView Payload = Snapshot ? FByteView(Snapshot->Payload) : FByteView(Required.DefaultPayload);
				if (Payload.size() != Required.UniformLayout.UniformPayloadSize) return false;
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
				Ranges.push_back(Local->second);
			}
			return true;
		}

		auto GetGBufferPipeline(FGBufferRenderer& GBuffer, const FPreparedStaticMeshPrimitive& Primitive,
			const FPreparedStaticMeshDraw& Draw, bool bModelData) -> std::shared_ptr<const FMeshPassPipeline>
		{
			const auto Declaration = ResolveMeshVertexDeclaration(Primitive, Draw);
			if (!Declaration) return {};
			const auto& Key = Draw.Command->PipelineKey;
			return GBuffer.EnsurePipeline_RenderThread({.Material = Key.Material, .CompiledProgram = Draw.Command->Material.CompiledProgram,
				.Rasterizer = Key.Rasterizer, .Depth = Key.Depth, .VertexDeclaration = Declaration, .FactoryKey = Key.FactoryKey,
				.LayoutKey = Key.LayoutKey, .Topology = Key.Topology, .bModelData = bModelData});
		}
	}

	namespace RendererPrivate
	{
		struct FStaticMeshCommandBuildContext
		{
			struct FMaterialGroup
			{
				uint32 Index;
				FResolvedStaticMeshView::FMaterialUniform Uniforms;
				bool bPrepared = false;
			};
			using FMaterialKey = std::tuple<uint64, const FUniformExpressionCache*, const FMaterialProgram*>;
			std::map<FMaterialKey, FMaterialGroup> MaterialGroups;
			FRHICommandListImmediate& Commands;
			FScene& Scene;
			FStaticMeshRenderer& Renderer;
			FGBufferRenderer& GBuffer;
			FCachedMeshDrawCommandStorage& Storage;
			FStaticMeshBatchCaptureContext& MaterialCapture;
			std::map<const FUniformBuffer*, FRHIUniformBufferRange> CollectionUniforms;
			FPrimitiveSceneInfo* Primitive = nullptr;
			uint32 NextBindingIndex = 0;

			auto GetMaterialGroup(const FMaterialRenderData& Material, const FMaterialRenderBinding& Binding,
				FSurfaceMaterialResources& Surfaces) -> FMaterialGroup*
			{
				const auto Cache = Material.UniformExpressionCache ? Material.UniformExpressionCache : Material.Representation.GetUniformExpressionCache();
				const FMaterialKey Key{Material.Representation.GetRecordId(), Cache.get(), Material.CompiledProgram.get()};
				if (const auto It = MaterialGroups.find(Key); It != MaterialGroups.end()) return &It->second;

				FMaterialGroup Group{static_cast<uint32>(MaterialGroups.size())};
				auto& Ranges = Group.Uniforms.CollectionUniforms;
				if (!PrepareCollectionUniforms(Commands, &Scene, Material, CollectionUniforms, Ranges)) return nullptr;
				if (!Material.Representation.GetUniformPayload().empty())
				{
					if (!Surfaces.UpdateUniformExpressionCache_RenderThread(Commands, *Cache, Material.Representation, Binding, Ranges)) return nullptr;
					Group.Uniforms.Uniform = Cache->UniformBuffer->GetRange();
				}
				return &MaterialGroups.emplace(Key, std::move(Group)).first->second;
			}
		};
	}

	auto FPrimitiveSceneInfo::CacheMeshDrawCommands(FScene* Scene, std::span<FPrimitiveSceneInfo* const> SceneInfos,
		FSceneRenderingService& Service) -> void
	{
		if (SceneInfos.empty() || !Service.StaticMeshRenderer.SupportsPersistentDraws()) return;
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.CacheStaticMeshDrawCommands");
		auto& Commands = FRHICommandListImmediate::Get();
		const auto Config = Service.GetPreparationConfig_RenderThread(Scene->GetFeatureLevel());
		// Hidden primitives may enqueue shader work without blocking the current view.
		FRenderPipelinePreparationBatch SceneBatch(false);
		FStaticMeshBatchCaptureContext MaterialCapture;
		auto& Storage = Scene->GetCachedMeshDrawCommands(Commands, Service.StaticMeshRenderer.Coordinator.GetGeneration_RenderThread());
		for (const auto Pass : FPassProcessorManager::GetRegisteredPasses(EShadingPath::Deferred))
		{
			if (!EnumHasAnyFlags(FPassProcessorManager::GetPassFlags(EShadingPath::Deferred, Pass), EMeshPassFlags::CachedMeshCommands)) continue;
			FStaticMeshCommandBuildContext BuildContext{.Commands = Commands, .Scene = *Scene, .Renderer = Service.StaticMeshRenderer,
				.GBuffer = Service.GBufferRenderer, .Storage = Storage, .MaterialCapture = MaterialCapture};
			FMeshDrawCommandBindingCache Bindings;
			FCachedPassMeshDrawListContext DrawListContext(Storage, {Pass}, Bindings, &BuildContext);
			auto Processor = FPassProcessorManager::CreateMeshPassProcessor(EShadingPath::Deferred, Pass, &DrawListContext);
			if (!Processor->IsEnabled(Config)) continue;
			for (auto* Primitive : SceneInfos)
			{
				const auto& List = Primitive->GetStaticMeshes();
				if (!List) continue;
				BuildContext.Primitive = Primitive;
				require(List->Meshes.size() <= INT32_MAX);
				for (int32 MeshIndex = 0; MeshIndex < static_cast<int32>(List->Meshes.size()); ++MeshIndex)
					Processor->AddMeshBatch(List->Meshes[MeshIndex], ~uint64(0), &Primitive->GetProxy(), MeshIndex);
			}
		}
		if (SceneBatch.HasPending())
			for (auto* Primitive : SceneInfos) Primitive->RequestStaticMeshUpdate();
	}

	auto FMeshPassProcessor::AddMeshBatch(const FMeshBatch& MeshBatch, uint64 BatchElementMask,
		const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId) -> void
	{
		if (MeshBatch.bUseForMaterial)
			ProcessStaticMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, EMeshCollectionPurpose::Receiver);
	}

	auto FMeshPassProcessor::ProcessStaticMeshBatch(const FMeshBatch& MeshBatch, uint64 BatchElementMask,
		const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId, EMeshCollectionPurpose Purpose) const -> void
	{
		check(DrawListContext && DrawListContext->StaticContext);
		auto& Context = *DrawListContext->StaticContext;
		auto& Owner = *Context.Primitive;
		const auto& List = Owner.GetStaticMeshes();
		check(PrimitiveSceneProxy == &Owner.GetProxy() && List);
		if (StaticMeshId == INDEX_NONE)
		{
			const auto Mesh = std::ranges::find_if(List->Meshes, [&](const FStaticMeshBatch& Entry) { return &Entry == &MeshBatch; });
			if (Mesh == List->Meshes.end()) return;
			StaticMeshId = static_cast<int32>(Mesh - List->Meshes.begin());
		}
		check(StaticMeshId >= 0 && &List->Meshes.at(StaticMeshId) == &MeshBatch);
		const bool bShadow = Purpose == EMeshCollectionPurpose::Shadow;
		const bool bGBuffer = GetVertexFactoryPass() == MaterialMeshPassGBuffer;
		// Retained sections and the unified shadow batch each expose one local element.
		if (!(BatchElementMask & 1) || !PrimitiveSceneProxy->GetRenderParticipation().IsRelevant(Purpose)) return;
		const auto& Transform = PrimitiveSceneProxy->GetPrimitiveSceneTransform();
		const auto Batch = List->Meshes[StaticMeshId].CaptureMeshBatch(Owner, Context.MaterialCapture);
		if (!Transform.bValid || FMeshBatchCollector(Purpose).Validate(Batch, &Transform) != EGeometrySubmissionOutcome::Submitted) return;
		const auto Element = Batch.GetElement(0);
		const auto Binding = std::dynamic_pointer_cast<const FVertexFactoryInputBinding>(Batch.Binding);
		const auto Factory = FindMeshVertexFactory(Batch.FactoryKey);
		if (!Binding || !Binding->Declaration || Binding->Streams.empty() || !Factory || Factory->GetLayoutKey() != Batch.LayoutKey
			|| !SupportsVertexFactory(*Factory) || !Factory->Supports(GetVertexFactoryPass(), Element.Draw)) return;
		const bool bSupportsGBuffer = Factory->Supports(MaterialMeshPassGBuffer, Element.Draw) && Factory->GetShaderType(MaterialMeshPassGBuffer);
		auto Material = Element.Material;
		if (Material.Representation.IsPending() || (Material.CompiledProgram && Material.CompiledProgram->FeatureLevel != PrimitiveSceneProxy->GetFeatureLevel())) return;
		FMaterialRenderBinding MaterialBinding;
		if (!ResolveMaterialBinding(Material, MaterialBinding, "CachedMeshPass")
			|| !IsMeshRelevant(Material.PlanningPassIdentity, PrimitiveSceneProxy->GetRenderParticipation(), bSupportsGBuffer)
			|| !std::isfinite(Material.PlanningPassIdentity.ShaderMap.OpacityMaskThreshold)) return;

		auto& Renderer = Context.Renderer;
		FPreparedStaticMeshPrimitive Primitive{.CustomPrimitiveData = Batch.CustomPrimitiveData, .CustomInstanceData = Batch.CustomInstanceData,
			.PrimitiveId = Batch.PrimitiveId, .BatchId = Batch.BatchId, .PrimitiveIdMode = Batch.PrimitiveIdMode,
			.PersistentPrimitiveIndex = Owner.GetPersistentIndex(), .InstanceSceneDataId = Batch.InstanceSceneDataId,
			.RequestedLODIndex = Batch.RequestedLOD, .SelectedLODIndex = Batch.SelectedLOD, .CollectedBinding = Binding,
			.GeometryRecord = Batch.GeometryRecord, .WorldBounds = Batch.WorldBounds, .LocalToWorld = Batch.LocalToWorld,
			.NormalToWorld = Transform.NormalToWorld, .WorldToLocal = Transform.WorldToLocal, .BoundsCenter = Batch.WorldBounds.GetCenter(),
			.StaticMeshes = List, .SceneInfo = Owner.GetSceneInfoRef()};
		auto Description = std::make_shared<FMeshDrawDescription>();
		Description->Material = Material;
		Description->GeometryOwner = Batch.GeometryRecord;
		Description->SectionIndex = Element.ElementId;
		Description->Geometry = Element.Draw;
		Description->Vertices = Element.Vertices;
		Description->Indices = Element.Indices;
		Description->MaterialSlotIndex = Element.MaterialSlotIndex;
		Description->bSupportsGBuffer = bSupportsGBuffer;
		auto& PipelineKey = Description->PipelineKey;
		PipelineKey.Material = Material.PlanningPassIdentity;
		PipelineKey.FactoryKey = Batch.FactoryKey;
		PipelineKey.LayoutKey = Batch.LayoutKey;
		PipelineKey.VertexDeclaration = Binding->Declaration;
		PipelineKey.Topology = Element.Draw.Topology;
		auto* Group = Context.GetMaterialGroup(Material, MaterialBinding, Renderer.SurfaceMaterials);
		if (!Group) return;
		auto& Uniforms = Group->Uniforms;
		FPreparedStaticMeshDraw Draw{.Command = Description, .StaticMeshIndex = static_cast<uint32>(StaticMeshId),
			.PrimitiveIndex = Context.NextBindingIndex++, .MaterialUniformIndex = Group->Index};
		FRHIUniformBufferRange PrimitiveUniform;
		if (!Factory->SupportsGPUScene())
		{
			const auto Parameters = BuildFallbackPrimitiveUniform(Primitive);
			auto& Buffer = Owner.GetPrimitiveUniformBuffer(Batch.BatchId);
			if (!Buffer.Update(Context.Commands, std::as_bytes(std::span(&Parameters, 1)))) return;
			PrimitiveUniform = Buffer.GetRange();
		}
		if (bGBuffer && !Renderer.EnsureMaterialSamplers_RenderThread(MaterialBinding)) return;
		const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const bool bSupportsModelData = bGBuffer && Caps && Caps->MaxColorAttachments >= 5;
		for (const bool bReversedZ : {false, true})
		{
			if (bShadow && bReversedZ) break;
			const auto State = MakeMeshPipelineState(Material.PlanningPassIdentity, {.bReverseCulling = Transform.Determinant < 0, .bReversedZ = bReversedZ});
			PipelineKey.Rasterizer = State.Rasterizer;
			PipelineKey.Depth = State.Depth;
			PipelineKey.ColorBlend = State.ColorBlend;
			for (const bool bModelData : {false, true})
			{
				if (bModelData && !bSupportsModelData) break;
				std::shared_ptr<const FMeshPassPipeline> Pipeline;
				if (bGBuffer) Pipeline = GetGBufferPipeline(Context.GBuffer, Primitive, Draw, bModelData);
				else Renderer.EnsureSectionResources_RenderThread(Primitive, Draw, MaterialBinding, Pipeline, *this);
				if (!Pipeline) continue;
				if (Context.Storage.FindCachedCommand(FCachedMeshDrawCommandStorage::MakeKey(Draw, Primitive, *Pipeline, Uniforms.Uniform.Buffer), Owner, nullptr, Binding.get())) continue;
				if (!Group->bPrepared)
				{
					FPreparedSurfaceMaterial Surface;
					if (!Renderer.SurfaceMaterials.Prepare_RenderThread(Context.Commands, &MaterialBinding, FStaticMeshRenderer::FState::MaterialPass(*Pipeline),
						{}, nullptr, nullptr, Surface, Uniforms.Uniform) || Surface.Uniform.ResourceOwner) return;
					Group->bPrepared = true;
					Uniforms.Surface = std::move(Surface.Surface);
					Uniforms.Uniform = Surface.Uniform;
				}
				FMeshDrawCommandBuildInput Input{Context.Commands, Primitive, Draw, Pipeline, Uniforms, PrimitiveUniform};
				const auto Built = BuildMeshDrawCommands(MakeMeshPassProcessorBatch(Input, *Factory), Input);
				if (!Built && Built.Status == EShaderBuildStatus::Error)
					DURIN_ERROR("Static mesh command cache build failed: pass={}, error={}", static_cast<uint8>(GetPass()), Built.Error);
			}
		}
	}

	auto FStaticMeshRenderer::PrepareUniforms_RenderThread(FRHICommandListImmediate& CommandList,
		FScene* Scene, const FSceneView& View, const FPreparedStaticMeshView& PreparedView,
		FResolvedStaticMeshView& ResolvedView, std::vector<FStaticMeshCommandBuildRequest>& CacheRequests, bool bProductionDeferred,
		bool bGBuffer, bool bShadow, bool bAllowPartial) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareMeshUniformGroups");
		check(!CommandList.IsInsideRenderPass());
		const auto Generation = Coordinator.GetGeneration_RenderThread();
		const FCachedMeshDrawCommandStorage* CachedDrawLists = nullptr;
		if (Scene)
		{
			Scene->GPUScene->UpdateCPUData();
			ResolvedView.GPUScene = Scene->GPUScene.get();
			CachedDrawLists = &Scene->GetCachedMeshDrawCommands(CommandList, Generation);
		}
		ResolvedView.FallbackPrimitiveUniforms.clear();
		ResolvedView.PersistentCommandsReused.assign(PreparedView.GetNumSections(), false);
		ResolvedView.SelectedShadowCommands = 0;
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
					const bool bDeferredDraw = FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, EMeshPipelinePass::GBuffer).IsMeshRelevant(Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer);
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
			if (!PrepareCollectionUniforms(CommandList, Scene, Draw.Command->Material, CollectionUniforms, GroupCollections[Group]))
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
		// Resolve hits before uniform preparation. Only misses need stable build inputs.
		std::vector<std::pair<uint32, FMeshPassKey>> MissingCommands;
		for (uint32 Index = 0; Index < PreparedView.GetNumSections(); ++Index)
		{
			const auto& Draw = PreparedView.GetDraw(Index);
			const auto* Primitive = PreparedView.GetPrimitive(Draw);
			if (!ResolvedView.IsReady(Draw) || !Primitive || (!CachedDrawLists || !State->IsCacheable(*Primitive))) continue;
			auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
			Record.SelectedCachedCommands.clear();
			bool AllHit = true, Any = false;
			for (const auto& [PassKey, Pipeline] : Record.Pipelines)
			{
				if (!Pipeline) continue;
				Any = true;
				const auto Owner = Primitive->SceneInfo.Resolve(); if (!Owner) { AllHit = false; continue; }
				const auto Pass = FState::MaterialPass(*Pipeline);
				auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				FResolvedStaticMeshView::FMaterialUniform CachedMaterial;
				const auto Existing = CachedDrawLists->FindCachedCommand(FCachedMeshDrawCommandStorage::MakeKey(Draw, *Primitive, *Pipeline, GroupUniforms[Draw.MaterialUniformIndex].Buffer), *Owner,
					Material ? nullptr : &CachedMaterial, Primitive->CollectedBinding.get());
				if (!Existing)
				{
					if (!Primitive->StaticMeshes) MissingCommands.emplace_back(Index, PassKey);
					else if (View.Settings.Mode.RasterMode == ERasterMode::Solid && GroupUniforms[Draw.MaterialUniformIndex].Buffer)
						Owner->RequestStaticMeshUpdate();
					AllHit = false;
					continue;
				}
				Record.SelectedCachedCommands.emplace(PassKey, Existing);
				if (bShadow) ++ResolvedView.SelectedShadowCommands;

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
		if (!MissingCommands.empty())
		{
			CacheRequests.reserve(MissingCommands.size());
			for (const auto& [Index, Key] : MissingCommands)
			{
				const auto& Draw = PreparedView.GetDraw(Index);
				if (!ResolvedView.IsReady(Draw)) continue;
				auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
				const auto& Pipeline = Record.Pipelines.at(Key);
				const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][FState::MaterialPass(*Pipeline)];
				// Scene commands can outlive the preparation that owns a single-frame uniform.
				if (!Material || Material->Uniform.ResourceOwner) continue;
				CacheRequests.push_back({{CommandList, *PreparedView.GetPrimitive(Draw), Draw, Pipeline, *Material,
					ResolvedView.GetFallbackPrimitiveUniform(Draw.PrimitiveIndex)}, Record.SelectedCachedCommands[Key]});
			}
		}
		DURIN_PROFILE_CPU_ZONE_TEXT(std::format("primitives={} material_groups={} primitive_uploads={} material_uploads={}",
			PreparedView.Primitives.size(), PreparedView.MaterialUniformGroups.size(),
			ResolvedView.Observations.PrimitiveUniformUploads, ResolvedView.Observations.MaterialUniformUploads));
		return true;
	}

	auto FStaticMeshRenderer::PrepareBindings_RenderThread(FRHICommandListImmediate& CommandList, FMeshDrawCommandBindingCache& BindingCache,
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
		if (bShadow && ResolvedView.SelectedShadowCommands)
			ResolvedView.GetMeshDrawCommandPass({EMeshPipelinePass::ShadowDepth})
				.ReserveCachedCommands(ResolvedView.SelectedShadowCommands);
		ResolvedView.DrawCommandStorage = std::make_shared<FDynamicMeshDrawCommandStorage>();

		ResolvedView.DynamicInstancingInstanceIds = {};
		struct FDynamicPass
		{
			FDynamicPass(FDynamicMeshDrawCommandStorage& Storage, FParallelMeshDrawCommandPass& Pass, FMeshDrawCommandBindingCache& Cache)
				: Context(Storage, Pass, Cache), Processor(FPassProcessorManager::CreateMeshPassProcessor(EShadingPath::Deferred, Pass.GetPass().Pass, &Context)) {}
			FDynamicPassMeshDrawListContext Context;
			std::unique_ptr<FMeshPassProcessor> Processor;
		};
		std::map<FMeshPassKey, FDynamicPass> DynamicPasses;
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
			for (auto& [PassKey, Pipeline] : Record.Pipelines)
			{
				if (!Pipeline) continue;
				const auto Pass = FState::MaterialPass(*Pipeline);
				const auto& Material = ResolvedView.MaterialUniforms[Draw.MaterialUniformIndex][Pass];
				auto& MeshPass = ResolvedView.GetMeshDrawCommandPass(PassKey);
				auto PreparePass = [&]() -> FShaderCompletion {
					if (!Material) return FShaderCompletion::Failure("Missing prepared material uniform.");

					const auto Selected = Record.SelectedCachedCommands.find(PassKey);
					const bool Cacheable = Selected != Record.SelectedCachedCommands.end();
					if (Cacheable && (!Selected->second || Selected->second.Info->CommandIndex == INDEX_NONE))
						return FShaderCompletion::Failure("Scene did not publish a current static mesh command.");
					const auto& SceneData = ResolvedView.GPUScenePrimitives[Draw.PrimitiveIndex];
					const auto* Buffers = Factory->SupportsGPUScene() ? SceneData.Buffers.get() : nullptr;
					const uint32 InstanceOffset = Buffers ? SceneData.InstanceIdOffset : INDEX_NONE_U32;
					const auto& Shaders = Pipeline->GetInitializer();
					const FMeshDrawCommandBindingCache::FPassGroupKey GroupKey{PassKey.Pass, Shaders.VertexShader.get(),
						Shaders.FragmentLayout, Buffers ? Buffers->InstanceIds.GetReference() : nullptr};
					auto Group = Cacheable ? BindingCache.PassGroups.find(GroupKey) : BindingCache.PassGroups.end();

					if (!Cacheable || Group == BindingCache.PassGroups.end())
					{
						FMeshDrawCommandBuildInput Input{CommandList, *Primitive, Draw, Pipeline, *Material,
							ResolvedView.GetFallbackPrimitiveUniform(Draw.PrimitiveIndex), ResolvedView.ViewUniforms[Pass], Lighting, {}, LocalLights};
						if (Buffers) Input.GPUScene = Buffers->GetShaderParameters();
						Input.InstanceIdOffset = InstanceOffset;

						if (!Cacheable)
						{
							auto& Dynamic = DynamicPasses.try_emplace(PassKey, *ResolvedView.DrawCommandStorage, MeshPass, BindingCache).first->second;
							return Dynamic.Processor->BuildMeshDrawCommands(MakeMeshPassProcessorBatch(Input, *Factory), Input);
						}
						auto Bindings = std::make_shared<FMeshPassBindings>();
						const auto Built = FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, PassKey.Pass).PreparePassBindings(Input, BindingCache, *Bindings);
						if (!Built) return Built;
						Group = BindingCache.PassGroups.emplace(GroupKey, std::move(Bindings)).first;
					}
					auto& Cached = Selected->second;
					const auto Bias = bShadow && Draw.RasterBias ? *Draw.RasterBias : Cached.Command->DepthBias;
					MeshPass.AddCachedMeshDrawCommand(std::move(Cached), Group->second, Bias, InstanceOffset, Draw.ResolvedIndex);
					return {};
				};
				if (const auto Result = PreparePass(); !Result)
				{
					DURIN_ERROR("Mesh pass binding collection failed: pass={}, error={}", static_cast<uint8>(PassKey.Pass), Result.Error);
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
		FResolvedStaticMeshView& ResolvedView, std::optional<bool> ModelData) -> bool
	{
		for (auto& Record : ResolvedView.Draws)
		{
			Record.Pipelines.erase({EMeshPipelinePass::GBuffer});
		}
		const bool bModelData = ModelData ? *ModelData : NeedsGBufferModelData(PreparedView);
		const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		if (bModelData && (!Caps || Caps->MaxColorAttachments < 5)) return false;
		bool Ready = true;
		ForEachShadowBucket(PreparedView, [&](const auto& Bucket) {
			for (const auto& Item : Bucket)
			{
				if (!FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, EMeshPipelinePass::GBuffer).IsMeshRelevant(Item.Command->Material.PlanningPassIdentity, {}, Item.Command->bSupportsGBuffer)) continue;
				const auto* Primitive = PreparedView.GetPrimitive(Item);
				if (!Primitive) { ResolvedView.Draws[Item.ResolvedIndex].bReady = false; Ready = false; continue; }
				auto Pipeline = GetGBufferPipeline(GBuffer, *Primitive, Item, bModelData);
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
	}

} // namespace Durin
