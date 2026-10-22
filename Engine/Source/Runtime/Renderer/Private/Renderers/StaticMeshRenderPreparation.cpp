#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include <typeinfo>
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"
#include "SceneInfo.h"
#include "Rendering/MeshBatch.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Renderers/MeshVertexFactory.h"
#include "Profiling/Profiling.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	using namespace RendererPrivate;
	auto PrepareMeshViewFacts(std::vector<FMeshViewPreparationInput> Inputs,
		const FSceneView& View, bool bAllowTasks) -> FMeshViewPreparationFacts
	{
		auto Owner = std::make_shared<const std::vector<FMeshViewPreparationInput>>(std::move(Inputs));
		// Capture only the fields used by projected-size calculation and LOD policy.
		FSceneView FrozenView;
		FrozenView.ViewProjectionMatrix = View.ViewProjectionMatrix;
		FrozenView.ViewMatrix = View.ViewMatrix;
		FrozenView.ProjectionMatrix = View.ProjectionMatrix;
		FrozenView.ViewportWidth = View.ViewportWidth;
		FrozenView.ViewportHeight = View.ViewportHeight;
		FrozenView.Settings.Mode = View.Settings.Mode;
		FrozenView.DepthConvention = View.DepthConvention;
		auto BuildRange = [Owner, FrozenView](size_t First, size_t Count) {
			std::vector<FMeshViewPreparationFact> Facts;
			Facts.reserve(Count);
			for (size_t Index = First; Index < First + Count; ++Index)
			{
				const auto& Input = (*Owner)[Index];
				auto& Fact = Facts.emplace_back();
				Fact.StaticBatches = Input.StaticBatches;
				Fact.Projected = ComputeProjectedScreenSize(FrozenView, Input.WorldBounds);
				if (Input.StaticBatches || Input.LODs)
				{
					const auto LODs = Input.StaticBatches ? std::span<const FMeshLODSelectionEntry>(Input.StaticBatches->Geometry->LODs)
						: std::span<const FMeshLODSelectionEntry>(*Input.LODs);
					const uint32 Requested = FrozenView.Settings.Mode.LODMode == EViewLODMode::ForceLOD0 ? 0u
						: SelectStaticMeshLOD(Fact.Projected.NormalizedScreenSize, LODs);
					Fact.LOD = FPreparedMeshLODSelection{Requested, ResolveAvailableStaticMeshLOD(Requested, LODs)};
				}
			}
			return Facts;
		};
		if (!bAllowTasks || Owner->size() < 256 || !IsTaskSchedulerRunning())
			return {BuildRange(0, Owner->size()), 0};
		struct FPending
		{
			std::vector<Tasks::TTask<std::vector<FMeshViewPreparationFact>>> Work;
			~FPending()
			{
				for (auto& Task : Work) if (Task.IsValid()) Tasks::Cancel(Task);
				for (auto& Task : Work) if (Task.IsValid()) require(Task.Wait().WaitStatus == ETaskWaitStatus::Completed);
			}
		} Pending;
		FMeshViewPreparationFacts Result;
		Result.Primitives.reserve(Owner->size());
		for (size_t First = 0; First < Owner->size();)
		{
			Pending.Work.clear();
			while (First < Owner->size() && Pending.Work.size() < 8)
			{
				const size_t Count = std::min(size_t(128), Owner->size() - First);
				Pending.Work.push_back(Tasks::LaunchIndependentTask("Renderer.PrepareViewLOD",
					[BuildRange, First, Count] { return BuildRange(First, Count); }));
				First += Count;
				++Result.TaskCount;
			}
			for (auto& Task : Pending.Work)
			{
				const auto Wait = Task.Wait();
				require(Wait.WaitStatus == ETaskWaitStatus::Completed);
				if (Wait.TaskState != ETaskState::Succeeded) return {BuildRange(0, Owner->size()), Result.TaskCount};
				auto Facts = std::move(Task.GetResult());
				Result.Primitives.insert(Result.Primitives.end(), std::make_move_iterator(Facts.begin()), std::make_move_iterator(Facts.end()));
			}
		}
		return Result;
	}
	namespace
	{
		auto MaterialFactHash(const FMaterialRenderRepresentation& Representation) -> uint64
		{
			return Representation.GetContentHash();
		}
	}

	auto FMeshDrawDescriptionCache::BeginSubmission() -> void
	{
		for (auto& [Key, Entry] : Entries) Entry.bUsed = false;
		for (auto& [Key, Entry] : StaticFacts)
			Entry.bUsed = false;
		for (auto& [Key, Entry] : StaticSections)
			Entry.bUsed = false;
	}

	auto FMeshDrawDescriptionCache::EndSubmission() -> void
	{
		auto Trim = [](auto& Table) {
			// Keep the active working set; scan idle entries once even after large scene changes.
			for (auto It = Table.begin(); Table.size() > 512 && It != Table.end();)
			{
				if (!It->second.bUsed)
					It = Table.erase(It);
				else
					++It;
			}
		};
		Trim(Entries);
		Trim(StaticSections);
		Trim(StaticFacts);
	}

	auto FMeshDrawDescriptionCache::Find(const FKey& Key, const FMaterialRenderData& Material)
		-> std::shared_ptr<const FMeshDrawDescription>
	{
		const auto It = Entries.find(Key);
		if (It == Entries.end() || It->second.Command->Material.PlanningPassIdentity != Material.PlanningPassIdentity
			|| It->second.Command->Material.CompiledProgram != Material.CompiledProgram) return {};
		It->second.bUsed = true;
		return It->second.Command;
	}

	auto FMeshDrawDescriptionCache::Store(const FKey& Key, std::shared_ptr<const FMeshDrawDescription> Command) -> void
	{
		Entries.insert_or_assign(Key, FEntry{std::move(Command), true});
	}

	auto FMeshDrawDescriptionCache::CaptureStaticFacts(const std::shared_ptr<const FStaticMeshBatchInputs>& Inputs,
		bool bShadow) -> std::shared_ptr<const FStaticMeshSectionFacts>
	{
		check(IsInRenderingThread());
		if (!Inputs || !Inputs->Geometry) return {};
		const FStaticKey Key{reinterpret_cast<uintptr_t>(Inputs.get()), bShadow ? 1u : 0u};
		if (const auto Found = StaticFacts.find(Key); Found != StaticFacts.end())
		{
			Found->second.bUsed = true;
			return Found->second.Value;
		}
		auto Binding = std::dynamic_pointer_cast<const FVertexFactoryInputBinding>(Inputs->Geometry->GetBinding());
		if (!Binding || !Binding->Declaration || Binding->Streams.empty()) return {};
		// Only the ordinary local factory has an audited view-independent Section contract.
		// Custom factories retain per-view preparation until they opt into an equivalent contract.
		if (typeid(*Binding) != typeid(FStaticMeshBatchBinding)) return {};
		auto Factory = FindMeshVertexFactory(Binding->GetFactoryKey());
		// Missing registrations may arrive later; never retain a missing-factory result.
		if (!Factory) return {};
		auto Facts = std::make_shared<FStaticMeshSectionFacts>();
		Facts->Inputs = Inputs;
		Facts->Binding = std::move(Binding);
		Facts->Factory = std::move(Factory);
		Facts->bFactoryCompatible = Facts->Factory->GetLayoutKey() == Facts->Binding->GetLayoutKey();
		Facts->Elements.resize(Inputs->Geometry->GetElements().size());
		if (Facts->bFactoryCompatible)
			for (size_t Index = 0; Index < Facts->Elements.size(); ++Index)
			{
				auto& Element = Facts->Elements[Index];
				const auto& Draw = Inputs->Geometry->GetElements()[Index].Draw;
				Element.bSupportsPreparation =
					Facts->Factory->Supports(bShadow ? MaterialMeshPassShadow : MaterialMeshPassForward, Draw);
				if (Element.bSupportsPreparation)
					Element.bSupportsGBuffer = Facts->Factory->Supports(MaterialMeshPassGBuffer, Draw);
			}
		StaticFacts.emplace(Key, FFactsEntry{Facts, true});
		return Facts;
	}
	auto FMeshDrawDescriptionCache::FindStaticSections(const std::shared_ptr<const FStaticMeshBatchInputs>& Inputs,
		uint64 Policy) -> std::shared_ptr<const FStaticMeshPassSections>
	{
		check(IsInRenderingThread());
		const auto Found = StaticSections.find(FStaticKey{reinterpret_cast<uintptr_t>(Inputs.get()), Policy});
		if (Found == StaticSections.end()) return {};
		Found->second.bUsed = true;
		return Found->second.Value;
	}
	auto FMeshDrawDescriptionCache::StoreStaticSections(
		uint64 Policy, std::shared_ptr<const FStaticMeshPassSections> Sections) -> void
	{
		check(IsInRenderingThread());
		const FStaticKey Key{reinterpret_cast<uintptr_t>(Sections->Facts->Inputs.get()), Policy};
		StaticSections.insert_or_assign(Key, FSectionsEntry{std::move(Sections), true});
	}

	auto FStaticMeshPreparationCache::ResolveTransform(FPrimitiveComponentId PrimitiveId,
		uint64 BatchId, const FMatrix& LocalToWorld) -> const FTransform&
	{
		const auto& Fact = Admission.ResolveTransform(PrimitiveId, BatchId, LocalToWorld);
		TransformBuilds = Admission.GetTransformBuilds();
		return Fact;
	}

	auto FStaticMeshPreparationCache::ResolveMaterial(FMaterialRenderData& Material) -> std::optional<uint32>
	{
		const auto& Representation = Material.Representation;
		const uint64 RecordId = Representation.GetRecordId();
		if (const auto It = MaterialRecordIndices.find(RecordId); It != MaterialRecordIndices.end())
			return It->second;
		const uint64 Hash = MaterialFactHash(Representation);
		const auto [Begin, End] = MaterialIndices.equal_range(Hash);
		for (auto It = Begin; It != End; ++It)
		{
			const auto& Cached = Materials[It->second];
			// Hash collisions and equal uniforms with different textures/layouts are distinct.
			if (Cached.IsError() == Representation.IsError()
				&& Cached.GetLayout() == Representation.GetLayout()
				&& std::ranges::equal(Cached.GetUniformPayload(), Representation.GetUniformPayload())
				&& std::ranges::equal(Cached.GetResources(), Representation.GetResources())
				&& std::ranges::equal(Cached.GetSamplers(), Representation.GetSamplers())
				&& std::ranges::equal(Cached.GetTextureFallbacks(), Representation.GetTextureFallbacks()))
			{
				MaterialRecordIndices.emplace(RecordId, It->second);
				return It->second;
			}
		}
		++MaterialBuilds;
		FMaterialRenderBinding Binding;
		if (!ResolveMaterialBinding(Material, Binding, "StaticMeshMaterialSelection")) return std::nullopt;
		const uint32 Index = static_cast<uint32>(Materials.size());
		Materials.push_back(Material.Representation);
		// Resolution may have selected ErrorMaterial; hash its final representation.
		MaterialIndices.emplace(MaterialFactHash(Material.Representation), Index);
		MaterialRecordIndices.emplace(Material.Representation.GetRecordId(), Index);
		return Index;
	}

	auto CollectStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View, ERasterMode RasterMode, ERenderPreparationMode Mode,
		FStaticMeshPreparationCache* SharedCache)
		-> std::shared_ptr<const FCollectedStaticMeshView>
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.CollectMeshView");

		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(), "Mesh collection must precede scene render passes.");
		auto Collected = std::make_shared<FCollectedStaticMeshView>();
		// Only retain policy and matrices consumed by mesh preparation, not editor
		// overlays, temporal history, or unrelated per-view resources.
		Collected->View.ViewMatrix = View.ViewMatrix;
		Collected->View.ProjectionMatrix = View.ProjectionMatrix;
		Collected->View.ViewLocation = View.ViewLocation;
		Collected->View.DepthConvention = View.DepthConvention;
		Collected->View.Settings.Mode = View.Settings.Mode;
		Collected->RasterMode = RasterMode;
		Collected->Mode = Mode;
		Collected->Primitives.reserve(SceneInfos.size());
		std::vector<FMeshViewPreparationInput> ViewInputs;
		ViewInputs.reserve(SceneInfos.size());

		FStaticMeshBatchCaptureContext LocalCapture;
		auto& Capture = SharedCache ? SharedCache->StaticBatchCapture : LocalCapture;
		for (const auto* SceneInfo : SceneInfos)
		{
			auto& Input = ViewInputs.emplace_back();
			if (!SceneInfo) continue;
			Input.WorldBounds = SceneInfo->GetWorldBounds();
			const auto& Proxy = SceneInfo->GetProxy();
			// Extensions with custom collection stay dynamic until they explicitly
			// provide their own retained contract.
			if (typeid(Proxy) == typeid(FStaticMeshSceneProxy))
				Input.StaticBatches = static_cast<const FStaticMeshSceneProxy&>(Proxy).CaptureStaticMeshBatches_RenderThread(Capture);
			if (!Input.StaticBatches) Input.LODs = Proxy.CaptureLODSelection_RenderThread();
		}
		const auto ViewFacts = PrepareMeshViewFacts(std::move(ViewInputs), View);

		size_t PrimitiveIndex = 0;
		for (const auto* SceneInfo : SceneInfos)
		{
			const auto& ViewFact = ViewFacts.Primitives[PrimitiveIndex++];
			auto& Primitive = Collected->Primitives.emplace_back();
			if (!SceneInfo) continue;
			Primitive.bPresent = true;
			Primitive.bRetainedStaticMesh = ViewFact.StaticBatches != nullptr;
			Primitive.Id = SceneInfo->GetId();
			Primitive.bSplineMesh = SceneInfo->GetKind() == EPrimitiveSceneProxyKind::SplineMesh;
			Primitive.WorldBounds = SceneInfo->GetWorldBounds();
			const auto& ProjectedSize = ViewFact.Projected;
			Primitive.bProjectedSizeFallback = ProjectedSize.Status != EProjectedScreenSizeStatus::Valid;
			FMeshCollectionContext Context;
			Context.PreparedLOD = ViewFact.LOD;
			Context.PrimitiveId = SceneInfo->GetId();
			Context.CustomPrimitiveData = SceneInfo->GetProxy().GetCustomPrimitiveData();
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
			FMeshBatchCollector Collector(Context.Purpose, SharedCache ? &SharedCache->Admission : nullptr);
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.CollectMeshBatches");

				if (ViewFact.StaticBatches) CollectPublishedStaticMeshBatch(*ViewFact.StaticBatches, Context, Collector);
				else SceneInfo->GetProxy().CollectMeshBatches(Context, Collector);
			}
			for (size_t I = 0; I < Primitive.Outcomes.size(); ++I)
				Primitive.Outcomes[I] = Collector.GetOutcomeCount(static_cast<EGeometrySubmissionOutcome>(I));
			for (auto& Batch : std::move(Collector).TakeBatches())
			{

				auto& Captured = Primitive.Batches.emplace_back();
				Captured.Batch = std::move(Batch);
				if (Captured.Batch.StaticInputs && SharedCache && SharedCache->Commands)
					Captured.StaticFacts = SharedCache->Commands->CaptureStaticFacts(
						Captured.Batch.StaticInputs, Mode == ERenderPreparationMode::ShadowDepth);
				if (Captured.StaticFacts)
				{
					Captured.InputBinding = Captured.StaticFacts->Binding;
					Captured.bFactoryCompatible = Captured.StaticFacts->bFactoryCompatible;
					continue;
				}
				Captured.InputBinding = std::dynamic_pointer_cast<const FVertexFactoryInputBinding>(Captured.Batch.Binding);
				if (!Captured.InputBinding || !Captured.InputBinding->Declaration || Captured.InputBinding->Streams.empty()) continue;
				const auto Factory = FindMeshVertexFactory(Captured.Batch.FactoryKey);
				Captured.bFactoryCompatible = Factory && Factory->GetLayoutKey() == Captured.Batch.LayoutKey;
				if (!Captured.bFactoryCompatible) continue;
				Captured.Elements.reserve(Captured.Batch.GetNumElements());
				for (size_t Index = 0; Index < Captured.Batch.GetNumElements(); ++Index)
				{
					const auto Element = Captured.Batch.GetElement(Index);
					auto& Facts = Captured.Elements.emplace_back();
					if (!Captured.Batch.GeometryRecord)
					{
						DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ValidateMeshInputs");
						Facts.InputOutcome = ValidateMeshGeometryInputs(*Captured.InputBinding,
							Element.Draw, Element.Vertices, Element.InstanceStreams);
					}
					if (Facts.InputOutcome != EGeometrySubmissionOutcome::Submitted) continue;
					Facts.bSupportsPreparation = Factory->Supports(Mode == ERenderPreparationMode::ShadowDepth
						? MaterialMeshPassShadow : MaterialMeshPassForward, Element.Draw);
					if (Facts.bSupportsPreparation)
						Facts.bSupportsGBuffer = Factory->Supports(MaterialMeshPassGBuffer, Element.Draw);
				}
			}
		}
		return Collected;
	}

	auto PrepareStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View, ERasterMode RasterMode,
		ERenderPreparationMode Mode, FStaticMeshPreparationCache* SharedCache) -> FPreparedStaticMeshView
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareStaticMeshes");
		DURIN_PROFILE_CPU_ZONE_TEXT(std::format("candidates={} shadow={}", SceneInfos.size(), Mode == ERenderPreparationMode::ShadowDepth));
		const auto Collected = CollectStaticMeshView_RenderThread(CommandList, SceneInfos, View, RasterMode, Mode, SharedCache);
		return PrepareCollectedStaticMeshView_RenderThread(CommandList, Collected, SharedCache);
	}

	auto ResolveCollectedStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::shared_ptr<const FCollectedStaticMeshView> Collected,
		FStaticMeshPreparationCache* SharedCache) -> std::shared_ptr<const FStaticMeshPreparationInputs>
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ResolveCollectedMeshes");

		check(IsInRenderingThread());
		check(Collected && !CommandList.IsInsideRenderPass());
		FStaticMeshPreparationCache LocalCache;
		auto& Cache = SharedCache ? *SharedCache : LocalCache;
		auto Resolved = std::make_shared<FStaticMeshPreparationInputs>();
		Resolved->Collected = std::move(Collected);
		const auto& View = Resolved->Collected->View;
		const auto RasterMode = Resolved->Collected->RasterMode;
		const auto Mode = Resolved->Collected->Mode;
		Resolved->Primitives.reserve(Resolved->Collected->Primitives.size());
		for (const auto& Primitive : Resolved->Collected->Primitives)
		{
			auto& Batches = Resolved->Primitives.emplace_back();
			Batches.reserve(Primitive.Batches.size());
			const bool bSplineMesh = Primitive.bSplineMesh;
			for (const auto& Captured : Primitive.Batches)
			{
				auto& FrozenBatch = Batches.emplace_back();
				const auto& Batch = Captured.Batch;
				const auto& Binding = Captured.InputBinding;
				if (!Binding || !Binding->Declaration || Binding->Streams.empty() || !Captured.bFactoryCompatible) continue;
				FrozenBatch.Transform = Cache.ResolveTransform(Primitive.Id, Batch.BatchId, Batch.LocalToWorld);
				if (!FrozenBatch.Transform.bValid) continue;
				const double Determinant = FrozenBatch.Transform.Determinant;
				bool bReverseCulling = Determinant < 0.0;
				bool bSplineChangesOrientation = false;
				if (const auto* Spline = dynamic_cast<const FSplineMeshBatchBinding*>(Binding.get()))
				{
					const auto& Params = Spline->DynamicData.Params;
					// Scale interpolation is monotone. A zero or sign change cannot
					// be represented by one front-face orientation for the whole draw.
					for (size_t Axis = 0; Axis < 2; ++Axis)
					{
						const double Start = Params.StartScale[Axis];
						const double End = Params.EndScale[Axis];
						bSplineChangesOrientation |= Start == 0.0 || End == 0.0 || (Start < 0.0) != (End < 0.0);
						bReverseCulling ^= Start < 0.0;
					}
				}
				const uint64 Policy =
					static_cast<uint64>(RasterMode) | (static_cast<uint64>(View.DepthConvention) << 8) |
					(static_cast<uint64>(Mode) << 16) | (static_cast<uint64>(bReverseCulling) << 24) |
					(static_cast<uint64>(bSplineMesh) << 25) | (static_cast<uint64>(bSplineChangesOrientation) << 26);

				const auto StableSections = Captured.StaticFacts && Cache.Commands
												? Cache.Commands->FindStaticSections(Batch.StaticInputs, Policy)
												: nullptr;
				if (StableSections)
				{
					auto Found = Cache.ResolvedSections.find(StableSections.get());
					if (Found == Cache.ResolvedSections.end())
					{
						auto Elements =
							std::make_shared<std::vector<FResolvedStaticMeshSection>>(StableSections->Commands.size());
						for (size_t Index = 0; Index < Elements->size(); ++Index)
						{
							const auto& Command = StableSections->Commands[Index];
							if (!Command) continue;
							auto Material = Command->Material;

							const auto MaterialIndex = Cache.ResolveMaterial(Material);
							if (!MaterialIndex) continue;
							(*Elements)[Index] = {Command, *MaterialIndex, true};
						}
						Found =
							Cache.ResolvedSections
								.emplace(StableSections.get(),
									FStaticMeshPreparationCache::FResolvedSections{StableSections, std::move(Elements)})
								.first;
					}
					FrozenBatch.StaticElements = Found->second.Elements;
					continue;
				}
				bool bStableMaterials = true;
				FrozenBatch.Elements.resize(Batch.GetNumElements());
				for (size_t ElementIndex = 0; ElementIndex < Batch.GetNumElements(); ++ElementIndex)
				{
					const auto& Facts = Captured.GetElementFacts(ElementIndex);
					if (Facts.InputOutcome != EGeometrySubmissionOutcome::Submitted || !Facts.bSupportsPreparation) continue;
					const auto Element = Batch.GetElement(ElementIndex);
					const auto SectionIndex = Element.ElementId;
					auto& Item = FrozenBatch.Elements[ElementIndex];
					auto Material = Element.Material;
					{
						DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ResolveMeshMaterial");

						const auto MaterialIndex = Cache.ResolveMaterial(Material);
						if (!MaterialIndex)
						{
							bStableMaterials = false;
							continue;
						}
						Item.MaterialUniformIndex = *MaterialIndex;

					}
					bStableMaterials &=
						Material.Representation.GetRecordId() == Element.Material.Representation.GetRecordId() &&
						Material.CompiledProgram == Element.Material.CompiledProgram;

					const FMeshDrawDescriptionCache::FKey Key{
						Batch.GeometryRecord ? Batch.GeometryRecord->GetRecordId() : 0,
						ElementIndex, Material.Representation.GetRecordId(), Policy};
					if (Batch.GeometryRecord && Cache.Commands)
						Item.Command = Cache.Commands->Find(Key, Material);
					if (Item.Command) Item.bTemplateReused = true;
					else
					{
						auto Candidate = std::make_shared<FMeshDrawDescription>();
						Candidate->Material = Material;
						Candidate->GeometryOwner = Batch.GeometryRecord;
						Candidate->SectionIndex = SectionIndex;
						Candidate->Geometry = Element.Draw;
						Candidate->bSupportsGBuffer = Captured.GetElementFacts(ElementIndex).bSupportsGBuffer;
						Candidate->Vertices = Element.Vertices;
						Candidate->Indices = Element.Indices;
						Candidate->MaterialSlotDiagnostic = Element.MaterialSlotDiagnostic;
						Candidate->PipelineKey.Material = Candidate->Material.PlanningPassIdentity;
						Candidate->PipelineKey.VertexDomain = bSplineMesh ? EVertexDeformationDomain::Spline : EVertexDeformationDomain::Local;
						Candidate->PipelineKey.FactoryKey = Batch.FactoryKey;
						Candidate->PipelineKey.LayoutKey = Batch.LayoutKey;
						Candidate->PipelineKey.VertexDeclaration = Binding->Declaration;
						Candidate->PipelineKey.Topology = Element.Draw.Topology;
						const auto PipelineState = MakeMeshPipelineState(Candidate->Material.PlanningPassIdentity, {
							.bWireframe = RasterMode == ERasterMode::Wireframe,
							.bReverseCulling = bReverseCulling,
							.bOrientationChanging = bSplineChangesOrientation,
							.bReversedZ = View.DepthConvention == ESceneDepthConvention::ReversedZ});
						Candidate->PipelineKey.Rasterizer = PipelineState.Rasterizer;
						Candidate->PipelineKey.Depth = PipelineState.Depth;
						Candidate->PipelineKey.ColorBlend = PipelineState.ColorBlend;
						const auto BlendMode =
							Candidate->Material.PlanningPassIdentity.ShaderMap.BlendMode;
						Candidate->Pass = BlendMode == EMaterialBlendMode::Masked ? EMeshBasePass::Masked : BlendMode == EMaterialBlendMode::Translucent ? EMeshBasePass::Translucent :
																														   EMeshBasePass::Opaque;
						if (Mode == ERenderPreparationMode::ShadowDepth
							&& Candidate->Pass == EMeshBasePass::Translucent)
							continue;

						if (!std::isfinite(Candidate->PipelineKey.Material.ShaderMap.OpacityMaskThreshold)) continue;
						FPreparedStaticMeshPrimitive StablePrimitive;
						StablePrimitive.CollectedBinding = Binding;
						Candidate->SortKey = MakeStaticMeshDrawSortKey(StablePrimitive, *Candidate);
						Item.Command = std::move(Candidate);
						if (Batch.GeometryRecord && Cache.Commands) Cache.Commands->Store(Key, Item.Command);
					}
				}
				if (Captured.StaticFacts && Cache.Commands && bStableMaterials)
				{
					auto Sections = std::make_shared<FStaticMeshPassSections>();
					Sections->Facts = Captured.StaticFacts;
					Sections->Commands.reserve(FrozenBatch.Elements.size());
					for (const auto& Element : FrozenBatch.Elements)
						Sections->Commands.push_back(Element.Command);
					Cache.Commands->StoreStaticSections(Policy, std::move(Sections));
				}
			}
		}
		Resolved->MaterialCount = Cache.Materials.size();
		return Resolved;
	}

	auto PrepareCollectedStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::shared_ptr<const FCollectedStaticMeshView> Collected,
		FStaticMeshPreparationCache* SharedCache) -> FPreparedStaticMeshView
	{
		return FinishStaticMeshView(StartCollectedStaticMeshView_RenderThread(CommandList, std::move(Collected), SharedCache));
	}

	auto StartCollectedStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::shared_ptr<const FCollectedStaticMeshView> Collected,
		FStaticMeshPreparationCache* SharedCache) -> FStaticMeshPreparationWork
	{
		const auto Resolved = ResolveCollectedStaticMeshView_RenderThread(CommandList, std::move(Collected), SharedCache);
		const char* Setting = std::getenv("DURIN_MESH_PREPARATION");
		const auto Policy = Setting && std::string_view(Setting) == "inline" ? EStaticMeshPreparationPolicy::Inline
			: Setting && std::string_view(Setting) == "tasks" ? EStaticMeshPreparationPolicy::Tasks : EStaticMeshPreparationPolicy::Auto;
		return StartStaticMeshPreparation(Resolved, Policy);
	}

	auto FinishStaticMeshView(FStaticMeshPreparationWork Work) -> FPreparedStaticMeshView
	{

		auto Prepared = FinishStaticMeshPreparation(std::move(Work));
		if (Prepared) return std::move(*Prepared);
		DURIN_ERROR("Mesh preparation failed at chunk {} (state {}).", Prepared.error().ChunkIndex, static_cast<uint32>(Prepared.error().State));
		FPreparedStaticMeshView Failed;
		Failed.bResourceFailure = true;
		return Failed;
	}

	static auto PrepareStaticMeshInputRange(const FStaticMeshPreparationInputs& Resolved,
		size_t FirstPrimitive, size_t PrimitiveCount) -> FPreparedStaticMeshView
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareCollectedMeshes");

		check(Resolved.Collected);
		const auto& Collected = *Resolved.Collected;
		const auto& View = Collected.View;
		FPreparedStaticMeshView Result;
		Result.Primitives.reserve(PrimitiveCount);
		auto PreparePrimitive = [&](const FCollectedStaticMeshView::FPrimitive& Primitive) {
			DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareMeshPrimitive");
			if (!Primitive.bPresent)
			{
				++Result.RejectedPrimitives;
				return;
			}
			const bool bSplineMesh = Primitive.bSplineMesh;
			++Result.VisibleCandidates;
			if (bSplineMesh) ++Result.VisibleSplineCandidates;
			else ++Result.VisibleLocalCandidates;
			++Result.SharedPrimitiveFactBuilds;
			if (Primitive.bProjectedSizeFallback) ++Result.ProjectedSizeFallbacks;
			for (size_t I = 0; I < Result.SubmissionOutcomes.size(); ++I)
				Result.SubmissionOutcomes[I] += Primitive.Outcomes[I];
			Result.bResourceFailure |= Primitive.Outcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)] != 0;
			if (Primitive.Batches.empty())
			{
				++Result.RejectedPrimitives;
				return;
			}
			const size_t FirstPrimitiveBatch = Result.Primitives.size();
			auto PrepareBatch = [&](const FCollectedStaticMeshView::FBatch& Captured) {
				const auto& Batch = Captured.Batch;
				DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareMeshBatch");
				const auto& Binding = Captured.InputBinding;
				if (!Binding || !Binding->Declaration || Binding->Streams.empty())
				{
					++Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)];
					Result.bResourceFailure = true;
					return;
				}
				if (!Captured.bFactoryCompatible)
				{
					++Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Unsupported)];
					return;
				}
				const uint32 RequestedLODIndex = Batch.RequestedLOD;
				const uint32 SelectedLODIndex = Batch.SelectedLOD;
				if (SelectedLODIndex != RequestedLODIndex) ++Result.ResourceFallbacks;
				++Result.SelectedLODFactBuilds;
				const FMatrix& LocalToWorld = Batch.LocalToWorld;
				const auto& FrozenBatch = Resolved.Primitives[&Primitive - Collected.Primitives.data()]
					[&Captured - Primitive.Batches.data()];
				if (!FrozenBatch.Transform.bValid) return;
				const auto& NormalToWorld = FrozenBatch.Transform.NormalToWorld;
				const auto& WorldToLocal = FrozenBatch.Transform.WorldToLocal;
				FVector3 BoundsCenter;
				if (Batch.WorldBounds.bIsValid && Math::IsFinite(Batch.WorldBounds.Min)
					&& Math::IsFinite(Batch.WorldBounds.Max))
					BoundsCenter = Batch.WorldBounds.GetCenter();
				else
				{
					const FVector4 Origin = LocalToWorld * FVector4(0.0, 0.0, 0.0, 1.0);
					if (!Math::IsFinite(Origin)) return;
					BoundsCenter = FVector3(Origin);
				}
				const uint32 PrimitiveIndex =
					static_cast<uint32>(Result.Primitives.size());
				Result.Primitives.push_back({.CustomPrimitiveData = Batch.CustomPrimitiveData,
					.CustomInstanceData = Batch.CustomInstanceData,
					.PrimitiveId = Primitive.Id,
					.BatchId = Batch.BatchId,
					.RequestedLODIndex = RequestedLODIndex,
					.SelectedLODIndex = SelectedLODIndex,
					.VertexDomain = bSplineMesh ? EVertexDeformationDomain::Spline : EVertexDeformationDomain::Local,
					.CollectedBinding = Binding,
					.GeometryRecord = Batch.GeometryRecord,
					.WorldBounds = Batch.WorldBounds,
					.LocalToWorld = LocalToWorld,
					.NormalToWorld = NormalToWorld,
					.WorldToLocal = WorldToLocal,
					.BoundsCenter = BoundsCenter,
					.bRetainedStaticMesh = Primitive.bRetainedStaticMesh});
				const size_t FirstSectionCount = Result.GetNumSections();
				const size_t FirstTriangleCount = Result.SelectedTriangles;

				for (size_t ElementIndex = 0; ElementIndex < Batch.GetNumElements(); ++ElementIndex)
				{
					const auto Element = Batch.GetElement(ElementIndex);
					if (!Batch.GeometryRecord)
					{
						++Result.DynamicGeometryInputValidations;
						const auto InputOutcome = Captured.GetElementFacts(ElementIndex).InputOutcome;
						if (InputOutcome != EGeometrySubmissionOutcome::Submitted)
						{
							++Result.SubmissionOutcomes[static_cast<size_t>(InputOutcome)];
							Result.bResourceFailure |= InputOutcome == EGeometrySubmissionOutcome::ResourceFailure;
							continue;
						}
					}
					else ++Result.PublishedGeometryElements;
					if (!Captured.GetElementFacts(ElementIndex).bSupportsPreparation)
					{
						++Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Unsupported)];
						continue;
					}
					const size_t TriangleCount = Element.Draw.Topology == EGeometryTopology::TriangleList ? Element.Draw.ElementCount / 3 : 0;
					const uint64 SectionIndex = Element.ElementId;
					++Result.SharedSectionFactBuilds;
					FPreparedStaticMeshDraw Item;
					const auto& FrozenElement = FrozenBatch.GetElement(ElementIndex);
					if (!FrozenElement.Command) continue;
					Item.Command = FrozenElement.Command;
					Item.MaterialUniformIndex = FrozenElement.MaterialUniformIndex;
					if (FrozenElement.bTemplateReused) ++Result.DrawDescriptionReuses;
					else ++Result.DrawDescriptionBuilds;
					Item.SortKey.State = {Item.Command, &Item.Command->SortKey};
					Item.PrimitiveIndex = PrimitiveIndex;
					Item.SortKey.PrimitiveId = Primitive.Id.Value;
					Item.SortKey.BatchId = Batch.BatchId;
					Item.SortKey.SelectedLODIndex = SelectedLODIndex;

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

					switch (Item.Command->Pass)
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
			for (const auto& Batch : Primitive.Batches) PrepareBatch(Batch);
			if (Result.Primitives.size() == FirstPrimitiveBatch) ++Result.RejectedPrimitives;
			else if (bSplineMesh) ++Result.PreparedSplinePrimitives;
			else ++Result.PreparedLocalPrimitives;
		};
		std::ranges::for_each(std::span(Collected.Primitives).subspan(FirstPrimitive, PrimitiveCount), PreparePrimitive);
		return Result;
	}

	static auto FinalizeStaticMeshPreparation(FPreparedStaticMeshView& Result, size_t MaterialCount) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.FinalizeMeshPreparation");

		Result.RejectedSplinePrimitives = Result.VisibleSplineCandidates
										  - std::min(Result.VisibleSplineCandidates, Result.PreparedSplinePrimitives);
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.SortMeshDraws");
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
						Bucket[Index - 1].SortKey.GetState();
					const FMeshDrawSortKey& Current = Bucket[Index].SortKey.GetState();
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
			// Build a separate group schedule without disturbing translucent draw order.
			std::vector<uint32> UniformGroups(MaterialCount, UINT32_MAX);
			std::unordered_multimap<uint32, uint32> AdditionalCollectionGroups;
			auto Collections = [](const FPreparedStaticMeshDraw& Draw) -> std::span<const FMaterialParameterCollectionLayout> {
				const auto& Program = Draw.Command->Material.CompiledProgram;
				return Program ? std::span<const FMaterialParameterCollectionLayout>(Program->ActiveCollections)
					: std::span<const FMaterialParameterCollectionLayout>{};
			};
			for (auto* Bucket : {&Result.Opaque, &Result.Masked, &Result.Translucent})
				for (auto& Draw : *Bucket)
				{
					const auto MaterialIndex = Draw.MaterialUniformIndex;
					auto& FirstGroup = UniformGroups[MaterialIndex];
					uint32 Group = FirstGroup;
					auto Matches = [&](uint32 Candidate) {
						return std::ranges::equal(Collections(Draw),
							Collections(Result.GetDraw(Result.MaterialUniformGroups[Candidate].RepresentativeDraw)));
					};
					// Equal local bytes do not imply equal Collection bindings. Keep the
					// common collection-free path allocation-free and split only collisions.
					if (Group != UINT32_MAX && !Matches(Group))
					{
						Group = UINT32_MAX;
						const auto [Begin, End] = AdditionalCollectionGroups.equal_range(MaterialIndex);
						for (auto It = Begin; It != End; ++It)
							if (Matches(It->second)) { Group = It->second; break; }
					}
					if (Group == UINT32_MAX)
					{
						Group = static_cast<uint32>(Result.MaterialUniformGroups.size());
						Result.MaterialUniformGroups.push_back({Draw.ResolvedIndex});
						if (FirstGroup == UINT32_MAX) FirstGroup = Group;
						else AdditionalCollectionGroups.emplace(MaterialIndex, Group);
					}
					Draw.MaterialUniformIndex = Group;
				}
			Result.SortingNanoseconds = static_cast<uint64>(std::chrono::duration_cast<
																std::chrono::nanoseconds>(
																std::chrono::steady_clock::now() - SortingStart
			)
																.count());

		}
		auto CountStateFacts = [&Result](const auto& Bucket) -> size_t {
			if (Bucket.empty())
			{
				return 0;
			}
			size_t StateGroups = 1;
			for (size_t Index = 1; Index < Bucket.size(); ++Index)
			{
				const FMeshDrawSortKey& Previous = Bucket[Index - 1].SortKey.GetState();
				const FMeshDrawSortKey& Current = Bucket[Index].SortKey.GetState();
				const bool bPipelineChanged =
					Previous.Pipeline != Current.Pipeline;
				const bool bMaterialChanged =
					Previous.MaterialUniform != Current.MaterialUniform;
				const bool bVertexFactoryChanged =
					Previous.VertexFactory != Current.VertexFactory;
				const bool bGeometryChanged =
					Previous.Geometry != Current.Geometry
					|| Bucket[Index - 1].SortKey.PrimitiveId != Bucket[Index].SortKey.PrimitiveId
					|| Bucket[Index - 1].SortKey.BatchId != Bucket[Index].SortKey.BatchId
					|| Bucket[Index - 1].SortKey.SelectedLODIndex != Bucket[Index].SortKey.SelectedLODIndex;
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
		DURIN_PROFILE_CPU_ZONE_TEXT(std::format("prepared_batches={} draws={} opaque={} masked={} translucent={}", Result.Primitives.size(), Result.GetNumSections(), Result.Opaque.size(), Result.Masked.size(), Result.Translucent.size()));
	}
	auto PrepareStaticMeshInputs(const FStaticMeshPreparationInputs& Inputs) -> FPreparedStaticMeshView
	{
		check(Inputs.Collected);
		auto Result = PrepareStaticMeshInputRange(Inputs, 0, Inputs.Collected->Primitives.size());
		FinalizeStaticMeshPreparation(Result, Inputs.MaterialCount);
		return Result;
	}

	auto PrepareStaticMeshInputChunk(std::shared_ptr<const FStaticMeshPreparationInputs> Inputs,
		size_t FirstPrimitive, size_t PrimitiveCount) -> FStaticMeshPreparationChunk
	{
		check(Inputs && Inputs->Collected);
		check(FirstPrimitive <= Inputs->Collected->Primitives.size()
			&& PrimitiveCount <= Inputs->Collected->Primitives.size() - FirstPrimitive);
		FStaticMeshPreparationChunk Chunk{std::move(Inputs), FirstPrimitive, PrimitiveCount};
		Chunk.Output = PrepareStaticMeshInputRange(*Chunk.Inputs, FirstPrimitive, PrimitiveCount);
		return Chunk;
	}

	auto MergeStaticMeshPreparationChunks(std::vector<FStaticMeshPreparationChunk> Chunks)
		-> std::expected<FPreparedStaticMeshView, EStaticMeshPreparationMergeError>
	{
		using EError = EStaticMeshPreparationMergeError;
		if (Chunks.empty()) return std::unexpected(EError::EmptyChunks);
		const auto Inputs = Chunks.front().Inputs;
		if (!Inputs || !Inputs->Collected) return std::unexpected(EError::MismatchedInputs);
		const size_t Total = Inputs->Collected->Primitives.size();
		for (const auto& Chunk : Chunks)
		{
			if (Chunk.Inputs != Inputs) return std::unexpected(EError::MismatchedInputs);
			if (Chunk.FirstPrimitive > Total || Chunk.PrimitiveCount > Total - Chunk.FirstPrimitive
				|| (Chunk.PrimitiveCount == 0 && (Total != 0 || Chunks.size() != 1)))
				return std::unexpected(EError::InvalidRange);
		}
		std::ranges::sort(Chunks, {}, &FStaticMeshPreparationChunk::FirstPrimitive);
		size_t Next = 0;
		size_t OutputPrimitives = 0;
		std::array<size_t, 3> DrawCounts{};
		for (const auto& Chunk : Chunks)
		{
			if (Chunk.FirstPrimitive != Next) return std::unexpected(EError::IncompleteCoverage);
			Next += Chunk.PrimitiveCount;
			if (Chunk.Output.Primitives.size() > UINT32_MAX - OutputPrimitives)
				return std::unexpected(EError::TooManyPrimitives);
			OutputPrimitives += Chunk.Output.Primitives.size();
			DrawCounts[0] += Chunk.Output.Opaque.size();
			DrawCounts[1] += Chunk.Output.Masked.size();
			DrawCounts[2] += Chunk.Output.Translucent.size();
		}
		if (Next != Total) return std::unexpected(EError::IncompleteCoverage);
		FPreparedStaticMeshView Result;
		Result.Primitives.reserve(OutputPrimitives);
		Result.Opaque.reserve(DrawCounts[0]);
		Result.Masked.reserve(DrawCounts[1]);
		Result.Translucent.reserve(DrawCounts[2]);
		auto AppendDraws = [](auto& Target, auto& Source, uint32 PrimitiveBase) {
			for (auto& Draw : Source)
			{
				Draw.PrimitiveIndex += PrimitiveBase;
				Target.push_back(std::move(Draw));
			}
		};
		auto AddHistogram = [](auto& Target, const auto& Source) {
			Target.resize(std::max(Target.size(), Source.size()));
			for (size_t Index = 0; Index < Source.size(); ++Index) Target[Index] += Source[Index];
		};
		for (auto& Chunk : Chunks)
		{
			const auto PrimitiveBase = static_cast<uint32>(Result.Primitives.size());
			Result.Primitives.insert(Result.Primitives.end(), std::make_move_iterator(Chunk.Output.Primitives.begin()),
				std::make_move_iterator(Chunk.Output.Primitives.end()));
			AppendDraws(Result.Opaque, Chunk.Output.Opaque, PrimitiveBase);
			AppendDraws(Result.Masked, Chunk.Output.Masked, PrimitiveBase);
			AppendDraws(Result.Translucent, Chunk.Output.Translucent, PrimitiveBase);
			AddHistogram(Result.RequestedLODHistogram, Chunk.Output.RequestedLODHistogram);
			AddHistogram(Result.SelectedLODHistogram, Chunk.Output.SelectedLODHistogram);
			for (size_t Index = 0; Index < Result.SubmissionOutcomes.size(); ++Index)
				Result.SubmissionOutcomes[Index] += Chunk.Output.SubmissionOutcomes[Index];
			Result.bResourceFailure |= Chunk.Output.bResourceFailure;
			Result.VisibleCandidates += Chunk.Output.VisibleCandidates;
			Result.VisibleLocalCandidates += Chunk.Output.VisibleLocalCandidates;
			Result.VisibleSplineCandidates += Chunk.Output.VisibleSplineCandidates;
			Result.PreparedLocalPrimitives += Chunk.Output.PreparedLocalPrimitives;
			Result.PreparedSplinePrimitives += Chunk.Output.PreparedSplinePrimitives;
			Result.PreparedSplineSections += Chunk.Output.PreparedSplineSections;
			Result.PreparedSplineTriangles += Chunk.Output.PreparedSplineTriangles;
			Result.RetainedSplineDeformationBytes += Chunk.Output.RetainedSplineDeformationBytes;
			Result.AcceptedSplineDynamicUpdates += Chunk.Output.AcceptedSplineDynamicUpdates;
			Result.RejectedPrimitives += Chunk.Output.RejectedPrimitives;
			Result.ProjectedSizeFallbacks += Chunk.Output.ProjectedSizeFallbacks;
			Result.ResourceFallbacks += Chunk.Output.ResourceFallbacks;
			Result.SelectedSections += Chunk.Output.SelectedSections;
			Result.SelectedTriangles += Chunk.Output.SelectedTriangles;
			Result.OpaqueSections += Chunk.Output.OpaqueSections;
			Result.MaskedSections += Chunk.Output.MaskedSections;
			Result.TranslucentSections += Chunk.Output.TranslucentSections;
			Result.OpaqueTriangles += Chunk.Output.OpaqueTriangles;
			Result.MaskedTriangles += Chunk.Output.MaskedTriangles;
			Result.TranslucentTriangles += Chunk.Output.TranslucentTriangles;
			Result.SharedPrimitiveFactBuilds += Chunk.Output.SharedPrimitiveFactBuilds;
			Result.SelectedLODFactBuilds += Chunk.Output.SelectedLODFactBuilds;
			Result.SharedSectionFactBuilds += Chunk.Output.SharedSectionFactBuilds;
			Result.DynamicGeometryInputValidations += Chunk.Output.DynamicGeometryInputValidations;
			Result.PublishedGeometryElements += Chunk.Output.PublishedGeometryElements;
			Result.DrawDescriptionBuilds += Chunk.Output.DrawDescriptionBuilds;
			Result.DrawDescriptionReuses += Chunk.Output.DrawDescriptionReuses;
		}
		FinalizeStaticMeshPreparation(Result, Inputs->MaterialCount);
		return Result;
	}
	namespace
	{
		constexpr size_t MeshPreparationChunkSize = 128;
		constexpr size_t MaxActiveMeshPreparationTasks = 8;
		auto LaunchNextMeshPreparationChunk(FStaticMeshPreparationWork& Work) -> void
		{
			const size_t First = Work.NextPrimitive;
			const size_t Count = std::min(MeshPreparationChunkSize, Work.Inputs->Collected->Primitives.size() - First);
			Tasks::FTaskExecutionOptions Options;
			Options.Cancellation = Work.Cancellation;
			auto Task = Tasks::LaunchIndependentTask("Renderer.PrepareMeshChunk",
				[Inputs = Work.Inputs, First, Count] { return PrepareStaticMeshInputChunk(Inputs, First, Count); }, Options);
			Work.Active.push_back({Work.LaunchedChunks++, std::move(Task)});
			Work.NextPrimitive += Count;
		}
	}

	FStaticMeshPreparationWork::~FStaticMeshPreparationWork()
	{
		for (auto& Slot : Active) Tasks::Cancel(Slot.Task);
		for (auto& Slot : Active)
			require(Slot.Task.Wait().WaitStatus == ETaskWaitStatus::Completed);
	}

	auto StartStaticMeshPreparation(std::shared_ptr<const FStaticMeshPreparationInputs> Inputs,
		EStaticMeshPreparationPolicy Policy, FTaskCancellationToken Cancellation) -> FStaticMeshPreparationWork
	{
		check(Inputs && Inputs->Collected);
		FStaticMeshPreparationWork Work;
		Work.Inputs = std::move(Inputs);
		Work.Cancellation = std::move(Cancellation);
		Work.bCanceled = Work.Cancellation.IsCancellationRequested();
		if (Work.bCanceled) return Work;
		const size_t Count = Work.Inputs->Collected->Primitives.size();
		if (Policy == EStaticMeshPreparationPolicy::Inline || !IsTaskSchedulerRunning()
			|| Count == 0 || (Policy == EStaticMeshPreparationPolicy::Auto && Count < 256))
		{
			Work.InlineOutput = PrepareStaticMeshInputs(*Work.Inputs);
			Work.bCanceled = Work.Cancellation.IsCancellationRequested();
			return Work;
		}
		while (Work.NextPrimitive < Count && Work.Active.size() < MaxActiveMeshPreparationTasks)
			LaunchNextMeshPreparationChunk(Work);
		return Work;
	}

	auto FinishStaticMeshPreparation(FStaticMeshPreparationWork Work)
		-> std::expected<FPreparedStaticMeshView, FStaticMeshPreparationError>
	{
		if (Work.bCanceled) return std::unexpected(FStaticMeshPreparationError{0, ETaskState::Canceled});
		if (Work.InlineOutput) return std::move(*Work.InlineOutput);
		const auto JoinStart = std::chrono::steady_clock::now();
		std::vector<FStaticMeshPreparationChunk> Chunks;
		std::optional<FStaticMeshPreparationError> Failure;
		while (!Work.Active.empty())
		{
			auto Slot = std::move(Work.Active.front());
			Work.Active.pop_front();
			const auto Wait = Slot.Task.Wait();
			require(Wait.WaitStatus == ETaskWaitStatus::Completed);
			if (!Failure && Wait.TaskState != ETaskState::Succeeded)
			{
				Failure = FStaticMeshPreparationError{Slot.Index, Wait.TaskState};
				for (auto& Pending : Work.Active) Tasks::Cancel(Pending.Task);
			}
			if (!Failure)
			{
				Chunks.push_back(std::move(Slot.Task.GetResult()));
				if (Work.NextPrimitive < Work.Inputs->Collected->Primitives.size()) LaunchNextMeshPreparationChunk(Work);
			}
		}
		if (Failure) return std::unexpected(*Failure);
		auto Merged = MergeStaticMeshPreparationChunks(std::move(Chunks));
		if (!Merged) return std::unexpected(FStaticMeshPreparationError{0, ETaskState::Failed, Merged.error()});
		Merged->PreparationTaskCount = Work.LaunchedChunks;
		Merged->PreparationJoinNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - JoinStart).count());
		return std::move(*Merged);
	}
} // namespace Durin
