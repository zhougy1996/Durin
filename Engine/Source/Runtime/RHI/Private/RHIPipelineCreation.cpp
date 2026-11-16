#include "RHIPipelineCreation.h"
#include "Threading/Task.h"
#include "Threading/Private/TaskSchedulerLifetime.h"
#include "Backend/RHICreation.h"
#include "PipelineCompileQueue.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	FRHIPipelineMetadataBudget::FRHIPipelineMetadataBudget(uint64 InCapacity)
		: Used(std::make_shared<std::atomic<uint64>>(0))
		, Capacity(InCapacity)
	{
	}
	auto FRHIPipelineMetadataBudget::Reserve(uint64 Bytes) -> std::shared_ptr<void>
	{
		auto Current = Used->load();
		do
		{
			if (Bytes > Capacity - Current) throw FRHIResourceCapacityError("pipeline metadata budget exhausted");
		} while (!Used->compare_exchange_weak(Current, Current + Bytes));
		// On control-block allocation failure, shared_ptr also invokes this deleter.
		return std::shared_ptr<void>(nullptr, [Budget = Used, Bytes](void*) { Budget->fetch_sub(Bytes); });
	}
	namespace
	{
		constexpr uint32 MaxRequests = 256;
		constexpr uint64 MaxDescriptionBytes = 1024 * 1024;
		constexpr uint64 MaxPendingBytes = 16 * 1024 * 1024;
		thread_local bool GInPipelineCreator = false;

		struct FRequestLifetime
		{
			std::atomic<bool> Closed = false;
			FRHIPipelineMetadataBudget Metadata;
		};

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
				if (Bytes > MaxDescriptionBytes || Count > (MaxDescriptionBytes - Bytes) / Stride)
					Bytes = MaxDescriptionBytes + 1;
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

	auto IsPipelineCreationPayloadBounded(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view Name) -> bool { return DescriptionBytes(Initializer, Name) <= MaxDescriptionBytes; }
	auto IsPipelineCreationPayloadBounded(const FComputePipelineStateInitializer& Initializer,
		std::string_view Name) -> bool { return DescriptionBytes(Initializer, Name) <= MaxDescriptionBytes; }

	// One shared compilation per key. The queue retains pending work even when
	// every caller drops its handle; completion events never retain native PSOs.
	struct FRHIPipelineCreationRequest::FState
	{
		FState(std::shared_ptr<FRequestLifetime> InLifetime, std::shared_ptr<const FKey> InIdentity)
			: Lifetime(std::move(InLifetime)), Identity(std::move(InIdentity)) {}
		~FState() { Completion.Trigger(); } // Also settles event construction rollback.
		auto Publish(FRHIPipelineCreationResult InResult) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				if (Outcome->load() != ERHIPipelineRequestState::Pending) return;
				Result = Lifetime->Closed.load()
					? FRHIPipelineCreationResult{.State = ERHIPipelineRequestState::Canceled} : std::move(InResult);
				Outcome->store(Result.State, std::memory_order_release);
			}
			Changed.notify_all();
			Completion.Trigger();
		}
		auto Retire() -> void
		{
			std::lock_guard Lock(Mutex);
			Result = {.State = ERHIPipelineRequestState::Canceled};
			Outcome->store(Result.State, std::memory_order_release);
		}
		std::shared_ptr<FRequestLifetime> Lifetime;
		const std::shared_ptr<const FKey> Identity;
		Tasks::FTaskEvent Completion{"RHI.PipelineCompilation"};
		std::shared_ptr<std::atomic<ERHIPipelineRequestState>> Outcome = std::make_shared<std::atomic<ERHIPipelineRequestState>>(ERHIPipelineRequestState::Pending);
		std::atomic<EPSOPrecachePriority> Priority = EPSOPrecachePriority::Medium;
		mutable std::mutex Mutex;
		std::condition_variable Changed;
		FRHIPipelineCreationResult Result;
	};

	auto FRHIPipelineCreationRequest::Rejected(ERHIPipelineRequestRejection Reason) -> FRHIPipelineCreationRequest
	{
		FRHIPipelineCreationRequest Result;
		Result.Rejection = Reason;
		return Result;
	}
	auto FRHIPipelineCreationRequest::GetStateObservation() const -> std::shared_ptr<const std::atomic<ERHIPipelineRequestState>>
	{ return State ? State->Outcome : nullptr; }
	auto FRHIPipelineCreationRequest::GetState() const -> ERHIPipelineRequestState
	{
		if (!State) return ERHIPipelineRequestState::Failed;
		return State->Outcome->load(std::memory_order_acquire);
	}
	auto FRHIPipelineCreationRequest::GetResult() const -> FRHIPipelineCreationResult
	{
		if (!State) return {.State = ERHIPipelineRequestState::Failed};
		std::lock_guard Lock(State->Mutex);
		return State->Result;
	}
	auto FRHIPipelineCreationRequest::GetTaskHandle() const -> Durin::Tasks::FTask
	{
		return State ? Durin::Tasks::FTask(State->Completion) : Durin::Tasks::FTask{};
	}
	auto FRHIPipelineCreationRequest::IsCompute() const -> bool
	{
		return State && State->Identity && State->Identity->index() == 1;
	}
	auto FRHIPipelineCreationRequest::GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc>
	{
		if (!State || !State->Identity) return {};
		return std::visit([](const auto& Key) { return Key.PipelineLayout; }, *State->Identity);
	}
	auto FRHIPipelineCreationRequest::BoostPriority(EPSOPrecachePriority Priority) const -> void
	{
		if (!State || Priority >= EPSOPrecachePriority::Num) return;
		auto Current = State->Priority.load();
		while (Current < Priority && !State->Priority.compare_exchange_weak(Current, Priority)) {}
	}
	auto FRHIPipelineCreationRequest::GetPriority() const -> EPSOPrecachePriority
	{ return State ? State->Priority.load() : EPSOPrecachePriority::NotSet; }
	auto FRHIPipelineCreationRequest::CanWait() const -> bool
	{
		return GetState() != ERHIPipelineRequestState::Pending
			|| !(GInPipelineCreator || IsExecutingRHICommands() || (GRHIThread && IsInRHIThread()) || IsInWorkerThread());
	}
	auto FRHIPipelineCreationRequest::Wait() const -> bool
	{
		if (!State) return false;
		if (GetState() != ERHIPipelineRequestState::Pending) return GetState() == ERHIPipelineRequestState::Ready;
		if (!CanWait()) return false;
		// This domain guarantees worker-only creation with no replay/game-thread dependency.
		// Core external completions intentionally carry unknown execution requirements.
		std::unique_lock Lock(State->Mutex);
		State->Changed.wait(Lock, [&] { return GetState() != ERHIPipelineRequestState::Pending; });
		return GetState() == ERHIPipelineRequestState::Ready;
	}

	struct FPipelineCompileQueue::FState
	{
		using FCompilation = FRHIPipelineCreationRequest::FState;
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

		auto Reject(ERHIPipelineRequestRejection Reason) -> FRHIPipelineCreationRequest
		{
			++Statistics.RejectedRequests;
			return FRHIPipelineCreationRequest::Rejected(Reason);
		}
		auto ReserveInputMetadata(uint64 Bytes) -> std::shared_ptr<void>
		{ return Backend.ReserveMetadata ? Backend.ReserveMetadata(Bytes) : Lifetime->Metadata.Reserve(Bytes); }
		auto RetainIdentity(std::shared_ptr<const FKey> Key, uint64 Bytes) -> std::shared_ptr<const FKey>
		{
			std::shared_ptr<void> Lease;
			if (Backend.ReserveMetadata) Lease = Backend.ReserveMetadata(Bytes);
			else Lease = Lifetime->Metadata.Reserve(Bytes);
			struct FIdentityOwner { std::shared_ptr<const FKey> Key; std::shared_ptr<void> Lease; };
			auto Owner = std::make_shared<FIdentityOwner>(Key, std::move(Lease));
			return {std::move(Owner), Key.get()};
		}
		template<typename T, typename TKeyFactory>
		auto Request(const T& Initializer, std::string_view Name, TKeyFactory&& MakeKey) -> FRHIPipelineCreationRequest
		{
			std::lock_guard Lock(Mutex);
			if (Lifetime->Closed.load()) return Reject(ERHIPipelineRequestRejection::Closed);
			if (!IsTaskSchedulerRunning()) return Reject(ERHIPipelineRequestRejection::Unsupported);
			const auto CurrentScheduler = Private::GetTaskSchedulerLifetime();
			if (SchedulerLifetime.owner_before(CurrentScheduler) || CurrentScheduler.owner_before(SchedulerLifetime))
				return Reject(ERHIPipelineRequestRejection::Unsupported);
			// Independent CPU tasks cannot introduce device work into their graph.
			if (Private::FTaskRuntimeAccess::IsExecutingIndependentCPU())
				return Reject(ERHIPipelineRequestRejection::Unsupported);
			const uint64 Bytes = DescriptionBytes(Initializer, Name);
			if (Bytes > MaxDescriptionBytes) return Reject(ERHIPipelineRequestRejection::CapacityExceeded);
			const uint64 RetainedBytes = Bytes + sizeof(FEntry) + sizeof(FKey);
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			try
			{
				auto Valid = MakeKey();
				if (!Valid)
				{
					if constexpr (requires { Valid.error(); })
						if (Valid.error() == std::remove_cvref_t<decltype(Valid.error())>::CapacityExceeded)
							return Reject(ERHIPipelineRequestRejection::CapacityExceeded);
					return Reject(ERHIPipelineRequestRejection::InvalidDescription);
				}
				auto Key = std::make_shared<const FKey>(std::move(*Valid));
				const auto KeyHash = FKeyHash{}(Key);
				const auto [Begin, End] = Compilations.equal_range(KeyHash);
				for (auto It = Begin; It != End; ++It)
					if (auto Shared = It->second.lock(); Shared && *Shared->Identity == *Key)
					{
						if (Shared->Outcome->load() == ERHIPipelineRequestState::Pending) ++Statistics.SharedPendingHits;
						FRHIPipelineCreationRequest Result;
						Result.State = std::move(Shared);
						Result.Rejection = ERHIPipelineRequestRejection::None;
						return Result;
					}
				// Only cold acquisition scans weak cache entries; duplicate handles do
				// not consume new states, events, or admission slots.
				std::erase_if(Compilations, [](const auto& Item) { return Item.second.expired(); });
				FRHIPipelineCreationResult Ready;
				if constexpr (Graphics) Ready.Graphics = Backend.FindGraphics(std::get<FGraphicsPipelineStateKey>(*Key));
				else Ready.Compute = Backend.FindCompute(std::get<FComputePipelineStateKey>(*Key));
				const bool bReady = Ready.Graphics || Ready.Compute;
				if (!bReady && (Statistics.UnfinishedRequests >= MaxRequests || RetainedBytes > MaxPendingBytes - Statistics.UnfinishedDescriptionBytes))
					return Reject(ERHIPipelineRequestRejection::CapacityExceeded);
				const auto IdentityBytes = std::visit([](const auto& Value) { return GetPipelineKeyOwnedBytes(Value); }, *Key);
				Key = RetainIdentity(std::move(Key), IdentityBytes + 256);
				FRHIPipelineCreationRequest Result;
				Result.State = std::make_shared<FCompilation>(Lifetime, Key);
				Result.Rejection = ERHIPipelineRequestRejection::None;
				if (bReady)
				{
					Ready.State = ERHIPipelineRequestState::Ready;
					Result.State->Publish(std::move(Ready));
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
				Result.Rejection = ERHIPipelineRequestRejection::None;
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
			catch (const FRHIResourceCapacityError&) { return Reject(ERHIPipelineRequestRejection::CapacityExceeded); }
		}
		auto CompleteEntry(const std::shared_ptr<FEntry>& Entry, FRHIPipelineCreationResult Result) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				Statistics.UnfinishedDescriptionBytes -= Entry->Bytes;
				--Statistics.UnfinishedRequests;
			}
			Entry->Compilation->Publish(std::move(Result));
		}
		auto AbandonDrain() -> void
		{
			std::deque<std::shared_ptr<FEntry>> Abandoned;
			{
				std::lock_guard Lock(Mutex);
				Lifetime->Closed.store(true);
				Abandoned.swap(Queue);
				Statistics.UnfinishedRequests = 0;
				Statistics.UnfinishedDescriptionBytes = 0;
				DrainActive = false;
			}
			for (const auto& Entry : Abandoned) Entry->Compilation->Publish({.State = ERHIPipelineRequestState::Canceled});
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
				FRHIPipelineCreationResult Result;
				if (Entry->Compilation->Identity->index() == 0)
					Result.Graphics = Backend.CreateGraphics(*std::get<0>(Entry->Inputs), std::get<0>(*Entry->Compilation->Identity));
				else Result.Compute = Backend.CreateCompute(*std::get<1>(Entry->Inputs), std::get<1>(*Entry->Compilation->Identity));
				Result.State = Result.Graphics || Result.Compute ? ERHIPipelineRequestState::Ready : ERHIPipelineRequestState::Failed;
				CompleteEntry(Entry, std::move(Result));
			}
		}

		FRHICapabilities Capabilities;
		FBackend Backend;
		std::shared_ptr<FRequestLifetime> Lifetime = std::make_shared<FRequestLifetime>();
		std::weak_ptr<const void> SchedulerLifetime = Private::GetTaskSchedulerLifetime();
		Tasks::TTask<void> LastDrain;
		mutable std::mutex Mutex;
		// Shared compilation cache, not a per-caller registry. Weak entries retain
		// no descriptors or native results; hash matches also compare the full key.
		// Device exit retires results held by surviving shared states.
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
	{ return State->Backend.ReserveMetadata ? State->Backend.ReserveMetadata(Bytes) : State->Lifetime->Metadata.Reserve(Bytes); }
	FPipelineCompileQueue::~FPipelineCompileQueue() { ReleaseResources(); }
	auto FPipelineCompileQueue::IsClosed() const -> bool
	{
		return State->Lifetime->Closed.load();
	}
	auto FPipelineCompileQueue::RequestGraphicsBatch(std::span<const FRHIGraphicsPipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		if (Items.size() > MaxRequests) return {.Rejection = ERHIPipelineRequestRejection::CapacityExceeded};
		FRHIPipelineCreationBatch Result;
		Result.Items.reserve(Items.size());
		for (const auto& Item : Items) Result.Items.push_back(RequestGraphics(Item.Initializer, Item.DebugName));
		return Result;
	}
	auto FPipelineCompileQueue::RequestComputeBatch(std::span<const FRHIComputePipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		if (Items.size() > MaxRequests) return {.Rejection = ERHIPipelineRequestRejection::CapacityExceeded};
		FRHIPipelineCreationBatch Result;
		Result.Items.reserve(Items.size());
		for (const auto& Item : Items) Result.Items.push_back(RequestCompute(Item.Initializer, Item.DebugName));
		return Result;
	}
	auto FPipelineCompileQueue::RequestGraphics(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view Name) -> FRHIPipelineCreationRequest
	{
		return State->Request(Initializer, Name, [&] {
			return BuildGraphicsPipelineStateKey(Initializer, &State->Capabilities);
		});
	}
	auto FPipelineCompileQueue::RequestCompute(const FComputePipelineStateInitializer& Initializer,
		std::string_view Name) -> FRHIPipelineCreationRequest
	{
		return State->Request(Initializer, Name, [&] {
			return BuildComputePipelineStateKey(Initializer, &State->Capabilities);
		});
	}
	auto FPipelineCompileQueue::RequestValidated(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view Name, const FGraphicsPipelineStateKey& Key) -> FRHIPipelineCreationRequest
	{
		return State->Request(Initializer, Name, [&] { return std::optional{Key}; });
	}
	auto FPipelineCompileQueue::RequestValidated(const FComputePipelineStateInitializer& Initializer,
		std::string_view Name, const FComputePipelineStateKey& Key) -> FRHIPipelineCreationRequest
	{
		return State->Request(Initializer, Name, [&] { return std::optional{Key}; });
	}
	auto FPipelineCompileQueue::GetStatistics() const -> FRHIPipelineCreationStatistics
	{
		std::lock_guard Lock(State->Mutex);
		return State->Statistics;
	}
	auto FPipelineCompileQueue::StopAndWait() -> void
	{
		std::lock_guard CloseLock(State->CloseMutex);
		if (State->WorkJoined) return;
		std::deque<std::shared_ptr<FState::FEntry>> Abandoned;
		{
			std::lock_guard Lock(State->Mutex);
			State->Lifetime->Closed.store(true);
			Abandoned.swap(State->Queue);
			for (const auto& Entry : Abandoned)
			{
				--State->Statistics.UnfinishedRequests;
				State->Statistics.UnfinishedDescriptionBytes -= Entry->Bytes;
			}
		}
		for (const auto& Entry : Abandoned) Entry->Compilation->Publish({.State = ERHIPipelineRequestState::Canceled});
		Abandoned.clear();
		if (State->LastDrain.IsValid())
		{
			const bool bJoined = Tasks::Wait(State->LastDrain);
			requiref(bJoined, "Pipeline creation work could not join.");
		}
		State->WorkJoined = true;
	}
	auto FPipelineCompileQueue::ReleaseResources() -> void
	{
		StopAndWait();
		std::lock_guard CloseLock(State->CloseMutex);
		for (auto& [Key, Weak] : State->Compilations)
			if (auto Compilation = Weak.lock()) Compilation->Retire();
		State->Compilations.clear();
	}
}
