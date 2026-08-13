#include "AsyncImport.h"

#include "Threading/Task.h"

namespace Durin::Asset::Import
{
	namespace
	{
		thread_local const FTaskCancellationToken* GImportCancellationToken = nullptr;

		auto GetPreparePlanAttribution() -> FTaskAttribution
		{
			static const FTaskAttribution Attribution = RegisterTaskAttribution("AssetImport", "PreparePlan");
			return Attribution;
		}

		auto GetPublishPlanAttribution() -> FTaskAttribution
		{
			static const FTaskAttribution Attribution = RegisterTaskAttribution("AssetImport", "PublishPlan");
			return Attribution;
		}

		class FScopedImportCancellationToken
		{
		public:
			explicit FScopedImportCancellationToken(
				const FTaskCancellationToken& InToken)
				: Prior(GImportCancellationToken)
			{
				GImportCancellationToken = &InToken;
			}
			~FScopedImportCancellationToken()
			{
				GImportCancellationToken = Prior;
			}

		private:
			const FTaskCancellationToken* Prior = nullptr;
		};

		auto IsTerminalTaskState(ETaskState State) -> bool
		{
			return State == ETaskState::Succeeded || State == ETaskState::Failed
				|| State == ETaskState::Canceled;
		}

		auto MakeTerminalResult(
			EImportDiagnosticCategory Category,
			std::string Message) -> FImportPlanResult
		{
			FImportPlanResult Result;
			Result.Message = Message;
			Result.Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = Category,
				.Phase = "async-preparation",
				.SourceIdentity = "root",
				.OutputIdentity = "request",
				.Message = std::move(Message)});
			FinalizeImportDiagnostics(
				Result.Diagnostics, "async-preparation", "root", "request");
			return Result;
		}
	}

	struct FAsyncImportRequestState
	{
		mutable std::mutex Mutex;
		uint64 Serial = 0;
		std::string OwnerId;
		std::string ProviderId;
		FTaskCancellationSource Cancellation;
		FTaskScope Scope;
		FTaskHandle ProducerTask;
		FTaskHandle Task;
		EAsyncImportPlanStatus Status = EAsyncImportPlanStatus::Pending;
		std::optional<FImportPlanResult> Result;
		bool bNoticeQueued = false;
		bool bMailboxDrained = false;
		bool bTaken = false;
	};

	class FAsyncImportCoordinator
		{
		public:
			auto Launch(FImportPlanRequest Request, std::string_view OwnerId)
				-> FAsyncImportPlanHandle
			{
				auto State = std::make_shared<FAsyncImportRequestState>();
				State->Scope = CreateTaskScope();
				State->Serial = NextSerial.fetch_add(1, std::memory_order_relaxed);
				State->OwnerId = OwnerId.empty()
					? std::format("request:{}", State->Serial) : std::string(OwnerId);
				State->ProviderId = Request.ProviderId;
				Request.Progress = nullptr;

				std::shared_ptr<FAsyncImportRequestState> Superseded;
				bool bAccepted = false;
				{
					std::lock_guard Lock(Mutex);
					const bool bProviderOpen = State->ProviderId.empty()
						|| !ClosedProviders.contains(State->ProviderId);
					bAccepted = bAdmissionOpen && bProviderOpen;
					if (const auto It = LatestByOwner.find(State->OwnerId);
						It != LatestByOwner.end())
					{
						if (const auto Prior = Requests.find(It->second);
							Prior != Requests.end()) Superseded = Prior->second;
					}
					LatestByOwner[State->OwnerId] = State->Serial;
					Requests.emplace(State->Serial, State);
				}
				if (Superseded) RequestCancellation(Superseded);

				if (!bAccepted)
				{
					(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
					std::lock_guard StateLock(State->Mutex);
					State->Status = EAsyncImportPlanStatus::Rejected;
					State->Result = MakeTerminalResult(
						EImportDiagnosticCategory::AsyncFailure,
						"Asynchronous import admission is closed; work was not accepted.");
					State->bMailboxDrained = true;
					return FAsyncImportPlanHandle(State);
				}

				FTaskLaunchOptions Options;
				Options.CancellationToken = State->Cancellation.GetToken();
				Options.Attribution = GetPreparePlanAttribution();
				Options.Scope = State->Scope.GetToken();
				constexpr uint64 EstimatedImportPlanResultBytes = 64ull * 1'024ull;
				auto Producer = LaunchUniqueCancelableTask<FImportPlanResult>(
					"AssetImport.PreparePlan",
					[Request = std::move(Request)](
						const FTaskCancellationToken& Token) mutable {
						FScopedImportCancellationToken TokenScope(Token);
						if (Token.IsCancellationRequested())
						{
							return MakeTerminalResult(
								EImportDiagnosticCategory::Canceled,
								"Asynchronous import preparation was canceled before it started.");
						}
						FImportPlanResult Result = CreateImportPlan(Request, GetProviderRegistry());
						if (Token.IsCancellationRequested())
						{
							return MakeTerminalResult(
								EImportDiagnosticCategory::Canceled,
								"Asynchronous import preparation was canceled.");
						}
						return Result;
					}, Options, EstimatedImportPlanResultBytes);

				if (!Producer.IsValid())
				{
					(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
					std::lock_guard StateLock(State->Mutex);
					if (!State->bNoticeQueued)
					{
						State->Status = EAsyncImportPlanStatus::Rejected;
						State->Result = MakeTerminalResult(
							EImportDiagnosticCategory::AsyncFailure,
							"The task scheduler rejected asynchronous import work; it was never accepted.");
						State->bMailboxDrained = true;
					}
					return FAsyncImportPlanHandle(State);
				}

				const FTaskHandle ProducerTask = Producer.GetTaskHandle();
				FTaskContinuationOptions PublisherOptions;
				PublisherOptions.Attribution = GetPublishPlanAttribution();
				FTaskHandle Publisher = ConsumeThenOutcome(
					std::move(Producer),
					"AssetImport.PublishPlan",
					[this, State](FUniqueTaskOutcome<FImportPlanResult>&& Outcome) {
						bool bQueueNotice = false;
						{
							std::lock_guard StateLock(State->Mutex);
							if (!State->bNoticeQueued)
							{
								if (Outcome.State == ETaskState::Succeeded && Outcome.Result)
								{
									State->Result = std::move(Outcome.Result);
								}
								else if (Outcome.State == ETaskState::Canceled)
								{
									State->Result = MakeTerminalResult(
										EImportDiagnosticCategory::Canceled,
										"Asynchronous import preparation was canceled before producing a result.");
								}
								else
								{
									State->Result = MakeTerminalResult(
										EImportDiagnosticCategory::AsyncFailure,
										Outcome.Diagnostic.empty()
											? "Asynchronous import preparation terminated without producing a result."
											: std::format("Asynchronous import preparation failed: {}", Outcome.Diagnostic));
								}
								State->bNoticeQueued = true;
								bQueueNotice = true;
							}
						}
						if (bQueueNotice) QueueNotice(State->Serial);
						(void)State->Scope.Close(ETaskScopeCloseMode::Drain);
					}, PublisherOptions);
				bool bCancelProducer = false;
				{
					std::lock_guard StateLock(State->Mutex);
					State->ProducerTask = ProducerTask;
					State->Task = Publisher.IsValid() ? Publisher : ProducerTask;
					if (!Publisher.IsValid())
					{
						State->Cancellation.RequestCancellation();
						bCancelProducer = true;
						if (!State->bNoticeQueued)
						{
							State->Status = EAsyncImportPlanStatus::Rejected;
							State->Result = MakeTerminalResult(
								EImportDiagnosticCategory::AsyncFailure,
								"The task scheduler rejected asynchronous import result publication; it was never accepted.");
							State->bMailboxDrained = true;
						}
					}
				}
				if (bCancelProducer)
				{
					(void)CancelTask(ProducerTask);
					(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
				}
				return FAsyncImportPlanHandle(State);
			}

			auto Drain() -> uint64
			{
				std::vector<uint64> Notices;
				{
					std::lock_guard Lock(Mutex);
					Notices.assign(Mailbox.begin(), Mailbox.end());
					Mailbox.clear();
				}

				std::unordered_set<uint64> Candidates(Notices.begin(), Notices.end());
				std::vector<uint64> DeferredNotices;

				uint64 Drained = 0;
				for (const uint64 Serial : Candidates)
				{
					std::shared_ptr<FAsyncImportRequestState> State;
					bool bCurrent = false;
					{
						std::lock_guard Lock(Mutex);
						const auto It = Requests.find(Serial);
						if (It == Requests.end()) continue;
						State = It->second;
						const auto Latest = LatestByOwner.find(State->OwnerId);
						bCurrent = Latest != LatestByOwner.end() && Latest->second == Serial;
					}
					std::lock_guard StateLock(State->Mutex);
					if (State->bMailboxDrained) continue;
					if (State->Task.IsValid()
						&& !IsTerminalTaskState(State->Task.GetState()))
					{
						DeferredNotices.push_back(Serial);
						continue;
					}
					if (!bCurrent)
					{
						State->Result.reset();
						State->Status = EAsyncImportPlanStatus::Superseded;
					}
					else if (!State->Task.IsValid()
						|| State->Task.GetState() == ETaskState::Canceled
						|| (!State->Result->Diagnostics.empty()
							&& State->Result->Diagnostics.back().Category
								== EImportDiagnosticCategory::Canceled))
						State->Status = EAsyncImportPlanStatus::Canceled;
					else State->Status = State->Result->bSucceeded
						? EAsyncImportPlanStatus::Succeeded : EAsyncImportPlanStatus::Failed;
					State->bMailboxDrained = true;
					++Drained;
				}
				if (!DeferredNotices.empty())
				{
					std::lock_guard Lock(Mutex);
					for (const uint64 Serial : DeferredNotices) Mailbox.push_back(Serial);
				}
				return Drained;
			}

			auto Take(const FAsyncImportPlanHandle& Handle, FImportPlanResult& OutResult)
				-> EAsyncImportPlanStatus
			{
				if (!Handle.State) return EAsyncImportPlanStatus::Invalid;
				EAsyncImportPlanStatus Status;
				{
					std::lock_guard StateLock(Handle.State->Mutex);
					Status = Handle.State->Status;
					if (!Handle.State->bMailboxDrained || Handle.State->bTaken)
						return Handle.State->bTaken
							? Status : EAsyncImportPlanStatus::Pending;
					if (Handle.State->Result)
						OutResult = std::move(*Handle.State->Result);
					Handle.State->Result.reset();
					Handle.State->bTaken = true;
				}
				RemoveIfOwned(Handle.State);
				return Status;
			}

			auto Cancel(const FAsyncImportPlanHandle& Handle) -> bool
			{
				return Handle.State && RequestCancellation(Handle.State);
			}

			auto CancelAndDrain(const FAsyncImportPlanHandle& Handle)
				-> EAsyncImportPlanStatus
			{
				if (!Handle.State) return EAsyncImportPlanStatus::Invalid;
				RequestCancellation(Handle.State);
				(void)Handle.State->Scope.Wait();
				Drain();
				FImportPlanResult Discarded;
				return Take(Handle, Discarded);
			}

			auto CancelAndDrainOwner(std::string_view OwnerId) -> void
			{
				CancelAndDrainMatching([&](const FAsyncImportRequestState& State) {
					return State.OwnerId == OwnerId;
				});
			}

			auto CancelAndDrainProvider(std::string_view ProviderId) -> void
			{
				CloseProvider(ProviderId);
				CancelAndDrainMatching([&](const FAsyncImportRequestState& State) {
					return State.ProviderId == ProviderId;
				});
			}

			auto CancelAndDrainAll() -> void
			{
				CancelAndDrainMatching([](const FAsyncImportRequestState&) { return true; });
			}

			auto OpenProvider(std::string_view ProviderId) -> void
			{
				std::lock_guard Lock(Mutex);
				ClosedProviders.erase(std::string(ProviderId));
			}

			auto CloseProvider(std::string_view ProviderId) -> void
			{
				std::lock_guard Lock(Mutex);
				ClosedProviders.emplace(ProviderId);
			}

			auto CloseAdmission() -> void
			{
				std::lock_guard Lock(Mutex);
				bAdmissionOpen = false;
			}

		private:
			template<typename TPredicate>
			auto CancelAndDrainMatching(TPredicate&& Predicate) -> void
			{
				std::vector<FAsyncImportPlanHandle> Handles;
				{
					std::lock_guard Lock(Mutex);
					for (const auto& [Serial, State] : Requests)
						if (Predicate(*State)) Handles.push_back(FAsyncImportPlanHandle(State));
				}
				for (const FAsyncImportPlanHandle& Handle : Handles) Cancel(Handle);
				for (const FAsyncImportPlanHandle& Handle : Handles)
					(void)Handle.State->Scope.Wait();
				Drain();
				for (const FAsyncImportPlanHandle& Handle : Handles)
				{
					FImportPlanResult Discarded;
					(void)Take(Handle, Discarded);
				}
			}

			auto RequestCancellation(
				const std::shared_ptr<FAsyncImportRequestState>& State) -> bool
			{
				FTaskHandle Task;
				bool bQueueNotice = false;
				{
					std::lock_guard StateLock(State->Mutex);
					if (State->bTaken || State->bMailboxDrained) return false;
					State->Cancellation.RequestCancellation();
					Task = State->ProducerTask.IsValid() ? State->ProducerTask : State->Task;
					if (!State->bNoticeQueued)
					{
						State->Result = MakeTerminalResult(
							EImportDiagnosticCategory::Canceled,
							"Asynchronous import preparation was canceled.");
						State->bNoticeQueued = true;
						bQueueNotice = true;
					}
				}
				if (bQueueNotice) QueueNotice(State->Serial);
				const bool bCanceled = Task.IsValid() ? CancelTask(Task) : true;
				(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
				return bCanceled;
			}

			auto QueueNotice(uint64 Serial) -> void
			{
				std::lock_guard Lock(Mutex);
				Mailbox.push_back(Serial);
			}

			auto RemoveIfOwned(
				const std::shared_ptr<FAsyncImportRequestState>& State) -> void
			{
				std::lock_guard Lock(Mutex);
				const auto It = Requests.find(State->Serial);
				if (It != Requests.end() && It->second == State) Requests.erase(It);
			}

			std::mutex Mutex;
			std::unordered_map<uint64, std::shared_ptr<FAsyncImportRequestState>> Requests;
			std::unordered_map<std::string, uint64> LatestByOwner;
			std::unordered_set<std::string> ClosedProviders;
			std::deque<uint64> Mailbox;
			std::atomic<uint64> NextSerial{1};
			bool bAdmissionOpen = true;
		};

	namespace
	{
		auto GetAsyncImportCoordinator() -> FAsyncImportCoordinator&
		{
			static FAsyncImportCoordinator Coordinator;
			return Coordinator;
		}
	}

	auto FAsyncImportPlanHandle::GetSerial() const -> uint64
	{
		return State ? State->Serial : 0;
	}

	auto FAsyncImportPlanHandle::GetStatus() const -> EAsyncImportPlanStatus
	{
		if (!State) return EAsyncImportPlanStatus::Invalid;
		std::lock_guard Lock(State->Mutex);
		return State->Status;
	}

	auto LaunchAsyncImportPlan(
		FImportPlanRequest Request,
		std::string_view OwnerId) -> FAsyncImportPlanHandle
	{
		return GetAsyncImportCoordinator().Launch(std::move(Request), OwnerId);
	}

	auto DrainAsyncImportCompletionMailbox() -> uint64
	{
		return GetAsyncImportCoordinator().Drain();
	}

	auto TryTakeAsyncImportPlanResult(
		const FAsyncImportPlanHandle& Handle,
		FImportPlanResult& OutResult) -> EAsyncImportPlanStatus
	{
		return GetAsyncImportCoordinator().Take(Handle, OutResult);
	}

	auto CancelAsyncImport(const FAsyncImportPlanHandle& Handle) -> bool
	{
		return GetAsyncImportCoordinator().Cancel(Handle);
	}

	auto CancelAndDrainAsyncImport(const FAsyncImportPlanHandle& Handle)
		-> EAsyncImportPlanStatus
	{
		return GetAsyncImportCoordinator().CancelAndDrain(Handle);
	}

	auto CancelAndDrainAsyncImportsForOwner(std::string_view OwnerId) -> void
	{
		GetAsyncImportCoordinator().CancelAndDrainOwner(OwnerId);
	}

	auto CancelAndDrainAsyncImportsForProvider(std::string_view ProviderId) -> void
	{
		GetAsyncImportCoordinator().CancelAndDrainProvider(ProviderId);
	}

	auto CancelAndDrainAllAsyncImports() -> void
	{
		GetAsyncImportCoordinator().CancelAndDrainAll();
	}

	auto OpenAsyncImportProviderAdmission(std::string_view ProviderId) -> void
	{
		GetAsyncImportCoordinator().OpenProvider(ProviderId);
	}

	auto CloseAsyncImportProviderAdmission(std::string_view ProviderId) -> void
	{
		GetAsyncImportCoordinator().CloseProvider(ProviderId);
	}

	auto CloseAsyncImportAdmission() -> void
	{
		GetAsyncImportCoordinator().CloseAdmission();
	}

	auto IsImportCancellationRequested() -> bool
	{
		return GImportCancellationToken
			&& GImportCancellationToken->IsCancellationRequested();
	}
}
