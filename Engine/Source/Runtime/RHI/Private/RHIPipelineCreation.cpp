#include "RHIPipelineCreation.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "Threading/AtomicSharedPtr.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	FRHIPipelineMetadataBudget::FRHIPipelineMetadataBudget() : Used(std::make_shared<std::atomic<uint64>>(0)) {}
	auto FRHIPipelineMetadataBudget::Reserve(uint64 Bytes) -> std::shared_ptr<void>
	{
		constexpr uint64 Capacity = 64ull * 1024 * 1024;
		auto Current = Used->load();
		do
		{
			if (Bytes > Capacity - Current) throw FRHIRecoverableCreationError("Pipeline CPU metadata budget is full.");
		} while (!Used->compare_exchange_weak(Current, Current + Bytes));
		// On control-block allocation failure, shared_ptr also invokes this deleter.
		return std::shared_ptr<void>(nullptr, [Budget = Used, Bytes](void*) { Budget->fetch_sub(Bytes); });
	}
	namespace
	{
		constexpr uint32 MaxRequests = 256;
		constexpr uint32 MaxObservers = 4096;
		constexpr uint64 MaxDescriptionBytes = 1024 * 1024;
		constexpr uint64 MaxPendingBytes = 16 * 1024 * 1024;
		thread_local bool GInPipelineCreator = false;

		struct FRequestLifetime
		{
			std::atomic<uint32> Observers = 0;
			std::atomic<bool> Closed = false;
			std::atomic<bool> Retired = false;
			std::atomic<bool> Failed = false;
			FRHIPipelineMetadataBudget Metadata;
			std::exception_ptr TerminalFailure;
			std::mutex WaitMutex;
			std::condition_variable Changed;
		};

		// Observers share result data; Core completions carry no native references.
		// Device shutdown retires resources even when request handles survive it.
		struct FPipelinePublication
		{
			explicit FPipelinePublication(FRHIPipelineCreationResult InResult) : Result(std::move(InResult)) {}
			auto Snapshot() const -> FRHIPipelineCreationResult
			{
				std::lock_guard Lock(Mutex);
				return Result;
			}
			auto Retire() -> void
			{
				std::lock_guard Lock(Mutex);
				Result.Graphics = nullptr;
				Result.Compute = nullptr;
			}
			mutable std::mutex Mutex;
			FRHIPipelineCreationResult Result;
		};
		using FPublished = std::shared_ptr<FPipelinePublication>;
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
		struct FKeyEqual
		{
			auto operator()(const std::shared_ptr<const FKey>& A,
				const std::shared_ptr<const FKey>& B) const -> bool { return *A == *B; }
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

	struct FRHIPipelineCreationRequest::FState
	{
		FState(Tasks::FTaskGroup& Group, std::shared_ptr<FRequestLifetime> InLifetime)
			: Lifetime(std::move(InLifetime)),
			Source(Tasks::TCompletionSource<void>::Create(Group, {.DebugName = "RHI.PipelineObserver"})),
			Task(Tasks::Share(Source.TakeTask())) { ++Lifetime->Observers; }
		~FState() { --Lifetime->Observers; }
		auto Publish(const FPublished& Publication) -> void
		{
			std::lock_guard Lock(Lifetime->WaitMutex);
			if (Lifetime->Closed.load()) Source.TrySetCanceled();
			else
			{
				Published.Store(Publication);
				if (!Source.TrySetValue()) Published.Store(FPublished{});
			}
			Lifetime->Changed.notify_all();
		}
		auto Cancel() -> bool
		{
			std::lock_guard Lock(Lifetime->WaitMutex);
			const bool Canceled = Source.TrySetCanceled();
			Lifetime->Changed.notify_all();
			return Canceled;
		}
		auto Retire() -> void
		{
			Cancel();
			if (const auto Value = Published.Load()) Value->Retire();
		}
		std::shared_ptr<FRequestLifetime> Lifetime;
		Tasks::TCompletionSource<void> Source;
		Tasks::TSharedTask<void> Task;
		TAtomicSharedPtr<FPipelinePublication> Published;
		// Accessed only under the service mutex, and cleared at publication.
		std::shared_ptr<const FKey> PendingKey;
		std::shared_ptr<const FKey> Identity;
	};

	auto FRHIPipelineCreationRequest::Rejected(ERHIPipelineRequestRejection Reason) -> FRHIPipelineCreationRequest
	{
		FRHIPipelineCreationRequest Result;
		Result.Rejection = Reason;
		return Result;
	}
	auto FRHIPipelineCreationRequest::GetState() const -> ERHIPipelineRequestState
	{
		if (!State) return ERHIPipelineRequestState::Failed;
		if (State->Lifetime->Failed.load()) return ERHIPipelineRequestState::Failed;
		if (State->Lifetime->Retired.load()) return ERHIPipelineRequestState::Canceled;
		if (State->Task.GetState() == ETaskState::Succeeded)
			if (const auto Value = State->Published.Load()) return Value->Result.State;
		if (State->Task.GetState() == ETaskState::Canceled) return ERHIPipelineRequestState::Canceled;
		if (State->Task.GetState() == ETaskState::Failed) return ERHIPipelineRequestState::Failed;
		return ERHIPipelineRequestState::Pending;
	}
	auto FRHIPipelineCreationRequest::GetResult() const -> FRHIPipelineCreationResult
	{
		if (!State) return {.State = ERHIPipelineRequestState::Failed, .Diagnostic = "Pipeline request was not admitted."};
		if (State->Lifetime->Failed.load())
			std::rethrow_exception(State->Lifetime->TerminalFailure);
		if (State->Lifetime->Retired.load()) return {.State = ERHIPipelineRequestState::Canceled};
		if (State->Task.GetState() == ETaskState::Succeeded)
			if (const auto Value = State->Published.Load()) return Value->Snapshot();
		if (State->Task.GetState() == ETaskState::Canceled) return {.State = ERHIPipelineRequestState::Canceled};
		if (State->Task.GetState() == ETaskState::Failed)
			return {.State = ERHIPipelineRequestState::Failed, .Diagnostic = "Pipeline observer completion failed."};
		return {};
	}
	auto FRHIPipelineCreationRequest::GetCompletion() const -> Tasks::FTaskCompletion
	{
		return State ? State->Task.GetCompletion() : Tasks::FTaskCompletion{};
	}
	auto FRHIPipelineCreationRequest::IsCompute() const -> bool
	{
		return State && State->Identity && State->Identity->index() == 1;
	}
	auto FRHIPipelineCreationRequest::GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc>
	{
		if (!State || !State->Identity) return {};
		return {State->Identity, std::visit([](const auto& Key) { return &Key.PipelineLayout; }, *State->Identity)};
	}
	auto FRHIPipelineCreationRequest::Cancel() const -> bool { return State && State->Cancel(); }
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
		std::unique_lock Lock(State->Lifetime->WaitMutex);
		State->Lifetime->Changed.wait(Lock, [&] { return GetState() != ERHIPipelineRequestState::Pending; });
		return GetState() == ERHIPipelineRequestState::Ready;
	}

	struct FRHIPipelineCreationService::FState
	{
		using FObserver = FRHIPipelineCreationRequest::FState;
		struct FEntry
		{
			std::shared_ptr<const FKey> Key;
			std::variant<std::shared_ptr<const FRHIGraphicsPipelineCreationInputs>,
				std::shared_ptr<const FRHIComputePipelineCreationInputs>> Inputs;
			uint64 Bytes = 0;
		};
		struct FDrainLifetime
		{
			explicit FDrainLifetime(FState& InOwner) : Owner(InOwner) {}
			~FDrainLifetime() { if (!Finished) Owner.AbandonDrain(); }
			FState& Owner;
			bool Finished = true;
		};
		FState(const FRHICapabilities& InCapabilities, FBackend InBackend)
			: Capabilities(InCapabilities), Backend(std::move(InBackend)),
			ScopeOwner(CreateTaskScope()), Group(ScopeOwner.GetToken()) {}

		auto Reject(ERHIPipelineRequestRejection Reason) -> FRHIPipelineCreationRequest
		{
			++Statistics.RejectedRequests;
			return FRHIPipelineCreationRequest::Rejected(Reason);
		}
		auto MakeObserver() -> std::shared_ptr<FObserver>
		{
			auto Observer = std::make_shared<FObserver>(Group, Lifetime);
			for (uint32 Offset = 0; Offset < MaxObservers; ++Offset)
			{
				const uint32 Index = (NextObserver + Offset) % MaxObservers;
				if (!Observers[Index].expired()) continue;
				Observers[Index] = Observer;
				NextObserver = (Index + 1) % MaxObservers;
				return Observer;
			}
			check(false);
			return {};
		}
		auto RetainIdentity(std::shared_ptr<const FKey> Key, uint64 Bytes) -> std::shared_ptr<const FKey>
		{
			std::shared_ptr<void> Lease;
			if (Backend.ReserveMetadata) Lease = Backend.ReserveMetadata(Bytes);
			else Lease = Lifetime->Metadata.Reserve(Bytes);
			struct FIdentityOwner { std::shared_ptr<const FKey> Key; std::shared_ptr<void> Lease; };
			auto Owner = std::make_shared<FIdentityOwner>(Key, std::move(Lease));
			return {std::move(Owner), Key.get()};
		}
		template<typename T>
		auto Request(const T& Initializer, std::string_view Name) -> FRHIPipelineCreationRequest
		{
			std::lock_guard Lock(Mutex);
			if (Lifetime->Closed.load() || Lifetime->Failed.load()) return Reject(ERHIPipelineRequestRejection::Closed);
			if (!IsTaskSchedulerRunning()) return Reject(ERHIPipelineRequestRejection::Unsupported);
			if (!Group.GetToken().CanLaunchFromCurrentContext()) return Reject(ERHIPipelineRequestRejection::Unsupported);
			if (Lifetime->Observers.load() >= MaxObservers) return Reject(ERHIPipelineRequestRejection::CapacityExceeded);
			const uint64 Bytes = DescriptionBytes(Initializer, Name);
			if (Bytes > MaxDescriptionBytes) return Reject(ERHIPipelineRequestRejection::CapacityExceeded);
			const uint64 RetainedBytes = Bytes * 2 + sizeof(FEntry) + sizeof(FKey);
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			try
			{
				using TKey = std::conditional_t<Graphics, FGraphicsPipelineStateKey, FComputePipelineStateKey>;
				TKey NativeKey;
				std::string Error;
				bool Valid;
				if constexpr (Graphics) Valid = BuildGraphicsPipelineStateKey(Initializer, &Capabilities, NativeKey, Error);
				else Valid = BuildComputePipelineStateKey(Initializer, &Capabilities, NativeKey, Error);
				if (!Valid) return Reject(ERHIPipelineRequestRejection::InvalidDescription);
				FRHIPipelineCreationResult Ready;
				if constexpr (Graphics) Ready.Graphics = Backend.FindGraphics(NativeKey);
				else Ready.Compute = Backend.FindCompute(NativeKey);
				FRHIPipelineCreationRequest Result;
				if (Ready.Graphics || Ready.Compute)
				{
					auto Identity = RetainIdentity(std::make_shared<const FKey>(std::move(NativeKey)), RetainedBytes);
					Ready.State = ERHIPipelineRequestState::Ready;
					Result.State = MakeObserver();
					Result.State->Identity = std::move(Identity);
					Result.Rejection = ERHIPipelineRequestRejection::None;
					Result.State->Publish(std::make_shared<FPipelinePublication>(std::move(Ready)));
					return Result;
				}
				auto Key = std::make_shared<const FKey>(std::move(NativeKey));
				if (const auto Existing = Pending.find(Key); Existing != Pending.end())
				{
					Result.State = MakeObserver();
					Result.State->PendingKey = Existing->second->Key;
					Result.State->Identity = Existing->second->Key;
					Result.Rejection = ERHIPipelineRequestRejection::None;
					++Statistics.SharedPendingHits;
					return Result;
				}
				if (Pending.size() >= MaxRequests || RetainedBytes > MaxPendingBytes - Statistics.UnfinishedDescriptionBytes)
					return Reject(ERHIPipelineRequestRejection::CapacityExceeded);
				auto Ticket = DrainActive ? std::shared_ptr<FDrainLifetime>{} : std::make_shared<FDrainLifetime>(*this);
				auto Entry = std::make_shared<FEntry>();
				Key = RetainIdentity(std::move(Key), RetainedBytes);
				Entry->Key = Key;
				Entry->Bytes = RetainedBytes;
				if constexpr (Graphics) Entry->Inputs = std::make_shared<const FRHIGraphicsPipelineCreationInputs>(Initializer, Name);
				else Entry->Inputs = std::make_shared<const FRHIComputePipelineCreationInputs>(Initializer, Name);
				Result.State = MakeObserver();
				Result.State->PendingKey = Entry->Key;
				Result.State->Identity = Entry->Key;
				Pending.emplace(Key, Entry);
				try { Queue.push_back(Entry); }
				catch (...) { Pending.erase(Key); throw; }
				Statistics.UnfinishedDescriptionBytes += RetainedBytes;
				Statistics.UnfinishedRequests = static_cast<uint32>(Pending.size());
				Result.Rejection = ERHIPipelineRequestRejection::None;
				if (!DrainActive)
				{
					DrainActive = true;
					Ticket->Finished = false;
					LastDrain = Tasks::Share(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker,
						{.DebugName = "RHI.PipelineCreation"}, [this, Ticket] {
							try { Drain(); }
							catch (...)
							{
								Lifetime->TerminalFailure = std::current_exception();
								Backend.PublishTerminalFailure(Lifetime->TerminalFailure);
								Lifetime->Failed.store(true);
								AbandonDrain();
								Ticket->Finished = true;
								throw;
							}
							Ticket->Finished = true;
						}));
				}
				return Result;
			}
			catch (const std::bad_alloc&) { return Reject(ERHIPipelineRequestRejection::CapacityExceeded); }
			catch (const FRHIRecoverableCreationError&) { return Reject(ERHIPipelineRequestRejection::CapacityExceeded); }
		}
		auto CompleteEntry(const std::shared_ptr<FEntry>& Entry, const FPublished& Publication) -> void
		{
			std::lock_guard Lock(Mutex);
			for (auto& Weak : Observers)
				if (auto Observer = Weak.lock(); Observer && Observer->PendingKey == Entry->Key)
				{
					Observer->Publish(Publication);
					Observer->PendingKey.reset();
				}
			Pending.erase(Entry->Key);
			Statistics.UnfinishedDescriptionBytes -= Entry->Bytes;
			Statistics.UnfinishedRequests = static_cast<uint32>(Pending.size());
		}
		auto AbandonDrain() -> void
		{
			std::lock_guard Lock(Mutex);
			if (!Lifetime->Failed.load()) Lifetime->Closed.store(true);
			for (auto& Weak : Observers) if (auto Observer = Weak.lock())
			{
				Observer->Cancel();
				Observer->PendingKey.reset();
			}
			Queue.clear();
			Pending.clear();
			Statistics.UnfinishedRequests = 0;
			Statistics.UnfinishedDescriptionBytes = 0;
			DrainActive = false;
		}
		auto Drain() -> void
		{
			GInPipelineCreator = true;
			struct FCreatorScope { ~FCreatorScope() { GInPipelineCreator = false; } } Scope;
			while (true)
			{
				std::shared_ptr<FEntry> Entry;
				{
					std::lock_guard Lock(Mutex);
					if (Queue.empty()) { DrainActive = false; return; }
					Entry = std::move(Queue.front());
					Queue.pop_front();
					++Statistics.NativeRequests;
				}
				FRHIPipelineCreationResult Result;
				try
				{
					if (Entry->Key->index() == 0)
						Result.Graphics = Backend.CreateGraphics(*std::get<0>(Entry->Inputs), std::get<0>(*Entry->Key));
					else Result.Compute = Backend.CreateCompute(*std::get<1>(Entry->Inputs), std::get<1>(*Entry->Key));
					Result.State = Result.Graphics || Result.Compute ? ERHIPipelineRequestState::Ready : ERHIPipelineRequestState::Failed;
				}
				catch (const FRHIRecoverableCreationError& Error)
				{
					Result.State = ERHIPipelineRequestState::Failed;
					Result.Diagnostic = Error.what();
				}
				CompleteEntry(Entry, std::make_shared<FPipelinePublication>(std::move(Result)));
			}
		}

		FRHICapabilities Capabilities;
		FBackend Backend;
		std::shared_ptr<FRequestLifetime> Lifetime = std::make_shared<FRequestLifetime>();
		// The device owns stronger drain authority over a scope containing only
		// worker creation and external observations; no owning-thread child is admitted.
		FTaskScope ScopeOwner;
		Tasks::FTaskGroup Group;
		Tasks::TSharedTask<void> LastDrain;
		mutable std::mutex Mutex;
		std::array<std::weak_ptr<FObserver>, MaxObservers> Observers;
		uint32 NextObserver = 0;
		std::unordered_map<std::shared_ptr<const FKey>, std::shared_ptr<FEntry>, FKeyHash, FKeyEqual> Pending;
		std::deque<std::shared_ptr<FEntry>> Queue;
		FRHIPipelineCreationStatistics Statistics;
		bool DrainActive = false;
		bool CloseStarted = false;
		std::mutex CloseMutex;
	};

	FRHIPipelineCreationService::FRHIPipelineCreationService(const FRHICapabilities& Capabilities, FBackend Backend)
		: State(std::make_unique<FState>(Capabilities, std::move(Backend))) {}
	FRHIPipelineCreationService::~FRHIPipelineCreationService() { CloseAndJoin(); }
	auto FRHIPipelineCreationService::IsClosed() const -> bool
	{
		return State->Lifetime->Closed.load() || State->Lifetime->Failed.load();
	}
	auto FRHIPipelineCreationService::RequestGraphicsBatch(std::span<const FRHIGraphicsPipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		if (Items.size() > MaxRequests) return {.Rejection = ERHIPipelineRequestRejection::CapacityExceeded};
		FRHIPipelineCreationBatch Result;
		Result.Items.reserve(Items.size());
		for (const auto& Item : Items) Result.Items.push_back(RequestGraphics(Item.Initializer, Item.DebugName));
		return Result;
	}
	auto FRHIPipelineCreationService::RequestComputeBatch(std::span<const FRHIComputePipelineBatchItem> Items)
		-> FRHIPipelineCreationBatch
	{
		if (Items.size() > MaxRequests) return {.Rejection = ERHIPipelineRequestRejection::CapacityExceeded};
		FRHIPipelineCreationBatch Result;
		Result.Items.reserve(Items.size());
		for (const auto& Item : Items) Result.Items.push_back(RequestCompute(Item.Initializer, Item.DebugName));
		return Result;
	}
	auto FRHIPipelineCreationService::RequestGraphics(const FGraphicsPipelineStateInitializer& Initializer,
		std::string_view Name) -> FRHIPipelineCreationRequest { return State->Request(Initializer, Name); }
	auto FRHIPipelineCreationService::RequestCompute(const FComputePipelineStateInitializer& Initializer,
		std::string_view Name) -> FRHIPipelineCreationRequest { return State->Request(Initializer, Name); }
	auto FRHIPipelineCreationService::GetStatistics() const -> FRHIPipelineCreationStatistics
	{
		std::lock_guard Lock(State->Mutex);
		auto Result = State->Statistics;
		Result.ActiveObservers = State->Lifetime->Observers.load();
		return Result;
	}
	auto FRHIPipelineCreationService::CloseAndJoin(bool RetireResults) -> void
	{
		std::lock_guard CloseLock(State->CloseMutex);
		{
			std::lock_guard Lock(State->Mutex);
			if (!State->CloseStarted)
			{
				State->CloseStarted = true;
				State->Lifetime->Closed.store(true);
				for (auto& Weak : State->Observers) if (auto Observer = Weak.lock())
				{
					Observer->Cancel();
					Observer->PendingKey.reset();
				}
				for (const auto& Entry : State->Queue)
				{
					State->Pending.erase(Entry->Key);
					State->Statistics.UnfinishedDescriptionBytes -= Entry->Bytes;
				}
				State->Queue.clear();
				State->Statistics.UnfinishedRequests = static_cast<uint32>(State->Pending.size());
				State->Group.Close();
			}
		}
		const auto Joined = State->ScopeOwner.Wait();
		checkf(Joined == ETaskScopeWaitResult::Quiescent, "Pipeline creation scope could not join.");
		if (RetireResults && !State->Lifetime->Retired.exchange(true))
		{
			for (auto& Weak : State->Observers) if (auto Observer = Weak.lock()) Observer->Retire();
		}
	}
}
