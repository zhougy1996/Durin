#include "CacheDispatchPrivate.h"
#include "RequestOwnerPrivate.h"
#include "CacheDiagnosticsPrivate.h"
#include "DerivedDataCache/DerivedDataCache.h"
#include "Logging/LogMacros.h"

namespace Durin::DerivedData::Private
{
	struct FCacheOperation;
	struct FAsyncCacheState;
	struct FCacheOperation : FRequestCompletion
	{
		std::atomic<bool> Started = false, Canceled = false;
		bool Active = false; // Protected by Cache->Mutex.
		std::shared_ptr<FAsyncCacheState> Cache;
		FCacheGetBackendRequest GetRequest;
		std::optional<FCachePutBackendRequest> PutRequest;
		FOnCacheGetComplete GetCallback;
		FOnCachePutComplete PutCallback;
		std::string Name;
		uint64 UserData = 0;
		std::chrono::steady_clock::time_point QueuedAt;
		uint64 QueuedTurn = 0; // Protected by Cache->Mutex.
		uint64 RetainedBytes = 0;
		auto IsCanceled() const -> bool { return Canceled.load() || Group->IsCanceled(); }
		auto OnFinished() -> void override;
		auto RequestCancel() -> void override;
		auto Finish(FCacheGetResult GetResult, FCachePutResult PutResult) -> void;
		auto Run() -> void;

	};
	struct FAsyncCacheState
	{
		std::mutex Mutex;
		bool Closed = false;
		uint32 Active = 0, Concurrency = 2, Capacity = 4096;
		uint64 DispatchTurn = 0;
		uint64 RetainedBytes = 0, MaximumRetainedBytes = 512ull * 1024 * 1024;
		std::shared_ptr<ICacheBackend> Backend;
		std::shared_ptr<IBuildScheduler> Scheduler;
		std::vector<TRefCountPtr<FCacheOperation>> Pending, All;
		auto Pump() -> void;
	};
	auto CanceledError() -> FCacheError { return {ECacheError::Canceled, "Asynchronous cache request canceled or dropped."}; }
	auto FCacheOperation::OnFinished() -> void
	{
		std::lock_guard Lock(Cache->Mutex);
		if (std::erase_if(Cache->All, [&](const auto& Op) { return Op.GetReference() == this; })) Cache->RetainedBytes -= RetainedBytes;
	}
	auto FCacheOperation::Finish(FCacheGetResult GetResult, FCachePutResult PutResult) -> void
	{
		if (IsCanceled()) { GetResult = std::unexpected(CanceledError()); PutResult = std::unexpected(CanceledError()); }
		const FCacheError* Error = PutRequest ? (!PutResult ? &PutResult.error() : nullptr) : (!GetResult ? &GetResult.error() : nullptr);
		if (Error && Error->Code != ECacheError::Canceled)
			DURIN_WARN_CATEGORY("DerivedData", "Cache {} [{}]: {}", PutRequest ? "write" : "read",
				(PutRequest ? PutRequest->Record.GetKey() : GetRequest.Key).ToString(), Error->Diagnostic);
		{ std::lock_guard Lock(Cache->Mutex); if (Active) { --Cache->Active; Active = false; } }
		// IO capacity is available before business callbacks; accounting remains until End completes.
		if (!Cache->Scheduler->IsInline()) Cache->Pump();
		Complete([&] {
			try {
				if (GetCallback) {
					FCacheGetResponse Response{.Name = Name, .UserData = UserData};
					if (GetResult && *GetResult) { Response.Status = EStatus::Ok; Response.Record = std::move(**GetResult); }
					else Response = FCacheGetRequest{.Key = GetRequest.Key, .Name = Name, .UserData = UserData}.MakeResponse(
						!GetResult && GetResult.error().Code == ECacheError::Canceled ? EStatus::Canceled : EStatus::Error);
					GetCallback(std::move(Response));
				}
				else if (PutCallback) PutCallback({.Name = Name, .Record = PutResult ? *PutResult : PutRequest->Record, .Key = PutRequest->Record.GetKey(),
					.UserData = UserData, .Status = PutResult ? EStatus::Ok : PutResult.error().Code == ECacheError::Canceled ? EStatus::Canceled : EStatus::Error});
			} catch (...) {}
			GetCallback = {};
			PutCallback = {};
			GetRequest = {};
			PutRequest.reset();
		});

	}
	auto FCacheOperation::RequestCancel() -> void
	{
		Canceled.store(true);
		bool Pending = false, CompleteNow = false;
		{ std::lock_guard Lock(Cache->Mutex); Pending = std::erase_if(Cache->Pending, [&](const auto& Op) { return Op.GetReference() == this; }) != 0; CompleteNow = (Pending || Active) && !Started.exchange(true); }
		if (CompleteNow) { Finish(std::unexpected(CanceledError()), std::unexpected(CanceledError())); }
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
			if (!IsCanceled())
			{
				if (PutRequest)
				{
					PutRequest->ShouldCancel = [this] { return IsCanceled(); };
					Saved = Cache->Backend->Put(*PutRequest);
				}
				else
				{
					GetRequest.Stats = &Stats;
					GetRequest.ShouldCancel = [this] { return IsCanceled(); };
					Result = Cache->Backend->Get(GetRequest);
				}
			}
		}
		catch (...) { Result = std::unexpected(FCacheError{ECacheError::StorageFailure, "Asynchronous cache backend threw."}); Saved = std::unexpected(Result.error()); }
		Finish(std::move(Result), std::move(Saved));
	}
	auto FAsyncCacheState::Pump() -> void
	{
		const bool Inline = Scheduler->IsInline();
		for (;;)
		{
			TRefCountPtr<FCacheOperation> Op;
			std::optional<FRequestBarrier> Publication;
			{
				std::lock_guard Lock(Mutex);
				if ((!Inline && Active >= Concurrency) || Pending.empty()) return;
				// FIFO admission means only the oldest item's age needs checking.
				// Unsigned turn subtraction also preserves age across counter wrap.
				auto Next = Pending.begin();
				if (DispatchTurn - (*Next)->QueuedTurn < 32)
					Next = std::ranges::min_element(Pending, {}, [](const auto& Item) { return Item->Group->GetPriority(); });
				// Retain the owner before cancellation can end the selected operation.
				Publication.emplace(*(*Next)->Group, ERequestBarrierFlags::Priority);
				Op = std::move(*Next); Pending.erase(Next); ++Active; Op->Active = true;
				++DispatchTurn;
			}
			std::shared_ptr<FCacheOperation> Dispatch;
			try
			{
				// The scheduled closure owns this same request; dropping it completes queued work.
				Dispatch = std::shared_ptr<FCacheOperation>(Op.GetReference(), [KeepAlive = Op](FCacheOperation*) { KeepAlive->RequestCancel(); });
				auto Scheduled = Scheduler->Schedule({"DerivedDataCacheIO", Op->Group->GetPriority()}, [Dispatch] { Dispatch->Run(); });
				{ std::lock_guard Lock(Op->Mutex); Op->Scheduled = Scheduled; }
				Op->Changed.notify_all();
				if (Scheduled) for (;;) {
					const auto Priority = Op->Group->GetPriority(); Scheduled->SetPriority(Priority);
					if (Priority == Op->Group->GetPriority()) break;
				}
			}
			catch (...)
			{
				if (!Op->Started.exchange(true)) Op->Finish(
					std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache dispatch threw."}),
					std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache dispatch threw."}));
			}
		}
	}
	class FCache final : public ICache
	{
	public:
		std::shared_ptr<FAsyncCacheState> State;
		FCacheResourceLimits ResourceLimits;
		FCache(std::shared_ptr<ICacheBackend> Cache, std::shared_ptr<IBuildScheduler> Scheduler, FCacheDispatchLimits Limits, FCacheResourceLimits ResourceLimits)
			: State(std::make_shared<FAsyncCacheState>()), ResourceLimits(ResourceLimits)
		{
			State->Backend = Cache ? std::move(Cache) : std::shared_ptr<ICacheBackend>(&GetCacheBackend(), [](ICacheBackend*) {});
			State->Scheduler = Scheduler ? std::move(Scheduler) : CreateTaskCacheScheduler();
			State->Concurrency = std::clamp(Limits.Concurrency, 1u, 64u); State->Capacity = std::clamp(Limits.Capacity, 1u, 4096u);
			State->MaximumRetainedBytes = Limits.MaximumRetainedBytes;
			State->Pending.reserve(State->Capacity); State->All.reserve(State->Capacity);
		}
		auto Get(std::span<const FCacheGetRequest> Requests, IRequestOwner& Owner, FOnCacheGetComplete&& Completion) -> void override;
		auto Put(std::span<const FCachePutRequest> Requests, IRequestOwner& Owner, FOnCachePutComplete&& Completion) -> void override;
		~FCache() override { Close(); Drain(); }
	private:
		auto Admit(const std::vector<TRefCountPtr<FCacheOperation>>& Operations, IRequestOwner& Owner)
			-> std::expected<void, FCacheError>
		{
			uint64 BatchBytes = 0;
			for (auto& Op : Operations)
			{
				const auto Charge = [&](uint64 Bytes) {
					if (Bytes > State->MaximumRetainedBytes - BatchBytes) return false;
					BatchBytes += Bytes; Op->RetainedBytes += Bytes; return true;
				};
				// Fixed request/table overhead is bounded by item and record limits.
				// This budget covers queued data, not total allocation overhead.
				if (!Charge(Op->GetRequest.Policy.GetValuePolicies().size() * sizeof(FCacheValuePolicy)))
					return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-request byte capacity exhausted."});
				if (!Op->PutRequest) continue;
				const auto& Record = Op->PutRequest->Record;
				std::vector<FSharedByteBuffer> Backings;
				const auto Retain = [&](FSharedByteBuffer Buffer) {
					if (std::ranges::any_of(Backings, [&](const auto& Other) { return Buffer.SharesStorageWith(Other); })) return true;
					if (!Charge(Buffer.GetRetainedCapacityBytes())) return false;
					Backings.push_back(std::move(Buffer)); return true;
				};
				for (const auto& Value : Record.GetValues()) if (!Retain(Value.Value.GetData()))
					return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-payload byte capacity exhausted."});
				if (!Retain(Record.GetMeta().GetBytes())) return std::unexpected(FCacheError{ECacheError::Capacity, "Cache retained-metadata byte capacity exhausted."});
			}
			{
				std::unique_lock Lock(State->Mutex);
				if (State->Closed) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Asynchronous cache is closed."});
				if (Operations.size() > State->Capacity - State->All.size() || BatchBytes > State->MaximumRetainedBytes - State->RetainedBytes)
					return std::unexpected(FCacheError{ECacheError::Capacity, "Asynchronous cache request capacity exhausted."});
				State->All.reserve(State->All.size() + Operations.size());
				State->Pending.reserve(State->Pending.size() + Operations.size());
				try { for (auto& Op : Operations)
				{
					Op->Cache = State; Op->QueuedTurn = State->DispatchTurn; Op->QueuedAt = std::chrono::steady_clock::now();
					Owner.Begin(Op.GetReference()); Op->Group = &Owner;
					State->All.push_back(Op); State->Pending.push_back(Op);
				} }
				catch (...) {
					for (auto& Op : Operations) {
						std::erase(State->All, Op); std::erase(State->Pending, Op);
					}
					Lock.unlock();
					for (auto& Op : Operations) if (Op->Group) Op->Complete([] {});
					throw;
				}
				State->RetainedBytes += BatchBytes;
			}
			for (auto& Op : Operations) if (Op->IsCanceled()) Op->RequestCancel();
			State->Pump(); return {};
		}
		auto Submit(const std::vector<TRefCountPtr<FCacheOperation>>& Operations, IRequestOwner& Owner) -> void
		{
			std::expected<void, FCacheError> Result;
			try { Result = Admit(Operations, Owner); }
			catch (...) { Result = std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache admission failed."}); }
			if (Result) return;
			const auto Status = Owner.IsCanceled() || Result.error().Code == ECacheError::Canceled ? EStatus::Canceled : EStatus::Error;
			FOwnerAccess::Invoke(Owner, [&] {
				for (const auto& Op : Operations) {
					if (Result.error().Code == ECacheError::Capacity)
						ObserveCache(Op->PutRequest ? Op->PutRequest->Record.GetKey() : Op->GetRequest.Key, ECacheObservation::CapacityRejected);
					try {
						if (Op->GetCallback) Op->GetCallback(FCacheGetRequest{.Key = Op->GetRequest.Key, .Name = Op->Name, .UserData = Op->UserData}.MakeResponse(Status));
						else Op->PutCallback({.Name = Op->Name, .Record = Op->PutRequest->Record, .Key = Op->PutRequest->Record.GetKey(), .UserData = Op->UserData, .Status = Status});
					} catch (...) {}
				}
			});
		}
		auto Close() -> void
		{ std::vector<TRefCountPtr<FCacheOperation>> Snapshot; { std::lock_guard Lock(State->Mutex); State->Closed = true; Snapshot = State->All; } for (const auto& Op : Snapshot) Op->RequestCancel(); }
		auto Drain() -> ERequestWaitResult
		{
			for (;;)
			{
				TRefCountPtr<FCacheOperation> Op;
				{ std::lock_guard Lock(State->Mutex); if (State->All.empty()) return ERequestWaitResult::Completed;
					if (std::ranges::any_of(State->All, [](const auto& Item) { return FRequestOwnerScope::Blocks(Item.GetReference()); })) return ERequestWaitResult::WouldBlock;
					Op = State->All.front();
				}
				if (Op->WaitInternal() != ERequestWaitResult::Completed) return ERequestWaitResult::WouldBlock;
			}
		}
	};
}

namespace Durin::DerivedData::Private
{
	auto FCache::Get(std::span<const FCacheGetRequest> Requests, IRequestOwner& Owner, FOnCacheGetComplete&& Completion) -> void
	{
		if (Requests.empty()) return;
		require(static_cast<bool>(Completion));
		FRequestBarrier Submission(Owner);
		if (Requests.size() > State->Capacity) {
			ObserveCache({}, ECacheObservation::CapacityRejected);
			std::vector<FCacheGetResponse> Responses; Responses.reserve(Requests.size());
			const auto Status = Owner.IsCanceled() ? EStatus::Canceled : EStatus::Error;
			for (const auto& Request : Requests) Responses.push_back(Request.MakeResponse(Status));
			FOwnerAccess::Invoke(Owner, [&] {
				for (auto& Response : Responses) {
					try { Completion(std::move(Response)); } catch (...) {}
				}
			});
			return;
		}
		auto Notify = std::make_shared<FOnCacheGetComplete>(std::move(Completion));
		std::vector<TRefCountPtr<FCacheOperation>> Operations;
		Operations.reserve(Requests.size());
		for (const auto& Request : Requests) {
			auto Op = MakeRefCount<FCacheOperation>();
			Op->GetRequest = {.Key = Request.Key, .RecordLimits = ResourceLimits.Record,
				.MaximumEncodedBytes = ResourceLimits.MaximumEncodedBytes, .MaximumWorkingSetBytes = ResourceLimits.MaximumWorkingSetBytes, .Policy = Request.Policy};
			Op->Name = Request.Name; Op->UserData = Request.UserData; Op->GetCallback = [Notify](FCacheGetResponse Response) { (*Notify)(std::move(Response)); };
			Operations.push_back(std::move(Op));
		}
		Submit(Operations, Owner);
	}
	auto FCache::Put(std::span<const FCachePutRequest> Requests, IRequestOwner& Owner, FOnCachePutComplete&& Completion) -> void
	{
		if (Requests.empty()) return;
		require(static_cast<bool>(Completion));
		FRequestBarrier Submission(Owner);
		if (Requests.size() > State->Capacity) {
			ObserveCache({}, ECacheObservation::CapacityRejected);
			std::vector<FCachePutResponse> Responses; Responses.reserve(Requests.size());
			const auto Status = Owner.IsCanceled() ? EStatus::Canceled : EStatus::Error;
			for (const auto& Request : Requests) Responses.push_back(Request.MakeResponse(Status));
			FOwnerAccess::Invoke(Owner, [&] {
				for (auto& Response : Responses) {
					try { Completion(std::move(Response)); } catch (...) {}
				}
			});
			return;
		}
		auto Notify = std::make_shared<FOnCachePutComplete>(std::move(Completion));
		std::vector<TRefCountPtr<FCacheOperation>> Operations;
		Operations.reserve(Requests.size());
		for (const auto& Request : Requests) {
			auto Op = MakeRefCount<FCacheOperation>();
			Op->PutRequest = FCachePutBackendRequest{Request.Record, ResourceLimits.MaximumEncodedBytes, ResourceLimits.Record, ResourceLimits.MaximumWorkingSetBytes};
			Op->PutRequest->Policy = Request.Policy;
			Op->Name = Request.Name; Op->UserData = Request.UserData; Op->PutCallback = [Notify](FCachePutResponse Response) { (*Notify)(std::move(Response)); };
			Operations.push_back(std::move(Op));
		}
		Submit(Operations, Owner);
	}
	auto CreateCacheForBackend(std::shared_ptr<ICacheBackend> Backend, std::shared_ptr<IBuildScheduler> Scheduler,
		FCacheDispatchLimits Limits, FCacheResourceLimits ResourceLimits) -> std::shared_ptr<ICache>
	{
		return std::make_shared<FCache>(std::move(Backend), Scheduler ? std::move(Scheduler) : CreateInlineBuildScheduler(), Limits, ResourceLimits);
	}
}
namespace Durin::DerivedData
{
	auto FCacheGetRequest::MakeResponse(EStatus Status) const -> FCacheGetResponse
	{
		FCacheGetResponse Response{.Name = Name, .UserData = UserData, .Status = Status};
		auto Record = FCacheRecordBuilder(Key).Build();
		if (Record) Response.Record = std::move(*Record);
		return Response;
	}
	auto FCachePutRequest::MakeResponse(EStatus Status) const -> FCachePutResponse
	{ return {.Name = Name, .Record = Record, .Key = Record.GetKey(), .UserData = UserData, .Status = Status}; }
	auto GetCache() -> ICache&
	{
		static const auto Cache = Private::CreateCacheForBackend({}, CreateTaskCacheScheduler());
		return *Cache;
	}
}
