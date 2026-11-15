#include "Renderers/GPUSceneRendering.h"
#include "SceneInfo.h"
#include "GPUScene.h"
#include "PrimitiveSceneShaderData.h"
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
	auto FGPUScenePrimitiveCollector::Add(std::shared_ptr<const FGPUSceneDynamicPrimitiveData> MeshBatchData, uint32 NumInstances) -> uint32
	{
		require(!bCommitted && Context && MeshBatchData && NumInstances && Primitives.size() < UINT32_MAX);
		Primitives.push_back({.DynamicData = std::move(MeshBatchData), .NumInstances = NumInstances});
		return static_cast<uint32>(Primitives.size() - 1);
	}
	auto FGPUScenePrimitiveCollector::Add(FPersistentPrimitiveIndex PrimitiveIndex, FPrimitiveInstanceId InstanceId, uint32 NumInstances) -> uint32
	{
		require(!bCommitted && Context && Context->IsUsingPersistentGPUScene() && PrimitiveIndex.IsValid()
			&& NumInstances && Primitives.size() < UINT32_MAX);
		CheckRenderingThread();
		const auto* Info = Context->GPUScene->GetPrimitiveSceneInfo(PrimitiveIndex);
		Primitives.push_back({.Source = {Context->GPUScene, Info ? Info->GPUSceneHandle : FGPUScenePrimitiveHandle{}},
			.InstanceId = InstanceId, .NumInstances = NumInstances});
		return static_cast<uint32>(Primitives.size() - 1);
	}
	auto FGPUScenePrimitiveCollector::Commit() -> bool
	{
		require(!bCommitted && Context); bCommitted = true;
		return Context->CommitPrimitiveCollector(*this);
	}
	auto FGPUScenePrimitiveCollector::GetPrimitiveData(uint32 PrimitiveIndex) const -> const FGPUScenePrimitiveData&
	{
		require(bCommitted && PrimitiveIndex < Results.size());
		return Results[PrimitiveIndex];
	}
	auto FGPUSceneDynamicContext::CommitPrimitiveCollector(FGPUScenePrimitiveCollector& Collector) -> bool
	{
		CheckRenderingThread();
		if (Collector.Primitives.empty()) return true;
		GPUScene->UpdateCPUData();
		std::vector<uint32> ResolvedIds(Collector.Primitives.size());
		std::vector<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>> NewRecords;
		std::set<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>, std::owner_less<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>>> NewIdentities;
		uint64 NumIds = InstanceIds.size();
		for (size_t Index = 0; Index < Collector.Primitives.size(); ++Index)
		{
			const auto& Primitive = Collector.Primitives[Index];
			NumIds += Primitive.NumInstances;
			if (NumIds > UINT32_MAX / sizeof(uint32)) return false;
			if (Primitive.DynamicData)
			{
				if (!DynamicInstances.contains(Primitive.DynamicData) && NewIdentities.insert(Primitive.DynamicData).second)
					NewRecords.push_back(Primitive.DynamicData);
			}
			else
			{
				if (Primitive.Source.GPUScene.lock() != GPUScene || !Primitive.Source.Resolve()) return false;
				const auto Existing = GPUScene->FindInstanceId(Primitive.Source.Handle.PersistentPrimitiveIndex, Primitive.InstanceId);
				if (!Existing) return false;
				ResolvedIds[Index] = *Existing;
			}
		}
		const auto Allocation = GPUScene->AllocateDynamicPrimitiveRange(static_cast<uint32>(NewRecords.size()));
		if (Allocation.Count != NewRecords.size()) return false;
		if (Allocation.Count) Allocations.push_back(Allocation);
		for (uint32 Index = 0; Index < NewRecords.size(); ++Index)
		{
			const auto& Record = NewRecords[Index];
			if (!GPUScene->WriteDynamicPrimitive(Allocation, Index, Record->LocalToWorld, Record->WorldBounds,
				Record->CustomPrimitiveData, Record->CustomInstanceData)) return false;
		}
		for (uint32 Index = 0; Index < NewRecords.size(); ++Index)
			DynamicInstances.emplace(NewRecords[Index], Allocation.InstanceOffset + Index);

		auto Buffers = std::make_shared<FGPUSceneBufferSet>(); Buffers->DynamicContext = shared_from_this(); Buffers->bStandaloneScene = bStandaloneScene;
		std::vector<FGPUScenePrimitiveData> Results;
		std::vector<uint32> Ids;
		Results.reserve(Collector.Primitives.size()); Ids.reserve(static_cast<size_t>(NumIds - InstanceIds.size()));
		for (size_t Index = 0; Index < Collector.Primitives.size(); ++Index)
		{
			const auto& Primitive = Collector.Primitives[Index];
			const uint32 InstanceId = Primitive.DynamicData ? DynamicInstances.at(Primitive.DynamicData) : ResolvedIds[Index];
			Results.push_back({Buffers, InstanceId, static_cast<uint32>(InstanceIds.size() + Ids.size())});
			Ids.insert(Ids.end(), Primitive.NumInstances, InstanceId);
		}
		InstanceIds.insert(InstanceIds.end(), Ids.begin(), Ids.end());
		PendingBufferSets.push_back(Buffers);
		Collector.Results = std::move(Results);
		return true;
	}
	auto FGPUSceneDynamicContext::Upload(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator) -> bool
	{
		CheckRenderingThread();
		if (PendingBufferSets.empty()) return true;
		if (GPUScene->IsDirty())
		{
			FRDGBuilder Graph;
			const auto Update = GPUScene->Update(Graph);
			const auto Executed = Graph.Execute(Commands, &Allocator);
			if (!GPUScene->CompleteUpdate(Graph, Update) || !Executed) return false;
		}
		const auto UploadedIds = InstanceIds.empty() ? FBufferRHIRef{} : UploadInstanceIds(Commands, InstanceIds);
		if (!InstanceIds.empty() && !UploadedIds) return false;
		for (const auto& Weak : PendingBufferSets) if (const auto Buffers = Weak.lock())
		{
			Buffers->SceneData = GPUScene->GetResourceBuffers();
			Buffers->InstanceIds = UploadedIds;
			Buffers->NumAllocatedPrimitives = static_cast<uint32>(GPUScene->GetPrimitiveSceneData().size());
			Buffers->NumAllocatedInstances = static_cast<uint32>(GPUScene->GetInstanceSceneData().size());
			Buffers->ResourceGeneration = GPUScene->GetResourceGeneration();
		}
		PendingBufferSets.clear();
		InstanceIds.clear();
		return true;
	}
	auto FGPUSceneResourceParameters::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FGPUSceneResourceParameters;
		static const std::array Members{
			MakeRDGResourceParameterMemberMetadata<P, decltype(GPUScenePrimitiveSceneData), FRDGBufferParameter>("GPUScenePrimitiveSceneData", offsetof(P, GPUScenePrimitiveSceneData), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::GraphicsShaderRead),
			MakeRDGResourceParameterMemberMetadata<P, decltype(GPUSceneInstanceSceneData), FRDGBufferParameter>("GPUSceneInstanceSceneData", offsetof(P, GPUSceneInstanceSceneData), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::GraphicsShaderRead),
			MakeRDGResourceParameterMemberMetadata<P, decltype(GPUSceneInstancePayloadData), FRDGBufferParameter>("GPUSceneInstancePayloadData", offsetof(P, GPUSceneInstancePayloadData), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::GraphicsShaderRead)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FGPUSceneResourceParameters", Members);
		return &Metadata;
	}
	auto FGPUSceneReadParameters::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FGPUSceneReadParameters;
		static const std::array Members{
			MakeRDGNestedParameterMemberMetadata<P, decltype(GPUScene)>("GPUScene", offsetof(P, GPUScene), FGPUSceneResourceParameters::GetRDGParametersMetadata()),
			MakeRDGBufferAccessMetadata<P, decltype(InstanceIds)>("InstanceIds", offsetof(P, InstanceIds)),
			MakeRDGBufferAccessMetadata<P, decltype(DynamicInstanceIds)>("DynamicInstanceIds", offsetof(P, DynamicInstanceIds))};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FGPUSceneReadParameters", Members);
		return &Metadata;
	}
	auto FGPUSceneBufferSet::GetShaderParameters(FRDGBuilder& Graph, const FBufferRHIRef& DynamicIds) const -> FGPUSceneReadParameters
	{
		auto Import = [&](const FBufferRHIRef& Buffer) -> std::optional<FRDGBufferParameter> {
			if (!Buffer) return {};
			const auto Existing = Graph.FindExternalBuffer(Buffer);
			const auto Name = Existing.IsValid() ? std::string{} : std::format("GPUScene.Read.{}", reinterpret_cast<uintptr_t>(Buffer.GetReference()));
			// Registration still validates the external entrance/final contract on every import.
			const auto Handle = Graph.RegisterExternalBuffer(Buffer, Name, ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			return FRDGBufferParameter{Handle, 0, Buffer->GetSize()};
		};
		auto Read = [&](const FBufferRHIRef& Buffer) -> std::optional<FRDGBufferAccess> {
			const auto Parameter = Import(Buffer);
			return Parameter ? std::optional(FRDGBufferAccess{Parameter->Buffer, 0, Parameter->Size, ERDGUse::Read, ERHIAccess::GraphicsShaderRead}) : std::nullopt;
		};
		FGPUSceneReadParameters Result;
		Result.GPUScene.GPUSceneNumAllocatedPrimitives = NumAllocatedPrimitives;
		Result.GPUScene.GPUSceneNumAllocatedInstances = NumAllocatedInstances;
		Result.GPUScene.GPUSceneResourceGeneration = ResourceGeneration;
		Result.GPUScene.GPUScenePrimitiveSceneData = Import(SceneData[0]);
		Result.GPUScene.GPUSceneInstanceSceneData = Import(SceneData[1]);
		Result.GPUScene.GPUSceneInstancePayloadData = Import(SceneData[2]);
		Result.InstanceIds = Read(InstanceIds);
		if (DynamicIds != InstanceIds) Result.DynamicInstanceIds = Read(DynamicIds);
		return Result;
	}
}
