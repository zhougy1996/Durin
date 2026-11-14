#include "GPUScene.h"
#include "SceneInfo.h"
#include "Rendering/PrimitiveSceneProxy.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "DynamicRHI.h"

namespace Durin
{
	struct FGPUSceneUpdate
	{
		const FRDGBuilder* Graph = nullptr;
		FBufferRHIRef Primitive, Instance, Payload;
		FGPUSceneResourceParameters Parameters;
		uint64 ResourceGeneration = 0;
		bool bRebuilt = false;
		bool bCompleted = false;
	};

	struct FGPUScene::FState
	{
		struct FEntry
		{
			FPrimitiveSceneInfo* Info = nullptr;
			uint64 Generation = 0;
			uint8 DirtyState = 0;
			std::vector<uint32> InstanceIds;
		};
		struct FRetiredRange
		{
			uint32 Offset = 0, Count = 0;
			FRHIGPUSyncPointRef Completion;
		};
		std::vector<FEntry> Entries;
		std::vector<FPrimitiveSceneShaderData> Primitives;
		std::vector<FInstanceSceneShaderData> Instances;
		std::vector<FPrimitiveCustomData> Payload;
		std::vector<bool> PrimitiveDirty, InstanceDirty, PayloadDirty;
		std::vector<FRetiredRange> FreePrimitives, FreeInstances;
		FBufferRHIRef PrimitiveBuffer, InstanceBuffer, PayloadBuffer;
		FRHIGPUSyncPointRef LastUse;
		std::weak_ptr<FGPUSceneUpdate> Pending;
		uint64 Revision = 0, ResourceGeneration = 1;
		std::unordered_map<uint64, FGPUSceneDynamicAllocation> DynamicAllocations;
		std::vector<FGPUSceneDynamicAllocation> PendingDynamicReleases;
		std::vector<std::shared_ptr<FGPUSceneInstanceIdBuffer>> InstanceIdBuffers;
        FGPUSceneStatistics Statistics;

