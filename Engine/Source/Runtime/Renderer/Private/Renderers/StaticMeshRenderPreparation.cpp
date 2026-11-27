#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/RenderPreparationTasks.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Rendering/PrimitiveSceneProxy.h"
#include "Renderers/MeshRendererShared.h"
#include "SceneInfo.h"
#include "Rendering/MeshBatch.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "Renderers/MeshVertexFactory.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "Math/Operations.h"
#include "VertexFactory.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	using namespace RendererPrivate;
	static constexpr size_t MinParallelViewLODPrimitives = 256;

	auto PrepareMeshViewFacts(std::vector<FMeshViewPreparationInput> Inputs,
		const FSceneView& View, bool bAllowTasks) -> FMeshViewPreparationFacts
	{
		if (Inputs.empty()) return {};
		// Capture only the fields used by projected-size calculation and LOD policy.
		FSceneView FrozenView;
		FrozenView.ViewProjectionMatrix = View.ViewProjectionMatrix;
		FrozenView.ViewMatrix = View.ViewMatrix;
		FrozenView.ProjectionMatrix = View.ProjectionMatrix;
		FrozenView.ViewportWidth = View.ViewportWidth;
		FrozenView.ViewportHeight = View.ViewportHeight;
		FrozenView.Settings.Mode = View.Settings.Mode;
		FrozenView.DepthConvention = View.DepthConvention;
		const FBoxScreenSizeProjection Projection(FrozenView);
		FMeshViewPreparationFacts Result;
		Result.Primitives.resize(Inputs.size());
		const auto BuildFact = [&](size_t Index) {
			const auto& Input = Inputs[Index];
			auto& Fact = Result.Primitives[Index];
			Fact.Projected = Projection.Project(Input.WorldBounds);
			if (Input.StaticBatches || Input.LODs)
			{
				const auto LODs = Input.StaticBatches ? std::span<const FMeshLODSelectionEntry>(Input.StaticBatches->LODs)
					: std::span<const FMeshLODSelectionEntry>(*Input.LODs);
				const uint32 Requested = FrozenView.Settings.Mode.LODMode == EViewLODMode::ForceLOD0 ? 0u
					: SelectStaticMeshLOD(Fact.Projected.NormalizedScreenSize, LODs);
				Fact.LOD = FPreparedMeshLODSelection{Requested, ResolveAvailableStaticMeshLOD(Requested, LODs)};
			}
		};
		Result.TaskCount = RunPreparationRanges("Renderer.PrepareViewLOD", Inputs.size(), 128,
			bAllowTasks && Inputs.size() >= MinParallelViewLODPrimitives, BuildFact);
		return Result;
	}

	namespace
	{
		auto MaterialFactHash(const FMaterialRenderRepresentation& Representation) -> uint64
		{
			return Representation.GetContentHash();
		}
	}

	namespace
	{
		auto DescriptionBytes(const FMeshDrawDescription& Value) -> size_t
		{ return sizeof(Value) + Value.SortKey.VertexFactory.capacity() * sizeof(uint32); }

		auto FactsBytes(const FStaticMeshSectionFacts& Value) -> size_t
		{ return sizeof(Value) + Value.Elements.capacity() * sizeof(FStaticMeshSectionFact); }

		auto SectionsBytes(const FStaticMeshPassSections& Value) -> size_t
		{ return sizeof(Value) + Value.Commands.capacity() * sizeof(decltype(Value.Commands)::value_type); }
	}

	auto FMeshDrawDescriptionCache::BeginSubmission(uint64 InFrame) -> void
	{
		if (InFrame < Frame) Reset();
		Frame = InFrame;
	}

	auto FMeshDrawDescriptionCache::EndSubmission() -> void
	{
		if (Frame - LastAgeSweep < MaxIdleFrames) return;
		EvictIdle(MaxIdleFrames);
		LastAgeSweep = Frame;
	}

	auto FMeshDrawDescriptionCache::EvictIdle(uint64 IdleFrames) -> void
	{
		const auto Trim = [&](auto& Table) {
			Evictions += std::erase_if(Table, [&](const auto& Pair) {
				if (Frame - Pair.second.LastAccessFrame < IdleFrames) return false;
				BudgetedBytes -= Pair.second.Charge;
				return true;
			});
		};
		Trim(Entries);
		Trim(StaticSections);
		Trim(StaticFacts);
	}

	template<typename TTable, typename T>
	auto FMeshDrawDescriptionCache::StoreEntry(TTable& Table, const typename TTable::key_type& Key,
		std::shared_ptr<const T> Value, size_t Weight) -> void
	{
		if (const auto Existing = Table.find(Key); Existing != Table.end())
		{
			BudgetedBytes -= Existing->second.Charge;
			Table.erase(Existing);
		}
		if (Weight > MaxBytes - sizeof(typename TTable::value_type)) return;
		const size_t Charge = Weight + sizeof(typename TTable::value_type);
		const auto HasRoom = [&] {
			return Entries.size() + StaticFacts.size() + StaticSections.size() < MaxEntries
				&& Charge <= MaxBytes - BudgetedBytes;
		};
		if (!HasRoom() && LastPressureSweep != Frame)
		{
			// Preserve the two most recent frame working sets. Oversize views can
			// still prepare draws without admitting every result to the cache.
			EvictIdle(2);
			LastPressureSweep = Frame;
		}
		if (!HasRoom()) return;
		Table.emplace(Key, typename TTable::mapped_type{std::move(Value), Frame, Charge});
		BudgetedBytes += Charge;
	}

	auto FMeshDrawDescriptionCache::Reset() -> void
	{
		Entries.clear();
		StaticFacts.clear();
		StaticSections.clear();
		BudgetedBytes = 0;
		LastAgeSweep = Frame = 0;
		LastPressureSweep.reset();
	}

	auto FMeshDrawDescriptionCache::GetAccountedBytes() const -> size_t
	{
		size_t Bytes = Entries.size() * sizeof(decltype(Entries)::value_type)
			+ StaticFacts.size() * sizeof(decltype(StaticFacts)::value_type)
			+ StaticSections.size() * sizeof(decltype(StaticSections)::value_type);
		std::unordered_set<const FMeshDrawDescription*> Descriptions;
		std::unordered_set<const FStaticMeshSectionFacts*> Facts;
		std::unordered_set<const FStaticMeshPassSections*> Sections;
		const auto CountDescription = [&](const FMeshDrawDescription* Description) {
			if (Description && Descriptions.insert(Description).second)
				Bytes += DescriptionBytes(*Description);
		};
		const auto CountFacts = [&](const FStaticMeshSectionFacts* Value) {
			if (Value && Facts.insert(Value).second)
				Bytes += FactsBytes(*Value);
		};
		for (const auto& [Key, Entry] : Entries) CountDescription(Entry.Value.get());
		for (const auto& [Key, Entry] : StaticFacts) CountFacts(Entry.Value.get());
		for (const auto& [Key, Entry] : StaticSections)
		{
			const auto& Value = *Entry.Value;
			if (!Sections.insert(&Value).second) continue;
			Bytes += SectionsBytes(Value);
			CountFacts(Value.Facts.get());
			for (const auto& Command : Value.Commands) CountDescription(Command.get());
		}
		return Bytes;
	}

	auto FMeshDrawDescriptionCache::Find(const FKey& Key, const FMaterialRenderData& Material)
		-> std::shared_ptr<const FMeshDrawDescription>
	{
		const auto It = Entries.find(Key);
		if (It == Entries.end() || It->second.Value->Material.PlanningPassIdentity != Material.PlanningPassIdentity
			|| It->second.Value->Material.CompiledProgram != Material.CompiledProgram
			|| It->second.Value->Material.UniformExpressionCache != Material.UniformExpressionCache) return {};
		It->second.LastAccessFrame = Frame;
		return It->second.Value;
	}

	auto FMeshDrawDescriptionCache::Store(const FKey& Key, std::shared_ptr<const FMeshDrawDescription> Command) -> void
	{
		const size_t Weight = DescriptionBytes(*Command);
		StoreEntry(Entries, Key, std::move(Command), Weight);
	}

	auto FMeshDrawDescriptionCache::CaptureStaticFacts(const std::shared_ptr<const FStaticMeshBatchInputs>& Inputs,
		bool bShadow) -> std::shared_ptr<const FStaticMeshSectionFacts>
	{
		check(IsInRenderingThread());
		if (!Inputs || !Inputs->Geometry) return {};
		const FStaticKey Key{reinterpret_cast<uintptr_t>(Inputs.get()), bShadow ? 1u : 0u};
		if (const auto Found = StaticFacts.find(Key); Found != StaticFacts.end())
		{
			Found->second.LastAccessFrame = Frame;
			return Found->second.Value;
		}
		auto Binding = std::dynamic_pointer_cast<const FVertexFactoryInputBinding>(Inputs->Geometry->GetBinding());
		if (!Binding || !Binding->Declaration || Binding->Streams.empty()) return {};
		// Only the ordinary local factory has an audited view-independent Section contract.
		// Custom factories retain per-view preparation until they opt into an equivalent contract.
		const auto* BindingObject = Binding.get();
		if (typeid(*BindingObject) != typeid(FStaticMeshBatchBinding)) return {};
		auto Factory = FindMeshVertexFactory(Binding->GetFactoryKey());
		// Missing registrations may arrive later; never retain a missing-factory result.
		if (!Factory) return {};
		auto Facts = std::make_shared<FStaticMeshSectionFacts>();
		Facts->Inputs = Inputs;
		Facts->Binding = std::move(Binding);
		Facts->Factory = std::move(Factory);
		Facts->bFactoryCompatible = Facts->Factory->GetLayoutKey() == Facts->Binding->GetLayoutKey();
		Facts->Elements.resize(1);
		if (Facts->bFactoryCompatible)
		{
			auto& Element = Facts->Elements.front();
			const auto& Draw = Inputs->Geometry->GetElements()[Inputs->SegmentIndex].Draw;
			Element.bSupportsPreparation =
				Facts->Factory->Supports(FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, bShadow ? EMeshPipelinePass::ShadowDepth : EMeshPipelinePass::Forward).GetVertexFactoryPass(), Draw);
			if (!bShadow && Element.bSupportsPreparation)
				Element.bSupportsGBuffer = Facts->Factory->Supports(FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, EMeshPipelinePass::GBuffer).GetVertexFactoryPass(), Draw);
		}
		StoreEntry(StaticFacts, Key, std::shared_ptr<const FStaticMeshSectionFacts>(Facts), FactsBytes(*Facts));
		return Facts;
	}
	auto FMeshDrawDescriptionCache::FindStaticSections(const std::shared_ptr<const FStaticMeshBatchInputs>& Inputs,
		uint64 Policy) -> std::shared_ptr<const FStaticMeshPassSections>
	{
		check(IsInRenderingThread());
		const auto Found = StaticSections.find(FStaticKey{reinterpret_cast<uintptr_t>(Inputs.get()), Policy});
		if (Found == StaticSections.end()) return {};
		Found->second.LastAccessFrame = Frame;
		return Found->second.Value;
	}
	auto FMeshDrawDescriptionCache::StoreStaticSections(
		uint64 Policy, std::shared_ptr<const FStaticMeshPassSections> Sections) -> void
	{
		check(IsInRenderingThread());
		const FStaticKey Key{reinterpret_cast<uintptr_t>(Sections->Facts->Inputs.get()), Policy};
		// A section owns its facts and descriptions even when other table rows
		// are evicted. Charge the full graph conservatively on admission.
		size_t Weight = SectionsBytes(*Sections) + FactsBytes(*Sections->Facts);
		for (const auto& Command : Sections->Commands)
		{
			if (Command) Weight += DescriptionBytes(*Command);
			if (Weight > MaxBytes) return;
		}
		StoreEntry(StaticSections, Key, std::move(Sections), Weight);
	}

	auto FStaticMeshPreparationCache::ResolveMaterial(FMaterialRenderData& Material) -> std::optional<uint32>
	{
		const auto& Representation = Material.Representation;
		const auto RecordId = std::make_pair(Representation.GetRecordId(), Material.UniformExpressionCache.get());
		if (const auto It = MaterialRecordIndices.find(RecordId); It != MaterialRecordIndices.end())
			return It->second;
		const uint64 Hash = MaterialFactHash(Representation);
		const auto [Begin, End] = MaterialIndices.equal_range(Hash);
		for (auto It = Begin; It != End; ++It)
		{
			const auto& Cached = Materials[It->second];
			// Hash collisions and equal uniforms with different textures/layouts are distinct.
			if (MaterialPublishers[It->second] == Material.UniformExpressionCache && Cached.IsError() == Representation.IsError()
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
		MaterialPublishers.push_back(Material.UniformExpressionCache);
		// Resolution may have selected ErrorMaterial; hash its final representation.
		MaterialIndices.emplace(MaterialFactHash(Material.Representation), Index);
		MaterialRecordIndices.emplace(std::make_pair(Material.Representation.GetRecordId(), Material.UniformExpressionCache.get()), Index);
		return Index;
	}

	static auto GetAdmittedStaticBatch(FStaticMeshPreparationCache::FStaticCollection& Collection,
		FStaticMeshPreparationCache::FStaticPrimitive& Primitive,
		const FPrimitiveSceneInfo& SceneInfo, size_t MeshIndex, EMeshCollectionPurpose Purpose,
		FMeshBatchValidationCache* Validation) -> std::expected<FStaticMeshPreparationCache::FAdmittedBatch, EGeometrySubmissionOutcome>
	{
		const auto& StaticMesh = Primitive.Publication->Meshes[MeshIndex];
		if (Purpose == EMeshCollectionPurpose::Shadow ? !StaticMesh.CastShadow : !StaticMesh.bUseForMaterial)
			return std::unexpected(EGeometrySubmissionOutcome::Excluded);
		if (Primitive.AdmittedBatchIndices.empty())
			Primitive.AdmittedBatchIndices.resize(Primitive.Publication->Meshes.size(), SIZE_MAX);
		auto& AdmittedIndex = Primitive.AdmittedBatchIndices[MeshIndex];
		if (AdmittedIndex != SIZE_MAX)
			return FStaticMeshPreparationCache::FAdmittedBatch{Collection.Batches[AdmittedIndex].Snapshot, AdmittedIndex};
		FMeshBatch Batch = StaticMesh.CaptureMeshBatch(SceneInfo, Collection.Capture);
		const FMeshBatchCollector Collector(Purpose, Validation);
		const auto& Transform = SceneInfo.GetProxy().GetPrimitiveSceneTransform();
		const auto Outcome = Collector.Validate(Batch, &Transform);
		if (Outcome != EGeometrySubmissionOutcome::Submitted) return std::unexpected(Outcome);
		const auto Index = Collection.Batches.size();
		auto& Entry = Collection.Batches.emplace_back();
		Entry.Snapshot = std::make_shared<const FStaticMeshPreparationCache::FStaticBatchSnapshot>(std::move(Batch), Transform);
		AdmittedIndex = Index;
		return FStaticMeshPreparationCache::FAdmittedBatch{Entry.Snapshot, Index};
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
		Collected->View.ViewProjectionMatrix = View.ViewProjectionMatrix;
		Collected->View.ViewportWidth = View.ViewportWidth;
		Collected->View.ViewportHeight = View.ViewportHeight;
		Collected->View.ViewLocation = View.ViewLocation;
		Collected->View.DepthConvention = View.DepthConvention;
		Collected->View.FeatureLevel = View.FeatureLevel;
		Collected->View.Settings.Mode = View.Settings.Mode;
		Collected->RasterMode = RasterMode;
		Collected->Mode = Mode;
		const auto Purpose = Mode == ERenderPreparationMode::ShadowDepth
			? EMeshCollectionPurpose::Shadow : EMeshCollectionPurpose::Receiver;
		Collected->Primitives.reserve(SceneInfos.size());
		FStaticMeshPreparationCache::FStaticCollection LocalCollection;
		auto& Collection = SharedCache ? SharedCache->StaticCollection : LocalCollection;
		struct FCollectionInput
		{
			FStaticMeshPreparationCache::FStaticPrimitive* Static = nullptr;
			std::optional<size_t> ViewFactIndex;
		};
		std::vector<FCollectionInput> CollectionInputs(SceneInfos.size());
		std::vector<FMeshViewPreparationInput> ViewInputs;
		const bool bCanOverlapProjection = SceneInfos.size() >= MinParallelViewLODPrimitives && IsTaskSchedulerRunning();
		if (!bCanOverlapProjection) ViewInputs.reserve(SceneInfos.size());
		for (size_t Index = 0; Index < SceneInfos.size(); ++Index)
		{
			const auto* SceneInfo = SceneInfos[Index];
			if (!SceneInfo) continue;
			const auto& Proxy = SceneInfo->GetProxy();
			if (!Proxy.GetRenderParticipation().IsRelevant(Purpose)) continue;
			auto& Input = CollectionInputs[Index];
			auto [It, Inserted] = Collection.Primitives.try_emplace(SceneInfo);
			auto& Primitive = It->second;
			if (Inserted) Primitive.Publication = SceneInfo->GetStaticMeshes();
			if (Primitive.Publication)
			{
				Input.Static = &Primitive;
				// Only the fallback statistic needs projection when admission already knows the requested LOD.
				if (bCanOverlapProjection && (Primitive.Publication->LODs.size() <= 1
					|| View.Settings.Mode.LODMode == EViewLODMode::ForceLOD0))
					continue;
			}
			Input.ViewFactIndex = ViewInputs.size();
			auto& ViewInput = ViewInputs.emplace_back();
			ViewInput.WorldBounds = SceneInfo->GetWorldBounds();
			if (Input.Static) ViewInput.StaticBatches = Input.Static->Publication;
			else ViewInput.LODs = Proxy.CaptureLODSelection_RenderThread();
		}
		const auto ViewFacts = PrepareMeshViewFacts(std::move(ViewInputs), View);

		using FCapturedBatch = FCollectedStaticMeshView::FBatch;
		struct FBatchValidation
		{
			FCapturedBatch* Captured;
			std::shared_ptr<const FMeshVertexFactoryImplementation> Factory;
		};
		std::vector<FBatchValidation> SnapshotValidation;
		const auto PreparationPass = FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, Mode == ERenderPreparationMode::ShadowDepth
			? EMeshPipelinePass::ShadowDepth : EMeshPipelinePass::Forward).GetVertexFactoryPass();
		const auto GBufferPass = FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, EMeshPipelinePass::GBuffer).GetVertexFactoryPass();
		const auto ResolveFactorySupport = [&](FCapturedBatch& Captured, const FMeshVertexFactoryImplementation& Factory) {
			for (size_t Index = 0; Index < Captured.Elements.size(); ++Index)
			{
				auto& Facts = Captured.Elements[Index];
				if (Facts.InputOutcome != EGeometrySubmissionOutcome::Submitted) continue;
				const auto Draw = Captured.GetBatch().GetElement(Index).Draw;
				Facts.bSupportsPreparation = Factory.Supports(PreparationPass, Draw);
				if (Mode != ERenderPreparationMode::ShadowDepth && Facts.bSupportsPreparation)
					Facts.bSupportsGBuffer = Factory.Supports(GBufferPass, Draw);
			}
		};

		size_t PrimitiveIndex = 0;
		for (const auto* SceneInfo : SceneInfos)
		{
			const auto& Input = CollectionInputs[PrimitiveIndex++];
			auto& Primitive = Collected->Primitives.emplace_back();
			if (!SceneInfo) continue;
			Primitive.bPresent = true;
			Primitive.FeatureLevel = SceneInfo->GetProxy().GetFeatureLevel();
			if (Input.Static) Primitive.StaticMeshes = Input.Static->Publication;
			Primitive.Id = SceneInfo->GetId();
			Primitive.SceneInfo = SceneInfo->GetSceneInfoRef();
			Primitive.PersistentPrimitiveIndex = SceneInfo->GetPersistentIndex();
			Primitive.bSplineMesh = SceneInfo->GetKind() == EPrimitiveSceneProxyKind::SplineMesh;
			Primitive.WorldBounds = SceneInfo->GetWorldBounds();
			if (!Input.Static && !Input.ViewFactIndex)
			{
				Primitive.Outcomes[static_cast<size_t>(EGeometrySubmissionOutcome::Excluded)] = 1;
				continue;
			}
			const auto* ViewFact = Input.ViewFactIndex ? &ViewFacts.Primitives[*Input.ViewFactIndex] : nullptr;
			if (ViewFact) Primitive.ProjectionStatus = ViewFact->Projected.Status;
			else Primitive.ProjectionStatus.reset();
			if (auto* Static = Input.Static)
			{
				const auto AddMesh = [&](size_t MeshIndex, uint32 RequestedLOD) {
					auto Batch = GetAdmittedStaticBatch(Collection, *Static, *SceneInfo, MeshIndex, Purpose,
						SharedCache ? &SharedCache->Admission : nullptr);
					const auto Outcome = Batch ? EGeometrySubmissionOutcome::Submitted : Batch.error();
					++Primitive.Outcomes[static_cast<size_t>(Outcome)];
					if (!Batch) return;
					auto& Captured = Primitive.Batches.emplace_back();
					Captured.Source = std::move(*Batch);
					require(MeshIndex < INDEX_NONE_U32);
					Captured.StaticMeshIndex = static_cast<uint32>(MeshIndex);
					Captured.RequestedLOD = RequestedLOD;
				};
				const auto& LODs = Static->Publication->LODs;
				const auto LOD = ViewFact ? *ViewFact->LOD : FPreparedMeshLODSelection{0, ResolveAvailableStaticMeshLOD(0, LODs)};
				bool bFound = false;
				for (size_t Index = 0; Index < Static->Publication->Meshes.size(); ++Index)
				{
					const auto Selected = Static->Publication->Meshes[Index].SelectedLOD;
					if (Selected != LOD.Selected) continue;
					AddMesh(Index, LOD.Requested);
					bFound = true;
				}
				if (!bFound) ++Primitive.Outcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)];
			}
			else
			{
				FMeshBatchCollector Collector(Purpose, SharedCache ? &SharedCache->Admission : nullptr);
				FMeshCollectionContext Context;
				const auto& ProjectedSize = ViewFact->Projected;
				Context.PreparedLOD = ViewFact->LOD;
				Context.PrimitiveId = SceneInfo->GetId();
				Context.CustomPrimitiveData = SceneInfo->GetProxy().GetCustomPrimitiveData();
				Context.CustomInstanceData = SceneInfo->GetProxy().GetDefaultInstanceCustomData();
				Context.LocalToWorld = SceneInfo->GetProxy().GetLocalToWorld();
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
				Context.Purpose = Purpose;
				DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.CollectMeshBatches");
				SceneInfo->GetProxy().CollectMeshBatches(Context, Collector);
				for (size_t I = 0; I < Primitive.Outcomes.size(); ++I)
					Primitive.Outcomes[I] = Collector.GetOutcomeCount(static_cast<EGeometrySubmissionOutcome>(I));
				auto Batches = std::move(Collector).TakeBatches();
				Primitive.Batches.reserve(Batches.size());
				for (auto& Batch : Batches)
				{
					auto& Captured = Primitive.Batches.emplace_back();
					Captured.RequestedLOD = Batch.RequestedLOD;
					Captured.Source = std::move(Batch);
				}
			}
			for (auto& Captured : Primitive.Batches)
			{
				const auto& Batch = Captured.GetBatch();
				if (Batch.PrimitiveIdMode == EPrimitiveIdMode::PrimID_DynamicPrimitiveShaderData)
					Captured.DynamicPrimitiveData = std::make_shared<FGPUSceneDynamicPrimitiveData>(FGPUSceneDynamicPrimitiveData{
						Batch.LocalToWorld, Batch.WorldBounds, Batch.CustomPrimitiveData, Batch.CustomInstanceData});
				if (Batch.StaticInputs && SharedCache && SharedCache->Commands)
					Captured.StaticFacts = SharedCache->Commands->CaptureStaticFacts(
						Batch.StaticInputs, Mode == ERenderPreparationMode::ShadowDepth);
				if (Captured.StaticFacts)
				{
					Captured.InputBinding = Captured.StaticFacts->Binding;
					Captured.bFactoryCompatible = Captured.StaticFacts->bFactoryCompatible;
					continue;
				}
				Captured.InputBinding = std::dynamic_pointer_cast<const FVertexFactoryInputBinding>(Batch.Binding);
				if (!Captured.InputBinding || !Captured.InputBinding->Declaration || Captured.InputBinding->Streams.empty()) continue;
				const auto Factory = FindMeshVertexFactory(Batch.FactoryKey);
				Captured.bFactoryCompatible = Factory && Factory->GetLayoutKey() == Batch.LayoutKey;
				if (!Captured.bFactoryCompatible) continue;
				Captured.Elements.resize(Batch.GetNumElements());
				if (!Batch.GeometryRecord || dynamic_cast<const FSplineMeshBatchBinding*>(Captured.InputBinding.get()))
					SnapshotValidation.push_back({&Captured, Factory});
				else ResolveFactorySupport(Captured, *Factory);
			}
		}

		RunPreparationRanges("Renderer.ResolveMeshInputFacts", SnapshotValidation.size(), 32,
			SnapshotValidation.size() >= 64, [&](size_t Index) {
				auto& Captured = *SnapshotValidation[Index].Captured;
				const auto& Batch = Captured.GetBatch();
				if (!Batch.GeometryRecord)
				{
					DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ValidateMeshInputs");
					for (size_t ElementIndex = 0; ElementIndex < Captured.Elements.size(); ++ElementIndex)
					{
						const auto Element = Batch.GetElement(ElementIndex);
						Captured.Elements[ElementIndex].InputOutcome = ValidateMeshGeometryInputs(*Captured.InputBinding,
							Element.Draw, Element.Vertices, Element.InstanceStreams);
					}
				}
				if (const auto* Spline = dynamic_cast<const FSplineMeshBatchBinding*>(Captured.InputBinding.get()))
				{
					const auto& Params = Spline->DynamicData.Params;
					bool bReverseWinding = false;
					bool bChangesOrientation = false;
					// A zero or sign change cannot be represented by one front face.
					for (size_t Axis = 0; Axis < 2; ++Axis)
					{
						const double Start = Params.StartScale[Axis];
						const double End = Params.EndScale[Axis];
						bChangesOrientation |= Start == 0.0 || End == 0.0 || (Start < 0.0) != (End < 0.0);
						bReverseWinding ^= Start < 0.0;
					}
					Captured.bReverseSplineWinding = bReverseWinding;
					Captured.bSplineChangesOrientation = bChangesOrientation;
				}
			});
		for (const auto& Work : SnapshotValidation) ResolveFactorySupport(*Work.Captured, *Work.Factory);
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
		return FinishStaticMeshView(BeginStaticMeshView_RenderThread(CommandList, SceneInfos, View, RasterMode, Mode, SharedCache));
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
		if (Resolved->Collected->RenderFrame != GRenderFrameCounterRenderThread)
		{
			Resolved->Collected = std::make_shared<FCollectedStaticMeshView>();
			return Resolved;
		}
		const auto& View = Resolved->Collected->View;
		const auto RasterMode = Resolved->Collected->RasterMode;
		const auto Mode = Resolved->Collected->Mode;
		Resolved->Primitives.reserve(Resolved->Collected->Primitives.size());
		// Aliases refer to stable elements of this view's block, retained by every consuming view.
		std::shared_ptr<std::deque<FResolvedStaticMeshBatch>> ShadowStorage;
		for (const auto& Primitive : Resolved->Collected->Primitives)
		{
			auto& FrozenPrimitive = Resolved->Primitives.emplace_back();
			FrozenPrimitive.bSourceAvailable = !Primitive.SceneInfo.IsSceneBound() || Primitive.SceneInfo.Resolve();
			const bool bShareShadow = SharedCache && Cache.Commands && Mode == ERenderPreparationMode::ShadowDepth
				&& Primitive.StaticMeshes && std::ranges::all_of(Primitive.Batches, [](const auto& B) { return B.StaticFacts != nullptr; });
			const bool bSplineMesh = Primitive.bSplineMesh;
			if (bShareShadow) FrozenPrimitive.SharedShadowBatches.reserve(Primitive.Batches.size());
			else FrozenPrimitive.Batches.reserve(Primitive.Batches.size());
			for (const auto& Captured : Primitive.Batches)
			{
				FStaticMeshPreparationCache::FStaticBatchEntry* ShadowEntry = nullptr;
				FResolvedStaticMeshBatch* ShadowBatch = nullptr;
				if (bShareShadow)
				{
					const auto& Handle = std::get<FStaticMeshPreparationCache::FAdmittedBatch>(Captured.Source);
					auto& Entries = Cache.StaticCollection.Batches;
					if (Handle.Index < Entries.size() && Entries[Handle.Index].Snapshot == Handle.Snapshot)
						ShadowEntry = &Entries[Handle.Index];
					if (ShadowEntry && ShadowEntry->ResolvedShadow && ShadowEntry->RasterMode == RasterMode
						&& ShadowEntry->DepthConvention == View.DepthConvention && ShadowEntry->ShadowFacts == Captured.StaticFacts)
					{
						FrozenPrimitive.SharedShadowBatches.push_back(ShadowEntry->ResolvedShadow);
						continue;
					}
					if (!ShadowStorage) ShadowStorage = std::make_shared<std::deque<FResolvedStaticMeshBatch>>();
					ShadowBatch = &ShadowStorage->emplace_back();
					FrozenPrimitive.SharedShadowBatches.emplace_back(ShadowStorage, ShadowBatch);
				}
				auto& FrozenBatch = ShadowBatch ? *ShadowBatch : FrozenPrimitive.Batches.emplace_back();
				const auto& Batch = Captured.GetBatch();
				const auto& Binding = Captured.InputBinding;
				if (!Binding || !Binding->Declaration || Binding->Streams.empty() || !Captured.bFactoryCompatible) continue;
				if (const auto* Static = std::get_if<FStaticMeshPreparationCache::FAdmittedBatch>(&Captured.Source))
					FrozenBatch.Transform = Static->Snapshot->Transform;
				else
					FrozenBatch.Transform = Cache.Admission.ResolveTransform(Primitive.Id, Batch.BatchId, Batch.LocalToWorld);
				if (!FrozenBatch.Transform.bValid) continue;
				const bool bReverseCulling = (FrozenBatch.Transform.Determinant < 0.0) != Captured.bReverseSplineWinding;
				const bool bSplineChangesOrientation = Captured.bSplineChangesOrientation;
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
							if (!MaterialIndex)
							{
								(*Elements)[Index].bMaterialUnavailable = true;
								continue;
							}
							(*Elements)[Index] = {Command, *MaterialIndex, true};
						}
						Found =
							Cache.ResolvedSections
								.emplace(StableSections.get(),
									FStaticMeshPreparationCache::FResolvedSections{StableSections, std::move(Elements)})
								.first;
					}
					FrozenBatch.StaticElements = Found->second.Elements;
					if (ShadowEntry)
					{
						ShadowEntry->RasterMode = RasterMode;
						ShadowEntry->DepthConvention = View.DepthConvention;
						ShadowEntry->ShadowFacts = Captured.StaticFacts;
						ShadowEntry->ResolvedShadow = FrozenPrimitive.SharedShadowBatches.back();
					}
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
					if (Material.Representation.IsPending())
					{
						bStableMaterials = false;
						continue;
					}
					if (Material.CompiledProgram && Material.CompiledProgram->FeatureLevel != Primitive.FeatureLevel)
					{
						Item.bMaterialUnavailable = true;
						bStableMaterials = false;
						continue;
					}
					{
						DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ResolveMeshMaterial");

						const auto MaterialIndex = Cache.ResolveMaterial(Material);
						if (!MaterialIndex)
						{
							Item.bMaterialUnavailable = true;
							bStableMaterials = false;
							continue;
						}
						Item.MaterialUniformIndex = *MaterialIndex;

					}
					bStableMaterials &=
						Material.Representation.GetRecordId() == Element.Material.Representation.GetRecordId() &&
						Material.CompiledProgram == Element.Material.CompiledProgram && Material.UniformExpressionCache == Element.Material.UniformExpressionCache;

					const FMeshDrawDescriptionCache::FKey Key{
						Batch.GeometryRecord ? Batch.GeometryRecord->GetRecordId() : 0,
						SectionIndex, Material.Representation.GetRecordId(), Policy, reinterpret_cast<uint64>(Material.UniformExpressionCache.get())};
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
						Candidate->MaterialSlotIndex = Element.MaterialSlotIndex;
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
						const auto& MaterialIdentity = Candidate->Material.PlanningPassIdentity;
						const auto ReceiverPass = MaterialIdentity.bIsSky ? EMeshPipelinePass::Sky : EMeshPipelinePass::Forward;
						const auto Pass = Mode == ERenderPreparationMode::ShadowDepth ? EMeshPipelinePass::ShadowDepth : ReceiverPass;
						if (!FPassProcessorManager::GetMeshPassProcessor(EShadingPath::Deferred, Pass).IsMeshRelevant(MaterialIdentity))
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

	auto BeginStaticMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View, ERasterMode RasterMode, ERenderPreparationMode Mode,
		FStaticMeshPreparationCache* SharedCache) -> FStaticMeshPreparationWork
	{
		auto Collected = CollectStaticMeshView_RenderThread(CommandList, SceneInfos, View, RasterMode, Mode, SharedCache);
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
		const FBoxScreenSizeProjection Projection(View);
		FPreparedStaticMeshView Result;
		Result.RenderFrame = Collected.RenderFrame;
		Result.Primitives.reserve(PrimitiveCount);
		auto PreparePrimitive = [&](const FCollectedStaticMeshView::FPrimitive& Primitive) {
			DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareMeshPrimitive");
			if (!Resolved.Primitives[&Primitive - Collected.Primitives.data()].bSourceAvailable)
			{
				++Result.VisibleCandidates; ++Result.RejectedPrimitives;
				return;
			}
			if (!Primitive.bPresent)
			{
				++Result.RejectedPrimitives;
				return;
			}
			const size_t ResourceFailuresBefore = Result.SubmissionOutcomes[
				static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)];
			auto CaptureResourceFailure = [&]() {
				if (Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)] > ResourceFailuresBefore)
					Result.ResourceFailurePrimitives.push_back(Primitive.Id);
			};
			const bool bSplineMesh = Primitive.bSplineMesh;
			++Result.VisibleCandidates;
			if (bSplineMesh) ++Result.VisibleSplineCandidates;
			else ++Result.VisibleLocalCandidates;
			++Result.SharedPrimitiveFactBuilds;
			const auto ProjectionStatus = Primitive.ProjectionStatus ? *Primitive.ProjectionStatus
				: Projection.Project(Primitive.WorldBounds).Status;
			if (ProjectionStatus != EProjectedScreenSizeStatus::Valid) ++Result.ProjectedSizeFallbacks;
			for (size_t I = 0; I < Result.SubmissionOutcomes.size(); ++I)
				Result.SubmissionOutcomes[I] += Primitive.Outcomes[I];
			Result.bResourceFailure |= Primitive.Outcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)] != 0;
			if (Primitive.Batches.empty())
			{
				CaptureResourceFailure();
				++Result.RejectedPrimitives;
				return;
			}
			const size_t FirstPrimitiveBatch = Result.Primitives.size();
			auto PrepareBatch = [&](const FCollectedStaticMeshView::FBatch& Captured) {
				const auto& Batch = Captured.GetBatch();
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
				const uint32 RequestedLODIndex = Captured.RequestedLOD;
				const uint32 SelectedLODIndex = Batch.SelectedLOD;

				const FMatrix& LocalToWorld = Batch.LocalToWorld;
				const auto& FrozenBatch = Resolved.Primitives[&Primitive - Collected.Primitives.data()]
					.GetBatch(&Captured - Primitive.Batches.data());
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
				const bool bReusePrimitive = Primitive.StaticMeshes && Result.Primitives.size() > FirstPrimitiveBatch
					&& Result.Primitives.back().GeometryRecord == Batch.GeometryRecord
					&& Result.Primitives.back().SelectedLODIndex == SelectedLODIndex;
				const uint32 PrimitiveIndex = static_cast<uint32>(Result.Primitives.size() - (bReusePrimitive ? 1 : 0));
				if (!bReusePrimitive)
				{
					Result.Primitives.push_back({.DynamicPrimitiveData = Captured.DynamicPrimitiveData, .CustomPrimitiveData = Batch.CustomPrimitiveData,
						.CustomInstanceData = Batch.CustomInstanceData,
						.PrimitiveId = Primitive.Id,
						.BatchId = Batch.BatchId,
						.PrimitiveIdMode = Batch.PrimitiveIdMode,
						.PersistentPrimitiveIndex = Primitive.PersistentPrimitiveIndex,
						.InstanceSceneDataId = Batch.InstanceSceneDataId,
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
						.StaticMeshes = Primitive.StaticMeshes, .SceneInfo = Primitive.SceneInfo});
				}
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
					if (!FrozenElement.Command)
					{
						if (FrozenElement.bMaterialUnavailable)
						{
							++Result.SubmissionOutcomes[static_cast<size_t>(EGeometrySubmissionOutcome::ResourceFailure)];
							Result.bResourceFailure = true;
						}
						continue;
					}
					Item.Command = FrozenElement.Command;
					Item.StaticMeshIndex = Captured.StaticMeshIndex;
					Item.MaterialUniformIndex = FrozenElement.MaterialUniformIndex;
					if (FrozenElement.bTemplateReused) ++Result.DrawDescriptionReuses;
					else ++Result.DrawDescriptionBuilds;
					Item.SortKey.State = {Item.Command, &Item.Command->SortKey};
					Item.PrimitiveIndex = PrimitiveIndex;
					Item.SortKey.PrimitiveId = Primitive.Id.Value;
					Item.SortKey.BatchId = Batch.BatchId;
					Item.SortKey.SelectedLODIndex = SelectedLODIndex;

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
					{
						FVector3 SortCenter = BoundsCenter;
						if (Element.LocalBounds.bIsValid && Math::IsFinite(Element.LocalBounds.Min) && Math::IsFinite(Element.LocalBounds.Max))
						{
							const FVector4 Center = LocalToWorld * FVector4(Element.LocalBounds.GetCenter(), 1.0);
							if (Math::IsFinite(Center)) SortCenter = FVector3(Center);
						}
						if (!Math::IsFinite(SortCenter)) SortCenter = FVector3(LocalToWorld[3]);
						Item.TranslucentSortDepth = ComputeTranslucentSortDepth(View, SortCenter);
						if (!std::isfinite(Item.TranslucentSortDepth)) continue;
						++Result.TranslucentSections;
						Result.TranslucentTriangles += TriangleCount;
						Result.Translucent.push_back(std::move(Item));
						break;
					}
					}
					Result.SelectedTriangles += TriangleCount;
				}

				const size_t PreparedSectionCount =
					Result.GetNumSections() - FirstSectionCount;
				if (PreparedSectionCount == 0)
				{
					if (!bReusePrimitive) Result.Primitives.pop_back();
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
				if (bReusePrimitive) return;
				if (SelectedLODIndex != RequestedLODIndex) ++Result.ResourceFallbacks;
				++Result.SelectedLODFactBuilds;
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
			CaptureResourceFailure();
			if (Result.Primitives.size() == FirstPrimitiveBatch) ++Result.RejectedPrimitives;
			else if (bSplineMesh) ++Result.PreparedSplinePrimitives;
			else ++Result.PreparedLocalPrimitives;
		};
		std::ranges::for_each(std::span(Collected.Primitives).subspan(FirstPrimitive, PrimitiveCount), PreparePrimitive);
		return Result;
	}

	auto FResolvedStaticMeshView::GetInstanceCullingDrawParams(FRDGBuilder& Graph, FInstanceCullingDrawParams Parameters) const -> FInstanceCullingDrawParams
	{
		if (!GPUSceneBuffers) return Parameters;
		Parameters.Scene = GPUSceneBuffers->GetShaderParameters(Graph);
		// Direct and CPU-compacted commands may coexist with indirect draws.
		// Each fallback command retains its selected ID buffer; the generated stream
		// in InstanceIdOffsetBuffer overrides it only for admitted indirect draws.
		Parameters.InstanceIdsBuffer = RegisterGPUSceneBuffer(Graph, GPUSceneBuffers->InstanceIds);
		Parameters.DynamicInstanceIdsBuffer = RegisterGPUSceneBuffer(Graph, DynamicInstancingInstanceIds);
		return Parameters;
	}
	auto CollectGPUScenePrimitives(const FPreparedStaticMeshView& Prepared, const std::shared_ptr<FGPUSceneDynamicContext>& Context)
		-> std::vector<FGPUScenePrimitiveData>
	{
		std::vector<uint32> Counts(Prepared.Primitives.size(), 1), Indices(Prepared.Primitives.size(), INDEX_NONE_U32);
		std::vector<bool> Supported(Prepared.Primitives.size());
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			if (!Draw.Command) continue;
			Counts[Draw.PrimitiveIndex] = std::max(Counts[Draw.PrimitiveIndex], Draw.Command->Geometry.InstanceCount);
			const auto Factory = FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
			if (Factory && Factory->SupportsGPUScene()) Supported[Draw.PrimitiveIndex] = true;
		}
		FGPUScenePrimitiveCollector Collector(Context);
		for (uint32 Index = 0; Index < Prepared.Primitives.size(); ++Index)
		{
			if (!Supported[Index]) continue;
			const auto& Primitive = Prepared.Primitives[Index];
			if (Context->IsUsingPersistentGPUScene() && Primitive.PrimitiveIdMode == EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo)
				{
				if (!Primitive.PersistentPrimitiveIndex.IsValid()) return {};
				if (Primitive.SceneInfo.IsSceneBound() && (!Primitive.SceneInfo.Resolve()
					|| Primitive.SceneInfo.Resolve() != Context->GetGPUScene().GetPrimitiveSceneInfo(Primitive.PersistentPrimitiveIndex))) return {};
				Indices[Index] = Collector.Add(Primitive.PersistentPrimitiveIndex, Primitive.InstanceSceneDataId, Counts[Index]);
			}
			else
			{
				auto Record = Primitive.DynamicPrimitiveData;
				if (!Record) Record = std::make_shared<FGPUSceneDynamicPrimitiveData>(FGPUSceneDynamicPrimitiveData{
					Primitive.LocalToWorld, Primitive.WorldBounds, Primitive.CustomPrimitiveData, Primitive.CustomInstanceData});
				Indices[Index] = Collector.Add(std::move(Record), Counts[Index]);
			}
		}
		if (!Collector.Commit()) return {};
		std::vector<FGPUScenePrimitiveData> Result(Prepared.Primitives.size());
		for (uint32 Index = 0; Index < Result.size(); ++Index)
			if (Indices[Index] != INDEX_NONE_U32) Result[Index] = Collector.GetPrimitiveData(Indices[Index]);
		return Result;
	}

	auto AdmitStaticMeshDraws(FPreparedStaticMeshView& View, const FResolvedStaticMeshView& Resources) -> size_t
	{
		std::unordered_set<uint64> Withheld;
		for (const auto Id : View.ResourceFailurePrimitives) Withheld.insert(Id.Value);
		for (const auto* Bucket : {&View.Opaque, &View.Masked, &View.Translucent})
			for (const auto& Draw : *Bucket)
				if (!Resources.IsReady(Draw))
					if (const auto* Primitive = View.GetPrimitive(Draw))
						Withheld.insert(Primitive->PrimitiveId.Value);

		for (auto* Bucket : {&View.Opaque, &View.Masked, &View.Translucent})
			std::erase_if(*Bucket, [&](const auto& Draw) {
				const auto* Primitive = View.GetPrimitive(Draw);
				return !Primitive || Withheld.contains(Primitive->PrimitiveId.Value);
			});
		return Withheld.size();
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
		Result.RenderFrame = Inputs->Collected->RenderFrame;
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
			Result.ResourceFailurePrimitives.insert(Result.ResourceFailurePrimitives.end(),
				Chunk.Output.ResourceFailurePrimitives.begin(), Chunk.Output.ResourceFailurePrimitives.end());
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
	FStaticMeshPreparationWork::~FStaticMeshPreparationWork()
	{
		if (Completion.IsValid()) require(Completion.Wait());
		// Shutdown can cancel the continuation while workers still own the inputs.
		for (const auto& Worker : Workers) require(Worker.Wait());
	}

	auto StartStaticMeshPreparation(std::shared_ptr<const FStaticMeshPreparationInputs> Inputs,
		EStaticMeshPreparationPolicy Policy, std::function<bool()> IsCancellationRequested) -> FStaticMeshPreparationWork
	{
		check(Inputs && Inputs->Collected);
		FStaticMeshPreparationWork Work;
		Work.IsCancellationRequested = std::move(IsCancellationRequested);
		Work.bCanceled = Work.IsCancellationRequested && Work.IsCancellationRequested();
		if (Work.bCanceled) return Work;
		const size_t Count = Inputs->Collected->Primitives.size();
		if (Policy == EStaticMeshPreparationPolicy::Inline || !IsTaskSchedulerRunning()
			|| Count == 0 || (Policy == EStaticMeshPreparationPolicy::Auto && Count < 256))
		{
			Work.InlineOutput = PrepareStaticMeshInputs(*Inputs);
			return Work;
		}

		constexpr size_t ChunkSize = 128;
		constexpr size_t MaxWorkers = 8;
		const size_t ChunkCount = 1 + (Count - 1) / ChunkSize;
		const size_t WorkerCount = std::min(MaxWorkers, ChunkCount);
		auto Chunks = std::make_shared<std::vector<FStaticMeshPreparationChunk>>(ChunkCount);
		Work.Workers.reserve(WorkerCount);
		for (size_t WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
		{
			Work.Workers.push_back(Tasks::LaunchTask("Renderer.PrepareMeshChunks",
				[Inputs, Chunks, Count, ChunkSize, WorkerIndex, WorkerCount] {
					for (size_t Index = WorkerIndex; Index < Chunks->size(); Index += WorkerCount)
					{
						const size_t First = Index * ChunkSize;
						(*Chunks)[Index] = PrepareStaticMeshInputChunk(Inputs, First, std::min(ChunkSize, Count - First));
					}
				}));
		}
		Work.Completion = Tasks::LaunchTask("Renderer.FinalizeMeshPreparation",
			[Chunks, Workers = Work.Workers]() -> std::expected<FPreparedStaticMeshView, FStaticMeshPreparationError> {
				for (size_t Index = 0; Index < Chunks->size(); ++Index)
				{
					if (!(*Chunks)[Index].Inputs)
						return std::unexpected(FStaticMeshPreparationError{Index, Workers[Index % Workers.size()].GetState()});
				}
				const size_t PreparedChunks = Chunks->size();
				auto Merged = MergeStaticMeshPreparationChunks(std::move(*Chunks));
				if (!Merged) return std::unexpected(FStaticMeshPreparationError{0, ETaskState::Failed, Merged.error()});
				Merged->PreparationTaskCount = PreparedChunks;
				return std::move(*Merged);
			}, Work.Workers);
		return Work;
	}

	auto FinishStaticMeshPreparation(FStaticMeshPreparationWork Work)
		-> std::expected<FPreparedStaticMeshView, FStaticMeshPreparationError>
	{
		const auto IsCanceled = [&] {
			return Work.bCanceled || (Work.IsCancellationRequested && Work.IsCancellationRequested());
		};
		if (IsCanceled()) return std::unexpected(FStaticMeshPreparationError{0, ETaskState::Canceled});
		if (Work.InlineOutput) return std::move(*Work.InlineOutput);
		const auto JoinStart = std::chrono::steady_clock::now();
		const auto Wait = Work.Completion.WaitDetailed();
		const auto JoinDuration = std::chrono::steady_clock::now() - JoinStart;
		require(Wait.WaitStatus == ETaskWaitStatus::Completed);
		if (Wait.TaskState != ETaskState::Succeeded)
			return std::unexpected(FStaticMeshPreparationError{0, Wait.TaskState});
		if (IsCanceled()) return std::unexpected(FStaticMeshPreparationError{0, ETaskState::Canceled});
		auto Prepared = std::move(Work.Completion.GetResult());
		if (Prepared)
			Prepared->PreparationJoinNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(JoinDuration).count());
		return Prepared;
	}
} // namespace Durin

