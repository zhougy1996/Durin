#include "DerivedDataCache/DerivedDataCacheAsync.h"
#include "RequestOwnerPrivate.h"

namespace Durin::DerivedData::Private
{
	struct FCacheOperation;
	struct FAsyncCacheState;
	struct FCacheOperation : FRequestCompletion
	{
		std::atomic<bool> Started = false, Completing = false, Canceled = false;
		bool Active = false; // Protected by Cache->Mutex.
		std::shared_ptr<FAsyncCacheState> Cache;
		FCacheGetRequest GetRequest;
		std::vector<FValueId> Ids;
		std::optional<FCachePutRequest> PutRequest;
		FCacheGetCallback GetCallback;
		FCachePutCallback PutCallback;
		uint32 Index = 0;
		auto IsCanceled() const -> bool { return Canceled.load() || Group->IsCanceled(); }
		auto OnRetired() -> void override;
		auto RequestCancel() -> bool override;
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
	auto FCacheOperation::OnRetired() -> void
	{
		std::lock_guard Lock(Cache->Mutex);
		std::erase_if(Cache->All, [&](const auto& Op) { return Op.get() == this; });
	}
	auto FCacheOperation::Finish(FCacheGetResult GetResult, FCachePutResult PutResult, FCacheQueryStats Stats) -> void
	{
		if (Completing.exchange(true)) return;
		if (IsCanceled()) { GetResult = std::unexpected(CanceledError()); PutResult = std::unexpected(CanceledError()); }
		Complete([&] {
			try {
				if (GetCallback) GetCallback(Index, std::move(GetResult), Stats);
				else if (PutCallback) PutCallback(Index, std::move(PutResult));
			} catch (...) {}
			GetCallback = {}; PutCallback = {}; GetRequest = {}; Ids.clear(); PutRequest.reset();
			{ std::lock_guard Lock(Cache->Mutex); if (Active) { --Cache->Active; Active = false; } }
		}, Cache->Scheduler->IsInline() ? nullptr : Cache.get());
		Cache->Pump();
	}
	auto FCacheOperation::RequestCancel() -> bool
	{
		const bool Changed = !Canceled.exchange(true);
		bool Pending = false, CompleteNow = false;
		{ std::lock_guard Lock(Cache->Mutex); Pending = std::erase_if(Cache->Pending, [&](const auto& Op) { return Op.get() == this; }) != 0; CompleteNow = (Pending || Active) && !Started.exchange(true); }
		if (CompleteNow) { if (Pending) DispatchReturned(); Finish(std::unexpected(CanceledError()), std::unexpected(CanceledError())); }
		std::shared_ptr<IBuildScheduledWork> Work;
		{ std::lock_guard Lock(Mutex); Work = Scheduled; }
		if (Work) Work->RequestCancel();
		return Changed;
	}
	auto FCacheOperation::Run() -> void
	{
		if (Started.exchange(true)) return;
		FCacheGetResult Result = std::unexpected(CanceledError());
		FCachePutResult Saved = std::unexpected(CanceledError());
		FCacheQueryStats Stats;
		try
		{
			const auto& External = PutRequest ? PutRequest->ShouldCancel : GetRequest.ShouldCancel;
			if (!IsCanceled() && !(External && External()))
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
	auto FAsyncCacheState::Pump() -> void
	{
		const bool Inline = Scheduler->IsInline();
		{ std::lock_guard Lock(Mutex); if (!Inline && Pumping) return; if (!Inline) Pumping = true; }
		for (;;)
		{
			std::shared_ptr<FCacheOperation> Op;
			{
				std::lock_guard Lock(Mutex);
				if ((!Inline && Active == Concurrency) || Pending.empty()) { if (!Inline) Pumping = false; return; }
				const auto Next = std::ranges::min_element(Pending, {}, [](const auto& Item) { return Item->Group->GetPriority(); });
				Op = std::move(*Next); Pending.erase(Next); ++Active; Op->Active = true;
			}
			try
			{
				auto Accepted = Op->Group->LaunchTask(Op.get(), *Scheduler, "DerivedDataCacheIO", [Op] { Op->Run(); },
					[Op] { Op->RequestCancel(); });
				if (!Accepted) Op->Finish(std::unexpected(FCacheError{ECacheError::StorageFailure, Accepted.error().Description}),
					std::unexpected(FCacheError{ECacheError::StorageFailure, Accepted.error().Description}));
				{ std::lock_guard Lock(Op->Mutex); if (Accepted) Op->Scheduled = *Accepted; }
				if (Accepted && *Accepted && Op->IsCanceled()) (*Accepted)->RequestCancel();
				Op->DispatchReturned();
			}
			catch (...)
			{
				Op->Finish(std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache dispatch threw."}),
					std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache dispatch threw."}));
				Op->DispatchReturned();
			}
		}
	}
	class FCacheBatchWork final : public IBuildScheduledWork
	{
	public:
		std::vector<std::weak_ptr<FCacheOperation>> Operations;
		auto RequestCancel() const -> void override { for (const auto& Weak : Operations) if (auto Op = Weak.lock()) Op->RequestCancel(); }
		auto Wait() const -> ERequestWaitResult override {
			for (const auto& Weak : Operations) if (const auto Op = Weak.lock(); Op && Op->Wait() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock;
			return ERequestWaitResult::Completed;
		}
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
		auto Admit(std::vector<std::shared_ptr<FCacheOperation>> Operations, IRequestOwner& Owner)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError>
		{
			auto Handle = std::make_shared<FCacheBatchWork>();
			{
				std::unique_lock Lock(State->Mutex);
				if (State->Closed) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Asynchronous cache is closed."});
				if (Operations.size() > State->Capacity - State->All.size())
					return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Asynchronous cache request capacity exhausted."});
				Handle->Operations.assign(Operations.begin(), Operations.end());
				try { for (auto& Op : Operations)
				{
					Op->Cache = State; Op->WaitDomain = State->Scheduler->IsInline() ? nullptr : State.get();
					{ std::lock_guard OperationLock(Op->Mutex); Op->Group = Owner.Begin(Op); }
					State->All.push_back(Op); State->Pending.push_back(Op);
				} }
				catch (...) {
					for (auto& Op : Operations) {
						std::erase(State->All, Op); std::erase(State->Pending, Op);
					}
					Lock.unlock();
					for (auto& Op : Operations) if (Op->Group) { Op->DispatchReturned(); Op->Complete(); }
					throw;
				}
			}
			for (auto& Op : Operations) if (Op->IsCanceled()) Op->RequestCancel();
			State->Pump(); return Handle;
		}
		auto Get(std::span<const FCacheGetRequest> Requests, IRequestOwner& Owner, FCacheGetCallback Callback)
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
		auto Put(std::span<const FCachePutRequest> Requests, IRequestOwner& Owner, FCachePutCallback Callback)
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
		{ std::vector<std::shared_ptr<FCacheOperation>> Snapshot; { std::lock_guard Lock(State->Mutex); State->Closed = true; Snapshot = State->All; } for (const auto& Op : Snapshot) Op->RequestCancel(); }
		auto Drain() -> ERequestWaitResult override
		{
			if (FRequestOwnerScope::Blocks(nullptr, State.get())) return ERequestWaitResult::WouldBlock;
			for (;;)
			{
				std::shared_ptr<FCacheOperation> Op;
				{ std::lock_guard Lock(State->Mutex); if (State->All.empty()) return ERequestWaitResult::Completed; Op = State->All.front(); }
				if (Op->Wait() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock;
			}
		}
	};
}

namespace Durin::DerivedData
{
	auto CreateAsyncCache(std::shared_ptr<ICache> Cache, std::shared_ptr<IBuildScheduler> Scheduler, uint32 Concurrency, uint32 Capacity) -> std::shared_ptr<IAsyncCache>
	{ return std::make_shared<Private::FAsyncCache>(std::move(Cache), std::move(Scheduler), Concurrency, Capacity); }
}