		auto RequireNoPendingUpdate() const -> void
		{
			const auto Active = Pending.lock();
			requiref(!Active || Active->bCompleted, "GPUScene mutation must follow graph completion or abandonment.");
		}
		static auto TakeRange(std::vector<FRetiredRange>& Free, uint32 Count) -> std::optional<uint32>
		{
			// Only completed ranges can coalesce; never lose an in-flight prerequisite.
			for (auto& Range : Free) if (Range.Completion.IsComplete()) Range.Completion = {};
			std::ranges::sort(Free, {}, &FRetiredRange::Offset);
			for (size_t Index = 1; Index < Free.size();)
			{
				auto& Previous = Free[Index - 1];
				const auto& Current = Free[Index];
				if (!Previous.Completion && !Current.Completion && Previous.Offset + Previous.Count == Current.Offset)
				{ Previous.Count += Current.Count; Free.erase(Free.begin() + Index); }
				else ++Index;
			}
			for (auto It = Free.begin(); It != Free.end(); ++It)
				if (It->Count >= Count && (!It->Completion || It->Completion.IsComplete()))
				{
					const uint32 Result = It->Offset;
					It->Offset += Count; It->Count -= Count;
					if (It->Count == 0) Free.erase(It);
					return Result;
				}
			return {};
		}
		template<typename T> static auto Store(std::vector<T>& Values, std::vector<bool>& Dirty, uint32 Index, const T& Value) -> void
		{
			if (std::ranges::equal(std::as_bytes(std::span(&Values[Index], 1)), std::as_bytes(std::span(&Value, 1)))) return;
			Values[Index] = Value; Dirty[Index] = true;
		}
		auto RetireDynamic(const FGPUSceneDynamicAllocation& Allocation) -> void
		{
			const auto Found = DynamicAllocations.find(Allocation.Id); require(Found != DynamicAllocations.end());
			const auto Range = Found->second;
			FreePrimitives.push_back({Range.PrimitiveOffset, Range.Count, LastUse});
			FreeInstances.push_back({Range.InstanceOffset, Range.Count, LastUse});
			DynamicAllocations.erase(Found);
		}
		auto DrainDynamicReleases() -> void
		{
			for (const auto& Range : PendingDynamicReleases) RetireDynamic(Range);
			PendingDynamicReleases.clear();
		}
		auto RetireInstances(FPrimitiveSceneInfo& Info) -> void
		{
			if (Info.NumInstanceSceneDataEntries == 0) return;
			FreeInstances.push_back({Info.InstanceSceneDataOffset, Info.NumInstanceSceneDataEntries, LastUse});
			for (uint32 Index = Info.InstanceSceneDataOffset; Index < Info.InstanceSceneDataOffset + Info.NumInstanceSceneDataEntries; ++Index)
			{
				Store(Instances, InstanceDirty, Index, FInstanceSceneShaderData{});
				Store(Payload, PayloadDirty, Index, FPrimitiveCustomData{});
			}
			Info.InstanceSceneDataOffset = INDEX_NONE_U32;
			Info.NumInstanceSceneDataEntries = 0;
		}
		auto Refresh(FPrimitiveSceneInfo& Info, uint8 Dirty) -> void
		{
			const uint32 PrimitiveIndex = Info.GetPersistentIndex().Index;
			auto RefreshPrimitive = [&] {
				const auto Parameters = FPrimitiveUniformShaderParametersBuilder().LocalToWorld(Info.GetTransform())
					.WorldBounds(Info.GetWorldBounds()).CustomPrimitiveData(Info.GetProxy().GetCustomPrimitiveData())
					.InstanceSceneData(Info.InstanceSceneDataOffset, Info.NumInstanceSceneDataEntries).Build();
				Store(Primitives, PrimitiveDirty, PrimitiveIndex, Parameters ? FPrimitiveSceneShaderData(*Parameters) : FPrimitiveSceneShaderData{});
			};
			if (Dirty == static_cast<uint8>(EGPUSceneDirtyState::PrimitiveCustomData)) { RefreshPrimitive(); return; }
			const auto Source = Info.GetProxy().GetInstanceSceneDataBuffers();
			std::vector<FInstanceSceneDataBuffers::FInstance> Default;
			if (!Source) Default.push_back({{}, FMatrix(1.0), Info.GetLocalBounds(), {}});
			auto Sorted = Source ? Source->Instances : std::move(Default);
			std::ranges::sort(Sorted, {}, [](const auto& Item) { return Item.Id.Id; });
			if (Source && (Sorted.size() > MaximumStaticMeshRenderInstances
				|| std::ranges::any_of(Sorted, [](const auto& Item) { return !Item.Id.IsValid() || !Item.CustomData.IsValid(); })
				|| std::ranges::adjacent_find(Sorted, {}, [](const auto& Item) { return Item.Id.Id; }) != Sorted.end())) Sorted.clear();
			const uint32 Count = static_cast<uint32>(Sorted.size());
			auto& SourceIds = Entries[PrimitiveIndex].InstanceIds;
			SourceIds.clear();
			for (const auto& Item : Sorted) SourceIds.push_back(Item.Id.Id);
			if (Count != Info.NumInstanceSceneDataEntries)
			{
				RetireInstances(Info);
				if (Count != 0)
				{
					const uint32 Offset = TakeRange(FreeInstances, Count).value_or(static_cast<uint32>(Instances.size()));
					require(uint64(Offset) + Count <= UINT32_MAX / sizeof(FInstanceSceneShaderData));
					if (Offset == Instances.size())
					{
						Instances.resize(size_t(Offset) + Count); Payload.resize(Instances.size());
						InstanceDirty.resize(Instances.size(), true); PayloadDirty.resize(Instances.size(), true);
					}
					Info.InstanceSceneDataOffset = Offset;
					Info.NumInstanceSceneDataEntries = Count;
				}
			}
			RefreshPrimitive();
			for (uint32 RelativeId = 0; RelativeId < Count; ++RelativeId)
			{
				const auto& Item = Sorted[RelativeId];
				const uint32 Index = Info.InstanceSceneDataOffset + RelativeId;
				const auto Transform = Info.GetTransform() * Item.LocalToPrimitive;
				FInstanceSceneShaderData Packed;
				Packed.Build(PrimitiveIndex, RelativeId, Index * (PrimitiveCustomDataFloatCount / 4), Item.Id,
					Transform, TransformPrimitiveBounds(Item.LocalBounds, Transform));
				Store(Instances, InstanceDirty, Index, Packed);
				Store(Payload, PayloadDirty, Index, Item.CustomData);
			}
		}
	};

