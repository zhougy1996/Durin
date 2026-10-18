#include "DerivedDataCache/DerivedDataCacheAsync.h"
#include "RequestOwnerPrivate.h"
#include "CacheDiagnosticsPrivate.h"

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
		std::chrono::steady_clock::time_point QueuedAt;
		uint64 QueuedTurn = 0; // Protected by Cache->Mutex.
		uint64 RetainedBytes = 0;
		bool Admitted = false; // Protected by Cache->Mutex.
		auto IsCanceled() const -> bool { return Canceled.load() || Group->IsCanceled(); }
		auto OnFinished() -> void override;
		auto RequestCancel() -> bool override;
		auto Finish(FCacheGetResult GetResult, FCachePutResult PutResult, FCacheQueryStats Stats = {}) -> void;
		auto Run() -> void;
	};
	struct FAsyncCacheState : std::enable_shared_from_this<FAsyncCacheState>
	{
		std::mutex Mutex;
		bool Closed = false, Pumping = false;
		uint32 Active = 0, Concurrency = 2, Capacity = 4096;
		uint64 DispatchTurn = 0;
		uint64 RetainedBytes = 0, MaximumRetainedBytes = 512ull * 1024 * 1024;
		std::shared_ptr<ICache> Backend;
		std::shared_ptr<IBuildScheduler> Scheduler;
		std::vector<std::shared_ptr<FCacheOperation>> Pending, All;
		auto Pump() -> void;
	};
	auto CanceledError() -> FCacheError { return {ECacheError::Canceled, "Asynchronous cache request canceled or dropped."}; }
	auto FCacheOperation::OnFinished() -> void
	{
		std::lock_guard Lock(Cache->Mutex);
		std::erase_if(Cache->All, [&](const auto& Op) { return Op.get() == this; });
		if (Admitted) { Cache->RetainedBytes -= RetainedBytes; Admitted = false; }
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
			GetCallback = {}; PutCallback = {}; GetRequest = {}; std::vector<FValueId>().swap(Ids); PutRequest.reset();
			{ std::lock_guard Lock(Cache->Mutex); if (Active) { --Cache->Active; Active = false; } }
		}, Cache->Scheduler->IsInline() ? nullptr : Cache.get());
		// Inline dispatch returns to the existing pump, which advances the batch.
		// Explicit nested submissions still pump synchronously from Admit.
		if (!Cache->Scheduler->IsInline()) Cache->Pump();
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
		ObserveCache(PutRequest ? PutRequest->Record.GetKey() : GetRequest.Key, ECacheObservation::AsyncStart,
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - QueuedAt).count());
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
				// FIFO admission means only the oldest item's age needs checking.
				// Unsigned turn subtraction also preserves age across counter wrap.
				auto Next = Pending.begin();
				if (DispatchTurn - (*Next)->QueuedTurn < 32)
					Next = std::ranges::min_element(Pending, {}, [](const auto& Item) { return Item->Group->GetPriority(); });
				Op = std::move(*Next); Pending.erase(Next); ++Active; Op->Active = true;
				++DispatchTurn;
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
		FAsyncCache(std::shared_ptr<ICache> Cache, std::shared_ptr<IBuildScheduler> Scheduler, FCacheDispatchLimits Limits)
			: State(std::make_shared<FAsyncCacheState>())
		{
			State->Backend = Cache ? std::move(Cache) : std::shared_ptr<ICache>(&GetCache(), [](ICache*) {});
			State->Scheduler = Scheduler ? std::move(Scheduler) : CreateTaskCacheScheduler();
			State->Concurrency = std::clamp(Limits.Concurrency, 1u, 64u); State->Capacity = std::clamp(Limits.Capacity, 1u, 4096u);
			State->MaximumRetainedBytes = Limits.MaximumRetainedBytes;
			State->Pending.reserve(State->Capacity); State->All.reserve(State->Capacity);
		}
		~FAsyncCache() override { Close(); Drain(); }
		auto Admit(std::vector<std::shared_ptr<FCacheOperation>> Operations, IRequestOwner& Owner)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError>
		{
			auto Handle = std::make_shared<FCacheBatchWork>();
			uint64 BatchBytes = 0;
			for (auto& Op : Operations)
			{
				const auto Charge = [&](uint64 Bytes) {
					if (Bytes > State->MaximumRetainedBytes - BatchBytes) return false;
					BatchBytes += Bytes; Op->RetainedBytes += Bytes; return true;
				};
				// Fixed request/table overhead is bounded by item and record limits.
				// This budget covers queued data, not total allocation overhead.
				if (!Charge(Op->Ids.size() * sizeof(FValueId)))
					return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-request byte capacity exhausted."});
				if (!Op->PutRequest) continue;
				const auto& Record = Op->PutRequest->Record;
				if (!Charge(Record.GetSchema().size()))
					return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-record byte capacity exhausted."});
				std::vector<FSharedByteBuffer> Backings;
				const auto Retain = [&](FSharedByteBuffer Buffer) {
					if (std::ranges::any_of(Backings, [&](const auto& Other) { return Buffer.SharesStorageWith(Other); })) return true;
					if (!Charge(Buffer.GetRetainedCapacityBytes())) return false;
					Backings.push_back(std::move(Buffer)); return true;
				};
				for (const auto& Value : Record.GetValues()) if (!Retain(Value.Value.GetData()))
					return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-payload byte capacity exhausted."});
				for (const auto& Meta : Record.GetMetadata()) if (!Retain(Meta.Object.GetBytes()))
					return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-metadata byte capacity exhausted."});
				for (const auto& Message : Record.GetMessages()) if (!Charge(Message.Text.size()))
					return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-message byte capacity exhausted."});
			}
			{
				std::unique_lock Lock(State->Mutex);
				if (State->Closed) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Asynchronous cache is closed."});
				if (Operations.size() > State->Capacity - State->All.size() || BatchBytes > State->MaximumRetainedBytes - State->RetainedBytes)
					return std::unexpected(FCacheError{ECacheError::Capacity, "Asynchronous cache request capacity exhausted."});
				Handle->Operations.assign(Operations.begin(), Operations.end());
				try { for (auto& Op : Operations)
				{
					Op->Cache = State; Op->QueuedTurn = State->DispatchTurn; Op->QueuedAt = std::chrono::steady_clock::now();
					Op->WaitDomain = State->Scheduler->IsInline() ? nullptr : State.get();
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
				State->RetainedBytes += BatchBytes;
				for (auto& Op : Operations) Op->Admitted = true;
			}
			for (auto& Op : Operations) if (Op->IsCanceled()) Op->RequestCancel();
			State->Pump(); return Handle;
		}
		auto Get(std::span<const FCacheGetRequest> Requests, IRequestOwner& Owner, FCacheGetCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> override
		try
		{
			if (Requests.empty() || !Callback)
				return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Invalid asynchronous cache batch."});
			if (Requests.size() > State->Capacity)
			{
				ObserveCache({}, ECacheObservation::CapacityRejected);
				return std::unexpected(FCacheError{ECacheError::Capacity, "Asynchronous cache batch exceeds capacity."});
			}
			std::vector<std::shared_ptr<FCacheOperation>> Ops; Ops.reserve(Requests.size());
			for (uint32 I = 0; I < Requests.size(); ++I)
			{
				if (Requests[I].RequiredValues.size() > 4096) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Selection is unbounded."});
				auto Op = std::make_shared<FCacheOperation>(); Op->Index = I; Op->GetRequest = Requests[I]; Op->GetRequest.Stats = nullptr;
				Op->GetRequest.RequiredValues = {}; Op->Ids.assign(Requests[I].RequiredValues.begin(), Requests[I].RequiredValues.end());
				Op->GetCallback = Callback; Ops.push_back(std::move(Op));
			}
			auto Result = Admit(std::move(Ops), Owner);
			if (!Result && Result.error().Code == ECacheError::Capacity)
				for (const auto& Request : Requests) ObserveCache(Request.Key, ECacheObservation::CapacityRejected);
			return Result;
		}
		catch (...) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache batch admission failed."}); }
		auto Put(std::span<const FCachePutRequest> Requests, IRequestOwner& Owner, FCachePutCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> override
		try
		{
			if (Requests.empty() || !Callback)
				return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Invalid asynchronous cache batch."});
			if (Requests.size() > State->Capacity)
			{
				ObserveCache({}, ECacheObservation::CapacityRejected);
				return std::unexpected(FCacheError{ECacheError::Capacity, "Asynchronous cache batch exceeds capacity."});
			}
			std::vector<std::shared_ptr<FCacheOperation>> Ops; Ops.reserve(Requests.size());
			for (uint32 I = 0; I < Requests.size(); ++I)
			{ auto Op = std::make_shared<FCacheOperation>(); Op->Index = I; Op->PutRequest = Requests[I]; Op->PutRequest->Stats = nullptr; Op->PutCallback = Callback; Ops.push_back(std::move(Op)); }
			auto Result = Admit(std::move(Ops), Owner);
			if (!Result && Result.error().Code == ECacheError::Capacity)
				for (const auto& Request : Requests) ObserveCache(Request.Record.GetKey(), ECacheObservation::CapacityRejected);
			return Result;
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
	auto CreateAsyncCache(std::shared_ptr<ICache> Cache, std::shared_ptr<IBuildScheduler> Scheduler, FCacheDispatchLimits Limits) -> std::shared_ptr<IAsyncCache>
	{ return std::make_shared<Private::FAsyncCache>(std::move(Cache), std::move(Scheduler), Limits); }
}