namespace Durin::RendererPrivate
{
	auto BuildFallbackPrimitiveUniform(
		const FPreparedStaticMeshPrimitive& Primitive) -> FMeshFallbackPrimitiveUniform
	{
		FMeshFallbackPrimitiveUniform TransformUniform;
		TransformUniform.CustomPrimitiveData = Primitive.CustomPrimitiveData;
		TransformUniform.CustomInstanceData = Primitive.CustomInstanceData;
		const FVector3 Origin(Primitive.LocalToWorld[3]);
		FMatrix LocalToWorld = Primitive.LocalToWorld;
		LocalToWorld[3] = FVector4(0.0, 0.0, 0.0, 1.0);
		TransformUniform.LocalToWorld = Math::TransposeToFloat(LocalToWorld);
		TransformUniform.NormalToWorld = Primitive.NormalToWorld;
		TransformUniform.WorldToLocal = Math::TransposeToFloat(
			Primitive.WorldToLocal * Math::TranslationMatrix(Origin));
		TransformUniform.BoundsCenter = FVector4f(FVector3f(Primitive.BoundsCenter - Origin), 1.0f);
		const FVector3f OriginHigh(Origin);
		TransformUniform.WorldOriginHigh = FVector4f(OriginHigh, 0.0f);
		TransformUniform.WorldOriginLow = FVector4f(FVector3f(Origin - FVector3(OriginHigh)), 0.0f);
		TransformUniform.TransformParams.x = Math::LinearDeterminant(Primitive.LocalToWorld) < 0.0 ? -1.0f : 1.0f;
		return TransformUniform;
	}

	auto PrepareFallbackPrimitiveUniform(FRHICommandListImmediate& Commands, const FPreparedStaticMeshPrimitive& Primitive) -> FRHIUniformBufferRange
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PreparePrimitiveUniform");
		const auto Uniform = BuildFallbackPrimitiveUniform(Primitive);
		return Commands.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
	}

	auto ResolveMeshVertexDeclaration(const FPreparedStaticMeshPrimitive& Primitive, const FPreparedStaticMeshDraw& Draw) -> FVertexDeclarationRHIRef
	{
		if (!Primitive.CollectedBinding || !Primitive.CollectedBinding->Declaration || !Draw.Command
			|| Draw.Command->Geometry.Validate(Draw.Command->Vertices.Range, Draw.Command->Indices.Range) != EGeometrySubmissionOutcome::Submitted) return {};
		return Primitive.CollectedBinding->Declaration;
	}
}