	FGPUScene::FGPUScene() : State(std::make_unique<FState>()) {}
	FGPUScene::~FGPUScene() = default;

	auto FGPUScene::AllocateDynamicPrimitiveRange(uint32 Count) -> FGPUSceneDynamicAllocation
	{
		CheckRenderingThread(); State->RequireNoPendingUpdate(); State->DrainDynamicReleases();
		if (Count == 0) return {};
		FGPUSceneDynamicAllocation Result;
		static std::atomic<uint64> NextAllocation{1};
		Result.Id = NextAllocation.fetch_add(1, std::memory_order_relaxed); require(Result.Id != 0);
		Result.Count = Count;
		Result.PrimitiveOffset = FState::TakeRange(State->FreePrimitives, Count).value_or(static_cast<uint32>(State->Primitives.size()));
		Result.InstanceOffset = FState::TakeRange(State->FreeInstances, Count).value_or(static_cast<uint32>(State->Instances.size()));
		require(uint64(Result.PrimitiveOffset) + Count <= UINT32_MAX / sizeof(FPrimitiveSceneShaderData));
		require(uint64(Result.InstanceOffset) + Count <= UINT32_MAX / sizeof(FInstanceSceneShaderData));
		if (uint64(Result.PrimitiveOffset) + Count > State->Primitives.size())
		{
			State->Primitives.resize(size_t(Result.PrimitiveOffset) + Count);
			State->Entries.resize(State->Primitives.size()); State->PrimitiveDirty.resize(State->Primitives.size(), true);
		}
		if (uint64(Result.InstanceOffset) + Count > State->Instances.size())
		{
			State->Instances.resize(size_t(Result.InstanceOffset) + Count); State->Payload.resize(State->Instances.size());
			State->InstanceDirty.resize(State->Instances.size(), true); State->PayloadDirty.resize(State->Instances.size(), true);
		}
		State->DynamicAllocations.emplace(Result.Id, Result); ++State->Revision;
		return Result;
	}
	auto FGPUScene::WriteDynamicPrimitive(const FGPUSceneDynamicAllocation& Allocation, uint32 Index,
		const FMatrix& Transform, const FBox& Bounds, const FPrimitiveCustomData& PrimitiveData, const FPrimitiveCustomData& InstanceData) -> bool
	{
		CheckRenderingThread(); State->RequireNoPendingUpdate();
		const auto Found = State->DynamicAllocations.find(Allocation.Id);
		if (Found == State->DynamicAllocations.end() || Index >= Found->second.Count || !InstanceData.IsValid()) return false;
		const auto& Range = Found->second;
		const auto PrimitiveId = Range.PrimitiveOffset + Index, InstanceId = Range.InstanceOffset + Index;
		const auto Parameters = FPrimitiveUniformShaderParametersBuilder().LocalToWorld(Transform).WorldBounds(Bounds)
			.CustomPrimitiveData(PrimitiveData).InstanceSceneData(InstanceId, 1).Build();
		FInstanceSceneShaderData Packed;
		if (!Parameters || !Packed.Build(PrimitiveId, 0, InstanceId * (PrimitiveCustomDataFloatCount / 4), {}, Transform, Bounds)) return false;
		FState::Store(State->Primitives, State->PrimitiveDirty, PrimitiveId, FPrimitiveSceneShaderData(*Parameters));
		FState::Store(State->Instances, State->InstanceDirty, InstanceId, Packed);
		FState::Store(State->Payload, State->PayloadDirty, InstanceId, InstanceData);
		++State->Revision;
		return true;
	}
	auto FGPUScene::ReleaseDynamicPrimitiveRange(const FGPUSceneDynamicAllocation& Allocation) -> void
	{
		CheckRenderingThread();
		if (Allocation.Id == 0) return;
		const auto Pending = State->Pending.lock();
		if (Pending && !Pending->bCompleted) State->PendingDynamicReleases.push_back(Allocation);
		else { State->DrainDynamicReleases(); State->RetireDynamic(Allocation); }
		// A release never mutates packed data captured by an in-flight upload.
		// Completion/abandonment resolves its retirement prerequisite before reuse.
	}

