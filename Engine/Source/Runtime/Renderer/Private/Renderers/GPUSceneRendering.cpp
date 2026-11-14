#include "Renderers/GPUSceneRendering.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "GPUScene.h"
#include "PrimitiveSceneShaderData.h"
#include "Renderers/MeshVertexFactory.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FGPUSceneDynamicContext::UploadInstanceIds(FRHICommandListImmediate& Commands, std::span<const uint32> Ids) -> FBufferRHIRef
	{
		const auto Upload = GPUScene->UploadInstanceIds(Commands, Ids);
		if (!Upload) return {};
		if (std::ranges::find(InstanceIdUploads, Upload) == InstanceIdUploads.end()) InstanceIdUploads.push_back(Upload);
		return Upload->Buffer;
	}

	auto FGPUSceneBufferSet::GetShaderParameters() const -> FGPUSceneShaderParameters
	{
		auto Range = [](const FBufferRHIRef& Buffer) -> FRHIStorageBufferRange { return {Buffer, 0, Buffer ? Buffer->GetSize() : 0}; };
		return {Range(SceneData[0]), Range(SceneData[1]), Range(SceneData[2]), Range(InstanceIds)};
	}

	FGPUSceneDynamicContext::FGPUSceneDynamicContext(std::shared_ptr<FGPUScene> InGPUScene) : GPUScene(InGPUScene ? InGPUScene : CreateRenderResource<FGPUScene>()), bStandaloneScene(!InGPUScene) {}
	FGPUSceneDynamicContext::~FGPUSceneDynamicContext()
	{
		if (Allocations.empty()) return;
		if (IsInRenderingThread()) for (const auto& Range : Allocations) GPUScene->ReleaseDynamicPrimitiveRange(Range);
		else EnqueueRenderCommand("ReleaseDynamicGPUSceneRanges", [Scene = std::move(GPUScene), Ranges = std::move(Allocations)](FRHICommandListImmediate&) {
			for (const auto& Range : Ranges) Scene->ReleaseDynamicPrimitiveRange(Range);
		});
	}
	auto FGPUSceneDynamicContext::RegisterGraphUse(const FRDGBuilder& Graph) -> void
	{
		GPUScene->RegisterGraphUse(Graph);
		for (const auto& Upload : InstanceIdUploads)
		{
			if (Graph.GetGPUCompletion()) Upload->LastUse = Graph.GetGPUCompletion();
			else if (Graph.GetExecutionResult() && !Graph.GetExecutionResult()->has_value()
				&& GetRDGExecutionStatus(*Graph.GetExecutionResult()) != ERDGExecutionStatus::CompileFailed) Upload->LastUse = {};
		}
	}
	auto FGPUSceneDynamicContext::CommitPrimitiveCollector(FGPUScenePrimitiveCollector& Collector, const FPreparedStaticMeshView& Prepared) -> std::vector<FGPUScenePrimitiveData>
	{
		CheckRenderingThread();
		std::vector<FGPUScenePrimitiveData> Result(Prepared.Primitives.size());
		std::vector<uint32> Counts(Result.size(), 1);
		std::vector<bool> Supported(Result.size());
		for (uint32 Index = 0; Index < Prepared.GetNumSections(); ++Index)
		{
			const auto& Draw = Prepared.GetDraw(Index);
			if (!Draw.Command) continue;
			Counts[Draw.PrimitiveIndex] = std::max(Counts[Draw.PrimitiveIndex], Draw.Command->Geometry.InstanceCount);
			const auto Factory = RendererPrivate::FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
			if (Factory && Factory->SupportsGPUScene()) Supported[Draw.PrimitiveIndex] = true;
		}
		if (std::ranges::none_of(Supported, [](bool Value) { return Value; })) return Result;
		GPUScene->UpdateCPUData();
		std::vector<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>> Records(Result.size());
		std::vector<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>> NewRecords;
		std::set<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>, std::owner_less<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>>> NewIdentities;
		for (uint32 Index = 0; Index < Result.size(); ++Index)
		{
			const auto& Primitive = Prepared.Primitives[Index];
			if (!Supported[Index] || (!bStandaloneScene && Primitive.PrimitiveIdMode == EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo)) continue;
			auto Record = Primitive.DynamicPrimitiveData;
			if (!Record) Record = std::make_shared<FGPUSceneDynamicPrimitiveData>(FGPUSceneDynamicPrimitiveData{
				Primitive.LocalToWorld, Primitive.WorldBounds, Primitive.CustomPrimitiveData, Primitive.CustomInstanceData});
			Records[Index] = Record;
			if (!DynamicInstances.contains(Record) && NewIdentities.insert(Record).second) NewRecords.push_back(Record);
		}
		const auto Allocation = GPUScene->AllocateDynamicPrimitiveRange(static_cast<uint32>(NewRecords.size()));
		if (Allocation.Count) Allocations.push_back(Allocation);
		for (uint32 Index = 0; Index < NewRecords.size(); ++Index)
		{
			const auto& Record = NewRecords[Index];
			if (!GPUScene->WriteDynamicPrimitive(Allocation, Index, Record->LocalToWorld, Record->WorldBounds,
				Record->CustomPrimitiveData, Record->CustomInstanceData)) return {};
			DynamicInstances.emplace(Record, Allocation.InstanceOffset + Index);
		}

		auto Buffers = std::make_shared<FGPUSceneBufferSet>(); Buffers->PrimitiveCollector = Collector.shared_from_this(); Buffers->bStandaloneScene = bStandaloneScene;
		std::vector<uint32> Ids;
		for (uint32 Index = 0; Index < Result.size(); ++Index)
		{
			if (!Supported[Index]) continue;
			const auto& Primitive = Prepared.Primitives[Index];
			uint32 InstanceId;
			if (!bStandaloneScene && Primitive.PrimitiveIdMode == EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo)
			{
				const auto Existing = GPUScene->FindInstanceId(Primitive.PersistentPrimitiveIndex, Primitive.InstanceSceneDataId);
				if (!Existing) return {};
				InstanceId = *Existing;
			}
			else
			{
				InstanceId = DynamicInstances.at(Records[Index]);
			}
			if (uint64(InstanceIds.size()) + Ids.size() + Counts[Index] > UINT32_MAX / sizeof(uint32)) return {};
			Result[Index] = {Buffers, InstanceId, static_cast<uint32>(InstanceIds.size() + Ids.size())};
			Ids.insert(Ids.end(), Counts[Index], InstanceId);
		}
		Collector.Buffers = Buffers;
		InstanceIds.insert(InstanceIds.end(), Ids.begin(), Ids.end());
		PendingCollectors.push_back(Collector.shared_from_this());
		return Result;
	}
	auto FGPUScenePrimitiveCollector::Commit(const FPreparedStaticMeshView& Prepared) -> std::vector<FGPUScenePrimitiveData>
	{
		require(!bCommitted && Context); bCommitted = true;
		return Context->CommitPrimitiveCollector(*this, Prepared);
	}
	auto FGPUSceneDynamicContext::Upload(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator) -> bool
	{
		CheckRenderingThread();
		if (PendingCollectors.empty()) return true;
		if (GPUScene->IsDirty())
		{
			FRDGBuilder Graph;
			const auto Update = GPUScene->Update(Graph);
			const auto Executed = Graph.Execute(Commands, &Allocator);
			if (!GPUScene->CompleteUpdate(Graph, Update) || !Executed) return false;
		}
		const auto UploadedIds = InstanceIds.empty() ? FBufferRHIRef{} : UploadInstanceIds(Commands, InstanceIds);
		if (!InstanceIds.empty() && !UploadedIds) return false;
		for (const auto& Weak : PendingCollectors) if (const auto View = Weak.lock()) if (const auto Buffers = View->Buffers.lock())
		{
			Buffers->SceneData = GPUScene->GetResourceBuffers();
			Buffers->InstanceIds = UploadedIds;
		}
		PendingCollectors.clear();
		InstanceIds.clear();
		return true;
	}
	auto FGPUSceneDynamicContext::Prepare(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator,
		const FPreparedStaticMeshView& Prepared) -> std::vector<FGPUScenePrimitiveData>
	{
		auto Result = CreatePrimitiveCollector()->Commit(Prepared);
		return Result.size() == Prepared.Primitives.size() && Upload(Commands, Allocator) ? Result : std::vector<FGPUScenePrimitiveData>{};
	}

	auto FGPUSceneReadParameters::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FGPUSceneReadParameters;
		static const std::array Members{MakeRDGBufferAccessMetadata<P, decltype(Buffers)>("Buffers", offsetof(P, Buffers))};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FGPUSceneReadParameters", Members);
		return &Metadata;
	}
	auto FGPUSceneReadParameters::Add(FRDGBuilder& Graph, const FResolvedStaticMeshView& View) -> void
	{
		std::array<const FRHIBuffer*, 25> SeenBuffers{};
		size_t NumSeenBuffers = 0;
		auto AddBuffer = [&](const FBufferRHIRef& Resource) {
			if (!Resource) return;
			const auto Seen = std::span(SeenBuffers).first(NumSeenBuffers);
			if (std::ranges::find(Seen, Resource.GetReference()) != Seen.end()) return;
			const auto Existing = Graph.FindExternalBuffer(Resource.GetReference());
			// An existing pass declaration does not prove the graph's external
			// entrance contract. Validate once per native resource in this Add,
			// including manually prepopulated declarations; only the name is lazy.
			const auto Name = Existing.IsValid() ? std::string{} : std::format("GPUScene.Read.{}", reinterpret_cast<uintptr_t>(Resource.GetReference()));
			const auto Handle = Graph.RegisterExternalBuffer(Resource, Name,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			if (NumSeenBuffers < SeenBuffers.size()) SeenBuffers[NumSeenBuffers++] = Resource.GetReference();
			if (std::ranges::any_of(Buffers, [&](const auto& Item) {
				return Item && Item->Buffer == Handle && Item->Offset == 0 && Item->Size == Resource->GetSize()
					&& Item->Use == ERDGUse::Read && Item->Access == ERHIAccess::GraphicsShaderRead && !Item->bDiscard;
			})) return;
			const auto Free = std::ranges::find_if(Buffers, [](const auto& Item) { return !Item; });
			require(Free != Buffers.end());
			*Free = FRDGBufferAccess{Handle, 0, Resource->GetSize(), ERDGUse::Read, ERHIAccess::GraphicsShaderRead};
		};
		AddBuffer(View.DynamicInstancingInstanceIds);
		std::array<const FGPUSceneBufferSet*, 5> SeenSets{};
		size_t NumSeenSets = 0;
		for (const auto& Primitive : View.GPUScenePrimitives)
		{
			if (!Primitive.Buffers) continue;
			const auto* Identity = Primitive.Buffers.get();
			const auto Seen = std::span(SeenSets).first(NumSeenSets);
			if (std::ranges::find(Seen, Identity) != Seen.end()) continue;
			if (NumSeenSets < SeenSets.size()) SeenSets[NumSeenSets++] = Identity;
			const auto& Set = *Primitive.Buffers;
			const std::array Resources{Set.SceneData[0], Set.SceneData[1], Set.SceneData[2], Set.InstanceIds};
			for (const auto& Resource : Resources) AddBuffer(Resource);
		}
	}
}
