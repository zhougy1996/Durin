#include "DerivedDataCache/DerivedDataCacheAsync.h"

namespace Durin::DerivedData::Private
{
	struct FCacheOperation;
	struct FCacheOwnerState
	{
		std::mutex Mutex;
		std::atomic<bool> Canceled = false;
		EBuildPriority Priority = EBuildPriority::Normal;
		std::vector<std::shared_ptr<FCacheOperation>> Operations;
	};
	struct FAsyncCacheState;
	struct FCacheCallbackScope;
	thread_local FCacheCallbackScope* CurrentCacheScope = nullptr;
	struct FCacheCallbackScope
	{
		FCacheOwnerState* Owner;
		FAsyncCacheState* Cache;
		FCacheCallbackScope* Previous = CurrentCacheScope;
		FCacheCallbackScope(FCacheOwnerState* Owner, FAsyncCacheState* Cache) : Owner(Owner), Cache(Cache) { CurrentCacheScope = this; }
		~FCacheCallbackScope() { CurrentCacheScope = Previous; }
	};
	struct FCacheOperation
	{
		std::mutex Mutex;
		std::condition_variable Changed;
		bool Dispatching = true, Released = false;
		std::atomic<bool> Started = false, Completing = false, Canceled = false;
		std::shared_ptr<IBuildScheduledWork> Scheduled;
		std::shared_ptr<FAsyncCacheState> Cache;
		std::shared_ptr<FCacheOwnerState> Owner;
		FCacheGetRequest GetRequest;
		std::vector<FValueId> Ids;
		std::optional<FCachePutRequest> PutRequest;
		FCacheGetCallback GetCallback;
		FCachePutCallback PutCallback;
		uint32 Index = 0;
		auto IsCanceled() const -> bool { return Canceled.load() || Owner->Canceled.load(); }
		auto Wait() -> EBuildWaitResult
		{
			std::shared_ptr<IBuildScheduledWork> Work;
			{
				std::unique_lock Lock(Mutex);
				Changed.wait(Lock, [&] { return !Dispatching || Released; });
				if (Released) return EBuildWaitResult::Completed;
				Work = Scheduled;
			}
			if (Work && Work->Wait() != EBuildWaitResult::Completed) return EBuildWaitResult::WouldBlock;
			std::unique_lock Lock(Mutex); Changed.wait(Lock, [&] { return Released; });
			return EBuildWaitResult::Completed;
		}
		auto Finish(FCacheGetResult GetResult, FCachePutResult PutResult, FCacheQueryStats Stats = {}) -> void;
		auto Run() -> void;
	};
	struct FAsyncCacheState : std::enable_shared_from_this<FAsyncCacheState>
	{
		std::mutex Mutex;
		bool Closed = false, Pumping = false;
		uint32 Active = 0, Concurrency = 2, Capacity = 4096;
		std::shared_ptr<ICache> Backend;
		std::shared_ptr<IBuildScheduler> Scheduler;
		std::vector<std::shared_ptr<FCacheOperation>> Pending, All;
		auto Pump() -> void;
	};
	auto CanceledError() -> FCacheError { return {ECacheError::Canceled, "Asynchronous cache request canceled or dropped."}; }
	auto FCacheOperation::Finish(FCacheGetResult GetResult, FCachePutResult PutResult, FCacheQueryStats Stats) -> void
	{
		if (Completing.exchange(true)) return;
		if (IsCanceled()) { GetResult = std::unexpected(CanceledError()); PutResult = std::unexpected(CanceledError()); }
		FCacheCallbackScope Scope(Owner.get(), Cache.get());
		try
		{
			if (GetCallback) GetCallback(Index, std::move(GetResult), Stats);
			else if (PutCallback) PutCallback(Index, std::move(PutResult));
		}
		catch (...) {}
		GetCallback = {}; PutCallback = {}; GetRequest = {}; Ids.clear(); PutRequest.reset();
		{ std::lock_guard Lock(Owner->Mutex); std::erase_if(Owner->Operations, [&](const auto& Op) { return Op.get() == this; }); }
		{ std::lock_guard Lock(Mutex); Released = true; }
		Changed.notify_all();
		{ std::lock_guard Lock(Cache->Mutex); std::erase_if(Cache->All, [&](const auto& Op) { return Op.get() == this; }); --Cache->Active; }
		Cache->Pump();
	}
	auto FCacheOperation::Run() -> void
	{
		if (Started.exchange(true)) return;
		FCacheGetResult Result = std::unexpected(CanceledError());
		FCachePutResult Saved = std::unexpected(CanceledError());
		FCacheQueryStats Stats;
		try
		{
			if (!IsCanceled())
			{
				if (PutRequest)
				{
					PutRequest->ShouldCancel = [this, External = std::move(PutRequest->ShouldCancel)] { return IsCanceled() || (External && External()); };
					Saved = Cache->Backend->Put(*PutRequest);
				}
				else
				{
					GetRequest.RequiredValues = Ids; GetRequest.Stats = &Stats;
					GetRequest.ShouldCancel = [this, External = std::move(GetRequest.ShouldCancel)] { return IsCanceled() || (External && External()); };
					Result = Cache->Backend->Get(GetRequest);
				}
			}
		}
		catch (...) { Result = std::unexpected(FCacheError{ECacheError::StorageFailure, "Asynchronous cache backend threw."}); Saved = std::unexpected(Result.error()); }
		Finish(std::move(Result), std::move(Saved), Stats);
	}
	struct FCacheDispatchTicket
	{
		std::shared_ptr<FCacheOperation> Operation;
		~FCacheDispatchTicket()
		{
			if (!Operation->Started.load()) Operation->Finish(std::unexpected(CanceledError()), std::unexpected(CanceledError()));
		}
	};
	auto FAsyncCacheState::Pump() -> void
	{
		{ std::lock_guard Lock(Mutex); if (Pumping) return; Pumping = true; }
		for (;;)
		{
			std::shared_ptr<FCacheOperation> Op;
			{
				std::lock_guard Lock(Mutex);
				if (Active == Concurrency || Pending.empty()) { Pumping = false; return; }
				const auto Next = std::ranges::min_element(Pending, {}, [](const auto& Item) { return Item->Owner->Priority; });
				Op = std::move(*Next); Pending.erase(Next); ++Active;
			}
			try
			{
				auto Ticket = std::make_shared<FCacheDispatchTicket>(); Ticket->Operation = Op;
				const auto Budget = Op->PutRequest ? Op->PutRequest->MaximumWorkingSetBytes : Op->GetRequest.MaximumWorkingSetBytes;
				auto Accepted = Scheduler->Schedule({"DerivedDataCacheIO", Op->Owner->Priority, Budget}, [Ticket] { Ticket->Operation->Run(); });
				if (!Accepted) Op->Finish(std::unexpected(FCacheError{ECacheError::StorageFailure, Accepted.error().Description}),
					std::unexpected(FCacheError{ECacheError::StorageFailure, Accepted.error().Description}));
				{ std::lock_guard Lock(Op->Mutex); if (Accepted) Op->Scheduled = std::move(*Accepted); Op->Dispatching = false; }
				Op->Changed.notify_all();
			}
			catch (...)
			{
				Op->Finish(std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache dispatch threw."}),
					std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache dispatch threw."}));
			}
		}
	}
	auto WaitOwner(const std::shared_ptr<FCacheOwnerState>& State) -> EBuildWaitResult
	{
		for (auto* Scope = CurrentCacheScope; Scope; Scope = Scope->Previous)
			if (Scope->Owner == State.get()) return EBuildWaitResult::WouldBlock;
		for (;;)
		{
			std::shared_ptr<FCacheOperation> Op;
			{ std::lock_guard Lock(State->Mutex); if (State->Operations.empty()) return EBuildWaitResult::Completed; Op = State->Operations.front(); }
			// Completion holds an active slot until the callback returns. Waiting on
			// another owner in this cache can otherwise exhaust all available slots.
			for (auto* Scope = CurrentCacheScope; Scope; Scope = Scope->Previous)
				if (Scope->Cache == Op->Cache.get()) return EBuildWaitResult::WouldBlock;
			if (Op->Wait() != EBuildWaitResult::Completed) return EBuildWaitResult::WouldBlock;
		}
	}
	class FCacheOwnerWork final : public IBuildScheduledWork
	{
	public:
		std::shared_ptr<FCacheOwnerState> Owner;
		auto Wait() const -> EBuildWaitResult override { return WaitOwner(Owner); }
	};
	class FAsyncCache final : public IAsyncCache
	{
	public:
		std::shared_ptr<FAsyncCacheState> State;
		FAsyncCache(std::shared_ptr<ICache> Cache, std::shared_ptr<IBuildScheduler> Scheduler, uint32 Concurrency, uint32 Capacity)
			: State(std::make_shared<FAsyncCacheState>())
		{
			State->Backend = Cache ? std::move(Cache) : std::shared_ptr<ICache>(&GetCache(), [](ICache*) {});
			State->Scheduler = Scheduler ? std::move(Scheduler) : CreateTaskCacheScheduler();
			State->Concurrency = std::clamp(Concurrency, 1u, 64u); State->Capacity = std::clamp(Capacity, 1u, 4096u);
			State->Pending.reserve(State->Capacity); State->All.reserve(State->Capacity);
		}
		~FAsyncCache() override { Close(); Drain(); }
		auto Admit(std::vector<std::shared_ptr<FCacheOperation>> Operations, FCacheRequestOwner& Owner)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError>
		{
			auto Handle = std::make_shared<FCacheOwnerWork>(); Handle->Owner = Owner.State;
			{
				std::scoped_lock Lock(State->Mutex, Owner.State->Mutex);
				if (State->Closed) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Asynchronous cache is closed."});
				if (Operations.size() > State->Capacity - State->All.size() || Operations.size() > 4096 - Owner.State->Operations.size())
					return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Asynchronous cache request capacity exhausted."});
				Owner.State->Operations.reserve(Owner.State->Operations.size() + Operations.size());
				for (auto& Op : Operations)
				{
					Op->Owner = Owner.State; Op->Cache = State;
					State->All.push_back(Op); State->Pending.push_back(Op); Owner.State->Operations.push_back(Op);
				}
			}
			State->Pump(); return Handle;
		}
		auto Get(std::span<const FCacheGetRequest> Requests, FCacheRequestOwner& Owner, FCacheGetCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> override
		try
		{
			if (Requests.empty() || Requests.size() > State->Capacity || !Callback)
				return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Invalid asynchronous cache batch."});
			std::vector<std::shared_ptr<FCacheOperation>> Ops; Ops.reserve(Requests.size());
			for (uint32 I = 0; I < Requests.size(); ++I)
			{
				if (Requests[I].RequiredValues.size() > 4096) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Selection is unbounded."});
				auto Op = std::make_shared<FCacheOperation>(); Op->Index = I; Op->GetRequest = Requests[I]; Op->GetRequest.Stats = nullptr;
				Op->GetRequest.RequiredValues = {}; Op->Ids.assign(Requests[I].RequiredValues.begin(), Requests[I].RequiredValues.end());
				Op->GetCallback = Callback; Ops.push_back(std::move(Op));
			}
			return Admit(std::move(Ops), Owner);
		}
		catch (...) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache batch admission failed."}); }
		auto Put(std::span<const FCachePutRequest> Requests, FCacheRequestOwner& Owner, FCachePutCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> override
		try
		{
			if (Requests.empty() || Requests.size() > State->Capacity || !Callback)
				return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Invalid asynchronous cache batch."});
			std::vector<std::shared_ptr<FCacheOperation>> Ops; Ops.reserve(Requests.size());
			for (uint32 I = 0; I < Requests.size(); ++I)
			{ auto Op = std::make_shared<FCacheOperation>(); Op->Index = I; Op->PutRequest = Requests[I]; Op->PutCallback = Callback; Ops.push_back(std::move(Op)); }
			return Admit(std::move(Ops), Owner);
		}
		catch (...) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache batch admission failed."}); }
		auto Close() -> void override
		{ std::lock_guard Lock(State->Mutex); State->Closed = true; for (const auto& Op : State->All) Op->Canceled = true; }
		auto Drain() -> EBuildWaitResult override
		{
			for (auto* Scope = CurrentCacheScope; Scope; Scope = Scope->Previous)
				if (Scope->Cache == State.get()) return EBuildWaitResult::WouldBlock;
			for (;;)
			{
				std::shared_ptr<FCacheOperation> Op;
				{ std::lock_guard Lock(State->Mutex); if (State->All.empty()) return EBuildWaitResult::Completed; Op = State->All.front(); }
				if (Op->Wait() != EBuildWaitResult::Completed) return EBuildWaitResult::WouldBlock;
			}
		}
	};
}

namespace Durin::DerivedData
{
	FCacheRequestOwner::FCacheRequestOwner(EBuildPriority Priority) : State(std::make_shared<Private::FCacheOwnerState>()) { State->Priority = Priority; }
	FCacheRequestOwner::~FCacheRequestOwner() { Cancel(); Wait(); }
	auto FCacheRequestOwner::Cancel() -> void { State->Canceled = true; }
	auto FCacheRequestOwner::Wait() const -> EBuildWaitResult { return Private::WaitOwner(State); }
	auto FCacheRequestOwner::Poll() const -> bool { std::lock_guard Lock(State->Mutex); return State->Operations.empty(); }
	auto CreateAsyncCache(std::shared_ptr<ICache> Cache, std::shared_ptr<IBuildScheduler> Scheduler, uint32 Concurrency, uint32 Capacity) -> std::shared_ptr<IAsyncCache>
	{ return std::make_shared<Private::FAsyncCache>(std::move(Cache), std::move(Scheduler), Concurrency, Capacity); }
}