	auto FPrimitiveSceneInfoRef::Resolve() const -> FPrimitiveSceneInfo*
	{
		CheckRenderingThread();
		const auto Scene = GPUScene.lock();
		return Scene ? Scene->ResolvePrimitive(Handle) : nullptr;
	}
	auto FGPUScene::ResolvePrimitive(FGPUScenePrimitiveHandle Handle) const -> FPrimitiveSceneInfo*
	{
		CheckRenderingThread();
		const auto Index = Handle.PersistentPrimitiveIndex.Index;
		return Index < State->Entries.size() && State->Entries[Index].Generation == Handle.Generation ? State->Entries[Index].Info : nullptr;
	}
	auto FGPUScene::AddPrimitive(FPrimitiveSceneInfo& Info) -> void
	{
		CheckRenderingThread(); State->RequireNoPendingUpdate();
		require(!Info.GetPersistentIndex().IsValid());
		const uint32 Index = FState::TakeRange(State->FreePrimitives, 1).value_or(static_cast<uint32>(State->Entries.size()));
		if (Index == State->Entries.size())
		{
			require(uint64(Index) + 1 <= UINT32_MAX / sizeof(FPrimitiveSceneShaderData));
			State->Entries.emplace_back(); State->Primitives.emplace_back(); State->PrimitiveDirty.push_back(true);
		}
		auto& Entry = State->Entries[Index];
		Entry.Info = &Info; ++Entry.Generation; Entry.DirtyState = static_cast<uint8>(EGPUSceneDirtyState::Added);
		Info.GPUSceneHandle = {{Index}, Entry.Generation};
		++State->Revision;
	}

	auto FGPUScene::RequestUpdate(FGPUScenePrimitiveHandle Handle, EGPUSceneDirtyState Dirty) -> bool
	{
		CheckRenderingThread(); State->RequireNoPendingUpdate();
		const uint32 Index = Handle.PersistentPrimitiveIndex.Index;
		if (Index >= State->Entries.size()) return false;
		auto& Entry = State->Entries[Index];
		if (!Entry.Info || Entry.Generation != Handle.Generation || (static_cast<uint8>(Dirty) & static_cast<uint8>(EGPUSceneDirtyState::Removed))) return false;
		if (Dirty != EGPUSceneDirtyState::None) { Entry.DirtyState |= static_cast<uint8>(Dirty); ++State->Revision; }
		return true;
	}

	auto FGPUScene::RemovePrimitive(FPrimitiveSceneInfo& Info) -> void
	{
		CheckRenderingThread(); State->RequireNoPendingUpdate();
		const auto Handle = Info.GPUSceneHandle;
		if (!Handle.PersistentPrimitiveIndex.IsValid()) return;
		auto& Entry = State->Entries[Handle.PersistentPrimitiveIndex.Index];
		require(Entry.Info == &Info && Entry.Generation == Handle.Generation);
		State->RetireInstances(Info);
		FState::Store(State->Primitives, State->PrimitiveDirty, Handle.PersistentPrimitiveIndex.Index, FPrimitiveSceneShaderData{});
		State->FreePrimitives.push_back({Handle.PersistentPrimitiveIndex.Index, 1, State->LastUse});
		Entry.Info = nullptr; Entry.DirtyState = 0;
		Entry.InstanceIds.clear();
		Info.GPUSceneHandle = {};
		++State->Revision;
	}

