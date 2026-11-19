#include "RHIPipelineCreation.h"
#include "Threading/Task.h"
#include "Backend/RHICreation.h"
#include "PipelineCompileQueue.h"
#include "Backend/RHIPipelineStateCacheBackend.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	FRHIPipelineMetadataTracker::FRHIPipelineMetadataTracker()
		: Used(std::make_shared<std::atomic<uint64>>(0))
	{
	}
	auto FRHIPipelineMetadataTracker::Reserve(uint64 Bytes) -> std::shared_ptr<void>
	{
		Used->fetch_add(Bytes);
		// On control-block allocation failure, shared_ptr also invokes this deleter.
		return std::shared_ptr<void>(nullptr, [Counter = Used, Bytes](void*) { Counter->fetch_sub(Bytes); });
	}
	namespace
	{
		thread_local bool GInPipelineCreator = false;

		using FKey = std::variant<FGraphicsPipelineStateKey, FComputePipelineStateKey>;
		struct FKeyHash
		{
			auto operator()(const std::shared_ptr<const FKey>& Key) const -> size_t
			{
				return std::visit([](const auto& Value) -> size_t {
					if constexpr (std::same_as<std::decay_t<decltype(Value)>, FGraphicsPipelineStateKey>)
						return FGraphicsPipelineStateKeyHasher{}(Value);
					else return FComputePipelineStateKeyHasher{}(Value);
				}, *Key) ^ Key->index();
			}
		};

		template<typename T>
		auto DescriptionBytes(const T& Initializer, std::string_view Name) -> uint64
		{
			uint64 Bytes = sizeof(T);
			const auto Add = [&](uint64 Count, uint64 Stride) {
				if (Count > (std::numeric_limits<uint64>::max() - Bytes) / Stride)
					RHITerminateOnOutOfMemory("RHI pipeline description size overflow");
				else Bytes += Count * Stride;
			};
			Add(Name.size(), 1);
			Add(Initializer.PipelineLayout.BindingLayouts.size(), sizeof(FBindingLayout));
			Add(Initializer.PipelineLayout.PushConstantRanges.size(), sizeof(FPushConstantRange));
			for (const auto& Layout : Initializer.PipelineLayout.BindingLayouts)
				Add(Layout.BindingLayouts.size(), sizeof(FBindingLayoutItem));
			return Bytes;
		}
	}

	// One shared compilation per key. The queue retains pending work even when
	// every caller drops its handle; completion events never retain native PSOs.
	struct FPipelineCompilation::FState
	{
		explicit FState(std::shared_ptr<const FKey> InIdentity)
			: Identity(std::move(InIdentity)) {}
		~FState() { Completion.Trigger(); } // Also settles event construction rollback.
		template<typename T> auto Publish(T Native, const std::atomic<bool>& QueueClosed) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				if (Outcome->load() != ERHIPipelineState::Pending) return;
				const auto Next = QueueClosed.load() ? ERHIPipelineState::Canceled
					: Native ? ERHIPipelineState::Ready : ERHIPipelineState::Failed;
				if (Next == ERHIPipelineState::Ready) Pipeline = std::move(Native);
				Outcome->store(Next, std::memory_order_release);
			}
			Changed.notify_all();
			Completion.Trigger();
		}
		auto Cancel() -> void
		{
			{
				std::lock_guard Lock(Mutex);
				if (Outcome->load() != ERHIPipelineState::Pending) return;
				Outcome->store(ERHIPipelineState::Canceled, std::memory_order_release);
			}
			Changed.notify_all();
			Completion.Trigger();
		}
		const std::shared_ptr<const FKey> Identity;
		Tasks::FTaskEvent Completion{"RHI.PipelineCompilation"};
		std::shared_ptr<std::atomic<ERHIPipelineState>> Outcome = std::make_shared<std::atomic<ERHIPipelineState>>(ERHIPipelineState::Pending);
		std::atomic<EPSOPrecachePriority> Priority = EPSOPrecachePriority::Medium;
		mutable std::mutex Mutex;
		std::condition_variable Changed;
		std::variant<FGraphicsPipelineStateRHIRef, FComputePipelineStateRHIRef> Pipeline;
	};

	auto FRHIPipelineStateObservation::GetState() const -> ERHIPipelineState
	{
		require(Outcome);
		return Outcome->load(std::memory_order_acquire);
	}
	auto FPipelineCompilation::GetStateObservation() const -> FRHIPipelineStateObservation
	{ return State ? FRHIPipelineStateObservation(State->Outcome) : FRHIPipelineStateObservation{}; }
	auto FPipelineCompilation::GetState() const -> ERHIPipelineState
	{
		require(State);
		return State->Outcome->load(std::memory_order_acquire);
	}
	auto FPipelineCompilation::GetGraphicsPipeline() const -> FGraphicsPipelineStateRHIRef
	{
		require(State);
		std::lock_guard Lock(State->Mutex);
		const auto* Native = std::get_if<FGraphicsPipelineStateRHIRef>(&State->Pipeline);
		return Native ? *Native : FGraphicsPipelineStateRHIRef{};
	}
	auto FPipelineCompilation::GetComputePipeline() const -> FComputePipelineStateRHIRef
	{
		require(State);
		std::lock_guard Lock(State->Mutex);
		const auto* Native = std::get_if<FComputePipelineStateRHIRef>(&State->Pipeline);
		return Native ? *Native : FComputePipelineStateRHIRef{};
	}
	auto FPipelineCompilation::GetTaskHandle() const -> Durin::Tasks::FTask
	{
		return State ? Durin::Tasks::FTask(State->Completion) : Durin::Tasks::FTask{};
	}
	auto FPipelineCompilation::IsCompute() const -> bool
	{
		return State && State->Identity && State->Identity->index() == 1;
	}
	auto FPipelineCompilation::GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc>
	{
		if (!State || !State->Identity) return {};
		return std::visit([](const auto& Key) { return Key.PipelineLayout; }, *State->Identity);
	}
	auto FPipelineCompilation::BoostPriority(EPSOPrecachePriority Priority) const -> void
	{
		if (!State || Priority >= EPSOPrecachePriority::Num) return;
		auto Current = State->Priority.load();
		while (Current < Priority && !State->Priority.compare_exchange_weak(Current, Priority)) {}
	}
	auto FPipelineCompilation::GetPriority() const -> EPSOPrecachePriority
	{ return State ? State->Priority.load() : EPSOPrecachePriority::NotSet; }
	auto FPipelineCompilation::CanWait() const -> bool
	{
		return GetState() != ERHIPipelineState::Pending
			|| !(GInPipelineCreator || IsExecutingRHICommands() || (GRHIThread && IsInRHIThread()) || IsInWorkerThread());
	}
	auto FPipelineCompilation::Wait() const -> ERHIPipelineState
	{
		require(State);
		if (!CanWait()) return ERHIPipelineState::Pending;
		std::unique_lock Lock(State->Mutex);
		State->Changed.wait(Lock, [&] { return GetState() != ERHIPipelineState::Pending; });
		return GetState();
	}

	struct FPipelineCompileQueue::FState
	{
		using FCompilation = FPipelineCompilation::FState;
		struct FEntry
		{
			std::shared_ptr<FCompilation> Compilation;
			std::variant<std::shared_ptr<const FRHIGraphicsPipelineCreationInputs>,
				std::shared_ptr<const FRHIComputePipelineCreationInputs>> Inputs;
			uint64 Bytes = 0;
			std::shared_ptr<void> Metadata;
		};
		struct FDrainLifetime
		{
			explicit FDrainLifetime(FState& InOwner) : Owner(InOwner) {}
			~FDrainLifetime() { if (!Finished) Owner.AbandonDrain(); }
			FState& Owner;
			bool Finished = true;
		};
		FState(const FRHICapabilities& InCapabilities, FBackend InBackend)
			: Capabilities(InCapabilities), Backend(std::move(InBackend)) {}

		auto ReserveInputMetadata(uint64 Bytes) -> std::shared_ptr<void>
		{ return Backend.ReserveMetadata ? Backend.ReserveMetadata(Bytes) : Metadata.Reserve(Bytes); }
		auto RetainIdentity(std::shared_ptr<const FKey> Key, uint64 Bytes) -> std::shared_ptr<const FKey>
		{
			std::shared_ptr<void> Lease;
			if (Backend.ReserveMetadata) Lease = Backend.ReserveMetadata(Bytes);
			else Lease = Metadata.Reserve(Bytes);
			struct FIdentityOwner { std::shared_ptr<const FKey> Key; std::shared_ptr<void> Lease; };
			auto Owner = std::make_shared<FIdentityOwner>(Key, std::move(Lease));
			return {std::move(Owner), Key.get()};
		}
		template<typename T, typename TKeyFactory>
		auto Request(const T& Initializer, std::string_view Name, TKeyFactory&& MakeKey) -> FPipelineCompilation
		{
			std::lock_guard Lock(Mutex);
			if (Closed.load()) return {};
			// Independent CPU tasks cannot introduce device work into their graph.
			if (Private::FTaskRuntimeAccess::IsExecutingIndependentCPU())
				return {};
			const uint64 Bytes = DescriptionBytes(Initializer, Name);
			const uint64 RetainedBytes = Bytes + sizeof(FEntry) + sizeof(FKey);
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			try
			{
				auto Valid = MakeKey();
				if (!Valid)
				{
					return {};
				}
				auto Key = std::make_shared<const FKey>(std::move(*Valid));
				const auto KeyHash = FKeyHash{}(Key);
				const auto [Begin, End] = Compilations.equal_range(KeyHash);
				for (auto It = Begin; It != End; ++It)
					if (auto Shared = It->second.lock(); Shared && *Shared->Identity == *Key)
					{
						if (Shared->Outcome->load() == ERHIPipelineState::Pending) ++Statistics.SharedPendingHits;
						FPipelineCompilation Result;
						Result.State = std::move(Shared);
						return Result;
					}
				// Only cold acquisition scans weak cache entries; duplicate handles do
				// not consume new states, events, or native work.
				std::erase_if(Compilations, [](const auto& Item) { return Item.second.expired(); });
				using TNative = std::conditional_t<Graphics, FGraphicsPipelineStateRHIRef, FComputePipelineStateRHIRef>;
				TNative Ready;
				if constexpr (Graphics) Ready = Backend.FindGraphics(std::get<FGraphicsPipelineStateKey>(*Key));
				else Ready = Backend.FindCompute(std::get<FComputePipelineStateKey>(*Key));
				const bool bReady = Ready != nullptr;
				const auto IdentityBytes = std::visit([](const auto& Value) { return GetPipelineKeyOwnedBytes(Value); }, *Key);
				Key = RetainIdentity(std::move(Key), IdentityBytes + 256);
				FPipelineCompilation Result;
				Result.State = std::make_shared<FCompilation>(Key);
				if (bReady)
				{
					Result.State->Publish(std::move(Ready), Closed);
					Compilations.emplace(KeyHash, Result.State);
					return Result;
				}
				auto Ticket = DrainActive ? std::shared_ptr<FDrainLifetime>{} : std::make_shared<FDrainLifetime>(*this);
				auto Entry = std::make_shared<FEntry>();
				Entry->Metadata = ReserveInputMetadata(Bytes);
				Entry->Compilation = Result.State;
				Entry->Bytes = RetainedBytes;
				if constexpr (Graphics) Entry->Inputs = std::make_shared<const FRHIGraphicsPipelineCreationInputs>(Initializer, Name);
				else Entry->Inputs = std::make_shared<const FRHIComputePipelineCreationInputs>(Initializer, Name);
				const auto Cached = Compilations.emplace(KeyHash, Result.State);
				try { Queue.push_back(Entry); }
				catch (...) { Compilations.erase(Cached); throw; }
				Statistics.UnfinishedDescriptionBytes += RetainedBytes;
				++Statistics.UnfinishedRequests;
				if (!DrainActive)
				{
					DrainActive = true;
					Ticket->Finished = false;
					// Serialize retirement as well as execution. Waiting the last drain
					// then covers every earlier callable that captured this device owner.
					const std::span Dependencies(&LastDrain, LastDrain.IsValid() ? 1 : 0);
					LastDrain = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.DebugName = "RHI.PipelineCreation"}, [this, Ticket] {
							Drain();
							Ticket->Finished = true; }, Dependencies);
				}
				return Result;
			}
			catch (const std::bad_alloc&)
			{
				RHITerminateOnOutOfMemory("RHI pipeline metadata");
			}
		}
		template<typename T> auto CompleteEntry(const std::shared_ptr<FEntry>& Entry, T Native) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				Statistics.UnfinishedDescriptionBytes -= Entry->Bytes;
				--Statistics.UnfinishedRequests;
			}
			Entry->Compilation->Publish(std::move(Native), Closed);
		}
		auto AbandonDrain() -> void
		{
			std::deque<std::shared_ptr<FEntry>> Abandoned;
			{
				std::lock_guard Lock(Mutex);
				Closed.store(true);
				Abandoned.swap(Queue);
				Statistics.UnfinishedRequests = 0;
				Statistics.UnfinishedDescriptionBytes = 0;
				DrainActive = false;
			}
			for (const auto& Entry : Abandoned) Entry->Compilation->Cancel();
		}
		auto Drain() noexcept -> void
		{
			GInPipelineCreator = true;
			struct FCreatorScope { ~FCreatorScope() { GInPipelineCreator = false; } } Scope;
			while (true)
			{
				std::shared_ptr<FEntry> Entry;
				{
					std::lock_guard Lock(Mutex);
					if (Queue.empty()) { DrainActive = false; return; }
					// max_element preserves FIFO order among equally urgent requests.
					const auto Next = std::max_element(Queue.begin(), Queue.end(), [](const auto& A, const auto& B) {
						return A->Compilation->Priority.load() < B->Compilation->Priority.load();
					});
					Entry = std::move(*Next);
					Queue.erase(Next);
					++Statistics.NativeRequests;
				}
				if (Entry->Compilation->Identity->index() == 0)
					CompleteEntry(Entry, Backend.CreateGraphics(*std::get<0>(Entry->Inputs), std::get<0>(*Entry->Compilation->Identity)));
				else CompleteEntry(Entry, Backend.CreateCompute(*std::get<1>(Entry->Inputs), std::get<1>(*Entry->Compilation->Identity)));
			}
		}

		FRHICapabilities Capabilities;
		FBackend Backend;
		std::atomic<bool> Closed = false;
		FRHIPipelineMetadataTracker Metadata;
		Tasks::TTask<void> LastDrain;
		mutable std::mutex Mutex;
		// Shared compilation cache, not a per-caller registry. Weak entries retain
		// no descriptors or native results; hash matches also compare the full key.
		std::unordered_multimap<size_t, std::weak_ptr<FCompilation>> Compilations;
		std::deque<std::shared_ptr<FEntry>> Queue;
		FRHIPipelineCreationStatistics Statistics;
		bool DrainActive = false;
		bool WorkJoined = false;
		std::mutex CloseMutex;
	};

	FPipelineCompileQueue::FPipelineCompileQueue(const FRHICapabilities& Capabilities, FBackend Backend)
		: State(std::make_unique<FState>(Capabilities, std::move(Backend)))
	{}
	auto FPipelineCompileQueue::ReserveCacheMetadata(uint64 Bytes) -> std::shared_ptr<void>
	{ return State->Backend.ReserveMetadata ? State->Backend.ReserveMetadata(Bytes) : State->Metadata.Reserve(Bytes); }
	FPipelineCompileQueue::~FPipelineCompileQueue() { Shutdown(); }
	auto FPipelineCompileQueue::IsClosed() const -> bool
	{
		return State->Closed.load();
	}
	auto FPipelineCompileQueue::RequestGraphicsBatch(std::span<const FRHIGraphicsPipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		FRHIPipelineCreationBatch Result;
		Result.Items.reserve(Items.size());
		for (const auto& Item : Items) Result.Items.push_back(RequestGraphics(Item.Initializer, Item.DebugName));
		return Result;
	}
	auto FPipelineCompileQueue::RequestComputeBatch(std::span<const FRHIComputePipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		FRHIPipelineCreationBatch Result;
		Result.Items.reserve(Items.size());
		for (const auto& Item : Items) Result.Items.push_back(RequestCompute(Item.Initializer, Item.DebugName));
		return Result;
	}
	auto FPipelineCompileQueue::RequestGraphics(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view Name) -> FPipelineCompilation
	{
		return State->Request(Initializer, Name, [&] {
			return BuildGraphicsPipelineStateKey(Initializer, &State->Capabilities);
		});
	}
	auto FPipelineCompileQueue::RequestCompute(const FComputePipelineStateInitializer& Initializer,
		std::string_view Name) -> FPipelineCompilation
	{
		return State->Request(Initializer, Name, [&] {
			return BuildComputePipelineStateKey(Initializer, &State->Capabilities);
		});
	}
	auto FPipelineCompileQueue::RequestValidated(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view Name, const FGraphicsPipelineStateKey& Key) -> FPipelineCompilation
	{
		return State->Request(Initializer, Name, [&] { return std::optional{Key}; });
	}
	auto FPipelineCompileQueue::RequestValidated(const FComputePipelineStateInitializer& Initializer,
		std::string_view Name, const FComputePipelineStateKey& Key) -> FPipelineCompilation
	{
		return State->Request(Initializer, Name, [&] { return std::optional{Key}; });
	}
	auto FPipelineCompileQueue::GetStatistics() const -> FRHIPipelineCreationStatistics
	{
		std::lock_guard Lock(State->Mutex);
		return State->Statistics;
	}
	auto FPipelineCompileQueue::Shutdown() -> void
	{
		std::lock_guard CloseLock(State->CloseMutex);
		if (State->WorkJoined) return;
		std::deque<std::shared_ptr<FState::FEntry>> Abandoned;
		{
			std::lock_guard Lock(State->Mutex);
			State->Closed.store(true);
			Abandoned.swap(State->Queue);
			for (const auto& Entry : Abandoned)
			{
				--State->Statistics.UnfinishedRequests;
				State->Statistics.UnfinishedDescriptionBytes -= Entry->Bytes;
			}
		}
		for (const auto& Entry : Abandoned) Entry->Compilation->Cancel();
		Abandoned.clear();
		if (State->LastDrain.IsValid())
		{
			const bool bJoined = Tasks::Wait(State->LastDrain);
			requiref(bJoined, "Pipeline creation work could not join.");
		}
		State->WorkJoined = true;
	}
}