	auto FGPUScene::UpdateCPUData() -> void
	{
		CheckRenderingThread(); State->RequireNoPendingUpdate(); State->DrainDynamicReleases();
		for (auto& Entry : State->Entries)
			if (Entry.Info && Entry.DirtyState) { State->Refresh(*Entry.Info, Entry.DirtyState); Entry.DirtyState = 0; }
	}
	auto FGPUScene::Update(FRDGBuilder& Graph) -> FGPUSceneUpdateResult
	{
		UpdateCPUData();
		if (!IsInitialized()) InitResource(FRHICommandListImmediate::Get());

		auto Transaction = std::make_shared<FGPUSceneUpdate>();
		Transaction->Graph = &Graph; Transaction->ResourceGeneration = State->ResourceGeneration;
		FGPUSceneUpdateResult Result;
		Result.Update = Transaction; Result.Revision = State->Revision;
		constexpr auto ReadAccess = ERHIAccess::GraphicsShaderRead;
		auto Upload = [&]<typename T>(const std::vector<T>& Values, const std::vector<bool>& Dirty,
			const FBufferRHIRef& Existing, FBufferRHIRef& Destination, std::string_view Name) -> FRDGBufferParameter
		{
			const uint32 RequiredCount = std::max(1u, static_cast<uint32>(Values.size()));
			const bool Rebuild = !Existing || Existing->GetSize() / sizeof(T) < RequiredCount;
			FRDGBufferHandle Buffer;
			uint32 Bytes;
			if (Rebuild)
			{
				uint64 Capacity = 1;
				while (Capacity < RequiredCount) Capacity *= 2;
				require(Capacity * sizeof(T) <= UINT32_MAX);
				Bytes = static_cast<uint32>(Capacity * sizeof(T));
				Buffer = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(Bytes, 16, EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy)}, Name, ReadAccess);
				FByteBuffer Initial(Bytes);
				const auto Source = std::as_bytes(std::span(Values));
				std::ranges::copy(Source, Initial.begin());
				const auto Pass = Graph.QueueBufferUploadOwned(Buffer, 0, std::move(Initial));
				Graph.MarkPassRoot(Pass, "GPUScene persistent upload");
				Result.UploadBytes += Bytes; ++Result.UploadRanges; ++Result.BufferRebuilds;
			}
			else
			{
				Bytes = Existing->GetSize();
				Buffer = Graph.RegisterExternalBuffer(Existing, Name, ReadAccess, ReadAccess);
				for (uint32 Begin = 0; Begin < Dirty.size();)
				{
					if (!Dirty[Begin]) { ++Begin; continue; }
					uint32 End = Begin + 1;
					while (End < Dirty.size() && Dirty[End]) ++End;
					const auto Data = std::as_bytes(std::span(Values).subspan(Begin, End - Begin));
					const auto Pass = Graph.QueueBufferUpload(Buffer, Begin * sizeof(T), Data);
					Graph.MarkPassRoot(Pass, "GPUScene persistent patch");
					Result.UploadBytes += Data.size(); ++Result.UploadRanges; Begin = End;
				}
			}
			Graph.QueueBufferExtraction(Buffer, &Destination, ReadAccess);
			return {Buffer, 0, Bytes};
		};
		Result.Parameters.GPUScenePrimitiveSceneData = Upload(State->Primitives, State->PrimitiveDirty, State->PrimitiveBuffer, Transaction->Primitive, "GPUScene.PrimitiveSceneData");
		Result.Parameters.GPUSceneInstanceSceneData = Upload(State->Instances, State->InstanceDirty, State->InstanceBuffer, Transaction->Instance, "GPUScene.InstanceSceneData");
		Result.Parameters.GPUSceneInstancePayloadData = Upload(State->Payload, State->PayloadDirty, State->PayloadBuffer, Transaction->Payload, "GPUScene.InstancePayloadData");
		Result.Parameters.GPUSceneNumAllocatedPrimitives = static_cast<uint32>(State->Primitives.size());
		Result.Parameters.GPUSceneNumAllocatedInstances = static_cast<uint32>(State->Instances.size());
		Transaction->bRebuilt = Result.BufferRebuilds != 0;
		Result.Parameters.GPUSceneResourceGeneration = State->ResourceGeneration + (Transaction->bRebuilt ? 1u : 0u);
		Transaction->Parameters = Result.Parameters;
		State->Pending = Transaction;
		return Result;
	}

	auto FGPUScene::GetShaderParameters(const FRDGBuilder& Graph) const -> FGPUSceneResourceParameters
	{
		CheckRenderingThread();
		const auto Pending = State->Pending.lock();
		require(Pending && Pending->Graph == &Graph && !Pending->bCompleted && Pending->ResourceGeneration == State->ResourceGeneration);
		return Pending->Parameters;
	}

	auto FGPUScene::CompleteUpdate(const FRDGBuilder& Graph, const FGPUSceneUpdateResult& Result) -> bool
	{
		CheckRenderingThread();
		const auto& Pending = Result.Update;
		require(Pending && State->Pending.lock() == Pending && Pending->Graph == &Graph && !Pending->bCompleted);
		Pending->bCompleted = true;
		const auto& Execution = Graph.GetExecutionResult();
		if (!Execution || !Execution->has_value() || Pending->ResourceGeneration != State->ResourceGeneration)
		{
			// Compilation failure cannot have recorded writes. Later failures may
			// have dispatched a prefix: replace native storage before retry/reuse.
			if (Execution && !Execution->has_value() && GetRDGExecutionStatus(*Execution) != ERDGExecutionStatus::CompileFailed) ReleaseRHI();
			return false;
		}
		require(Pending->Primitive && Pending->Instance && Pending->Payload);
		State->PrimitiveBuffer = Pending->Primitive; State->InstanceBuffer = Pending->Instance; State->PayloadBuffer = Pending->Payload;
		if (Pending->bRebuilt) ++State->ResourceGeneration;
		std::ranges::fill(State->PrimitiveDirty, false);
		std::ranges::fill(State->InstanceDirty, false);
		std::ranges::fill(State->PayloadDirty, false);
        State->Statistics.UploadBytes += Result.UploadBytes;
        State->Statistics.UploadRanges += Result.UploadRanges;
        State->Statistics.BufferRebuilds += Result.BufferRebuilds;
		State->LastUse = Graph.GetGPUCompletion();
		return true;
	}

    auto FGPUScene::GetStatistics() const -> FGPUSceneStatistics
    {
        CheckRenderingThread();
        auto Result = State->Statistics;
        Result.MirrorCapacityBytes = State->Primitives.capacity() * sizeof(FPrimitiveSceneShaderData)
            + State->Instances.capacity() * sizeof(FInstanceSceneShaderData) + State->Payload.capacity() * sizeof(FPrimitiveCustomData);
        for (const auto& Buffer : GetResourceBuffers()) if (Buffer) Result.CapacityBytes += Buffer->GetSize();
        for (const auto& Entry : State->InstanceIdBuffers) Result.InstanceIdCapacityBytes += Entry->Buffer->GetSize();
        return Result;
    }

	auto FGPUScene::UploadInstanceIds(FRHICommandListImmediate& Commands, std::span<const uint32> Ids)
		-> std::shared_ptr<FGPUSceneInstanceIdBuffer>
	{
		CheckRenderingThread();
		if (Ids.empty() || Ids.size() > UINT32_MAX / sizeof(uint32)) return {};
		if (!IsInitialized()) InitResource(Commands);
		const auto Hash = FXxHash64::HashBuffer(std::as_bytes(Ids));
		for (const auto& Entry : State->InstanceIdBuffers)
			if (Entry->Hash == Hash && std::ranges::equal(Entry->Ids, Ids))
			{ ++State->Statistics.InstanceIdBufferReuses; return Entry; }

		const uint64 RequiredBytes = Ids.size_bytes();
		std::shared_ptr<FGPUSceneInstanceIdBuffer> Allocation;
		for (const auto& Entry : State->InstanceIdBuffers)
			if (Entry.use_count() == 1 && Entry->LastUse.IsComplete() && Entry->Buffer->GetSize() >= RequiredBytes
				&& (!Allocation || Entry->Buffer->GetSize() < Allocation->Buffer->GetSize())) Allocation = Entry;
		const bool bReusedStorage = bool(Allocation);
		if (!Allocation)
		{
			uint64 Capacity = 256;
			while (Capacity < RequiredBytes) Capacity *= 2;
			if (Capacity > UINT32_MAX) Capacity = RequiredBytes;
			Allocation = std::make_shared<FGPUSceneInstanceIdBuffer>();
			Allocation->Buffer = GDynamicRHI->RHICreateBuffer(Commands, FRHIBufferCreateDesc::Create("GPUScene.InstanceIds",
				static_cast<uint32>(Capacity), sizeof(uint32), EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::DestinationCopy));
			if (!Allocation->Buffer) return {};
			++State->Statistics.InstanceIdBufferCreates;
			// Bound scene-owned native capacity. Outstanding/failed leases are never
			// overwritten or evicted to make room for an unrelated upload.
			constexpr uint64 MaximumRetainedBytes = 4ull * 1024 * 1024;
			uint64 RetainedBytes = 0;
			size_t RetainedCount = State->InstanceIdBuffers.size();
			for (const auto& Entry : State->InstanceIdBuffers) RetainedBytes += Entry->Buffer->GetSize();
			std::erase_if(State->InstanceIdBuffers, [&](const auto& Entry) {
				if (RetainedCount < 32 && RetainedBytes + Capacity <= MaximumRetainedBytes) return false;
				if (Entry.use_count() != 1 || !Entry->LastUse.IsComplete()) return false;
				RetainedBytes -= Entry->Buffer->GetSize();
				--RetainedCount;
				return true;
			});
			if (State->InstanceIdBuffers.size() < 32 && RetainedBytes + Capacity <= MaximumRetainedBytes)
				State->InstanceIdBuffers.push_back(Allocation);
		}
		else ++State->Statistics.InstanceIdBufferReuses;
		Allocation->LastUse = {};
		Allocation->Hash = Hash;
		Allocation->Ids.assign(Ids.begin(), Ids.end());
		const auto Buffer = Allocation->Buffer;
		const std::array Before{FRHIBufferTransition::Whole(Buffer,
			bReusedStorage ? ERHIAccess::GraphicsShaderRead : ERHIAccess::None, ERHIAccess::TransferWrite)};
		Commands.TransitionBuffers(Before);
		Commands.WriteBuffer(Buffer, 0, std::as_bytes(Ids));
		const std::array After{FRHIBufferTransition::Whole(Buffer, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead)};
		Commands.TransitionBuffers(After);
		State->Statistics.InstanceIdUploadBytes += RequiredBytes;
		return Allocation;
	}

	auto FGPUScene::ReleaseRHI() -> void
	{
		CheckRenderingThread();
		State->InstanceIdBuffers.clear();
		State->PrimitiveBuffer = {}; State->InstanceBuffer = {}; State->PayloadBuffer = {};
		State->LastUse = {}; ++State->ResourceGeneration;
		std::ranges::fill(State->PrimitiveDirty, true);
		std::ranges::fill(State->InstanceDirty, true);
		std::ranges::fill(State->PayloadDirty, true);
		for (auto& Range : State->FreePrimitives) Range.Completion = {};
		for (auto& Range : State->FreeInstances) Range.Completion = {};
	}
	auto FGPUScene::GetPrimitiveSceneData() const -> std::span<const FPrimitiveSceneShaderData> { return State->Primitives; }
	auto FGPUScene::GetInstanceSceneData() const -> std::span<const FInstanceSceneShaderData> { return State->Instances; }
	auto FGPUScene::GetInstancePayloadData() const -> std::span<const FPrimitiveCustomData> { return State->Payload; }
	auto FGPUScene::GetResourceBuffers() const -> std::array<FBufferRHIRef, 3>
	{ return {State->PrimitiveBuffer, State->InstanceBuffer, State->PayloadBuffer}; }
	auto FGPUScene::IsDirty() const -> bool
	{
		return !State->PrimitiveBuffer || std::ranges::any_of(State->Entries, [](const auto& Entry) { return Entry.DirtyState != 0; })
			|| std::ranges::find(State->PrimitiveDirty, true) != State->PrimitiveDirty.end()
			|| std::ranges::find(State->InstanceDirty, true) != State->InstanceDirty.end()
			|| std::ranges::find(State->PayloadDirty, true) != State->PayloadDirty.end();
	}
	auto FGPUScene::RegisterGraphUse(const FRDGBuilder& Graph) -> void
	{
		CheckRenderingThread();
		if (Graph.GetGPUCompletion()) State->LastUse = Graph.GetGPUCompletion();
		else if (Graph.GetExecutionResult() && !Graph.GetExecutionResult()->has_value()
			&& GetRDGExecutionStatus(*Graph.GetExecutionResult()) != ERDGExecutionStatus::CompileFailed) ReleaseRHI();
	}
	auto FGPUScene::FindInstanceId(FPersistentPrimitiveIndex Primitive, FPrimitiveInstanceId StableInstanceId) const -> std::optional<uint32>
	{
		if (!Primitive.IsValid() || Primitive.Index >= State->Entries.size()) return {};
		const auto& Entry = State->Entries[Primitive.Index];
		if (!Entry.Info) return {};
		const auto It = std::ranges::lower_bound(Entry.InstanceIds, StableInstanceId.Id);
		if (It == Entry.InstanceIds.end() || *It != StableInstanceId.Id) return {};
		return Entry.Info->InstanceSceneDataOffset + static_cast<uint32>(It - Entry.InstanceIds.begin());
	}
}
