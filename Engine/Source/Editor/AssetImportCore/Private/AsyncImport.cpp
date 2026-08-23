#include "AsyncImport.h"
#include "ImportService.h"
#include "ImportJob.h"

#include "Threading/Task.h"

namespace Durin::Asset
{
	auto IsImportOperationTransitionAllowed(
		EImportOperationState From, EImportOperationState To) -> bool
	{
		if (From == To) return false;
		switch (From)
		{
		case EImportOperationState::Queued:
			return To == EImportOperationState::Running
				|| To == EImportOperationState::Canceling
				|| To == EImportOperationState::Canceled
				|| To == EImportOperationState::Superseded
				|| To == EImportOperationState::Rejected;
		case EImportOperationState::Running:
			return To == EImportOperationState::Canceling
				|| To == EImportOperationState::Finalizing
				|| To == EImportOperationState::Succeeded
				|| To == EImportOperationState::Failed
				|| To == EImportOperationState::Canceled
				|| To == EImportOperationState::Superseded
				|| To == EImportOperationState::Rejected;
		case EImportOperationState::Canceling:
			return To == EImportOperationState::Canceled
				|| To == EImportOperationState::Failed
				|| To == EImportOperationState::Superseded;
		case EImportOperationState::Finalizing:
			return To == EImportOperationState::Succeeded
				|| To == EImportOperationState::Failed;
		default:
			return false;
		}
	}

	namespace
	{
		constexpr size_t MaximumProgressDiagnosticBytes = 1'024;
		thread_local const FTaskCancellationToken* GImportCancellationToken = nullptr;
		thread_local uint32 GImportWorkerPreparationDepth = 0;

		auto ToOperationState(EAsyncImportPlanStatus Status) -> EImportOperationState
		{
			switch (Status)
			{
			case EAsyncImportPlanStatus::Succeeded: return EImportOperationState::Succeeded;
			case EAsyncImportPlanStatus::Failed: return EImportOperationState::Failed;
			case EAsyncImportPlanStatus::Canceled: return EImportOperationState::Canceled;
			case EAsyncImportPlanStatus::Superseded: return EImportOperationState::Superseded;
			case EAsyncImportPlanStatus::Rejected: return EImportOperationState::Rejected;
			default: return EImportOperationState::Pending;
			}
		}

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
				++GImportWorkerPreparationDepth;
			}
			~FScopedImportCancellationToken()
			{
				check(GImportWorkerPreparationDepth > 0);
				--GImportWorkerPreparationDepth;
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

		auto MakeOperationOutcome(
			EImportOperationState State,
			std::string_view Diagnostic,
			const FImportPlanResult* Result = nullptr) -> FImportOutcome
		{
			FImportOutcome Outcome{
				.State = State,
				.Diagnostic = std::string(Diagnostic)};
			if (Result) Outcome.Diagnostics = Result->Diagnostics;
			return Outcome;
		}
	}

	struct FAsyncImportProgressState
	{
		mutable std::mutex Mutex;
		FImportOperationSnapshot Snapshot;
		std::deque<FImportOperationSnapshot> History;

		auto Publish(const FImportProgressEvent& Event) noexcept -> void
		{
			std::lock_guard Lock(Mutex);
			if (Snapshot.IsTerminal()) return;
			if (static_cast<uint8>(Event.Phase) < static_cast<uint8>(Snapshot.Phase)) return;

			const bool bNewPhase = Event.Phase != Snapshot.Phase;
			if (Event.Phase == EImportPhase::Publication
				&& Snapshot.State == EImportOperationState::Pending)
			{
				Snapshot.State = EImportOperationState::Finalizing;
				Snapshot.bCancelable = false;
			}
			if (bNewPhase)
			{
				Snapshot.Phase = Event.Phase;
				Snapshot.CompletedWork = 0;
				Snapshot.TotalWork = 0;
				Snapshot.Progress.reset();
			}
			Snapshot.SourceIdentity = Event.SourceIdentity;
			Snapshot.OutputIdentity = Event.OutputIdentity;
			if (Event.TotalWork > 0)
			{
				Snapshot.TotalWork = std::max(Snapshot.TotalWork, Event.TotalWork);
				Snapshot.CompletedWork = std::max(
					Snapshot.CompletedWork, std::min(Event.CompletedWork, Snapshot.TotalWork));
				Snapshot.Progress = static_cast<float>(Snapshot.CompletedWork)
					/ static_cast<float>(Snapshot.TotalWork);
			}
			else if (bNewPhase || Event.CompletedWork == 0)
			{
				Snapshot.CompletedWork = Event.CompletedWork;
				Snapshot.TotalWork = 0;
				Snapshot.Progress.reset();
			}
			if (!Event.Message.empty())
				Snapshot.Diagnostic = Event.Message.substr(0, MaximumProgressDiagnosticBytes);
			++Snapshot.Revision;
			Record(bNewPhase || Event.State != EImportProgressState::Started);
		}

		auto MarkState(EImportOperationState State, std::string_view Diagnostic = {}) -> void
		{
			std::lock_guard Lock(Mutex);
			if (Snapshot.IsTerminal()) return;
			Snapshot.State = State;
			Snapshot.bCancelable = State == EImportOperationState::Pending;
			if (!Diagnostic.empty())
				Snapshot.Diagnostic = std::string(Diagnostic.substr(0, MaximumProgressDiagnosticBytes));
			++Snapshot.Revision;
			Record(true);
		}

		auto SetBackground(bool bRunningInBackground) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (Snapshot.IsTerminal()) return false;
			if (Snapshot.bRunningInBackground == bRunningInBackground) return true;
			Snapshot.bRunningInBackground = bRunningInBackground;
			++Snapshot.Revision;
			Record(true);
			return true;
		}

		auto CopySnapshot() const -> FImportOperationSnapshot
		{
			std::lock_guard Lock(Mutex);
			return Snapshot;
		}

		auto CopyHistory() const -> std::vector<FImportOperationSnapshot>
		{
			std::lock_guard Lock(Mutex);
			return {History.begin(), History.end()};
		}

	private:
		auto Record(bool bForceAppend) -> void
		{
			if (!bForceAppend && !History.empty()
				&& History.back().Phase == Snapshot.Phase
				&& History.back().SourceIdentity == Snapshot.SourceIdentity
				&& History.back().OutputIdentity == Snapshot.OutputIdentity)
				History.back() = Snapshot;
			else History.push_back(Snapshot);
			while (History.size() > MaximumAsyncImportProgressHistory) History.pop_front();
		}
	};

	class FAsyncImportProgressReporter final : public IImportProgressReporter
	{
	public:
		explicit FAsyncImportProgressReporter(std::shared_ptr<FAsyncImportProgressState> InState)
			: State(std::move(InState)) {}

		auto Report(const FImportProgressEvent& Event) noexcept -> void override
		{
			State->Publish(Event);
		}

	private:
		std::shared_ptr<FAsyncImportProgressState> State;
	};

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
		std::shared_ptr<FAsyncImportProgressState> Progress;
		std::optional<FImportPlanResult> Result;
		std::optional<FImportOutcome> Outcome;
		bool bNoticeQueued = false;
		bool bMailboxDrained = false;
		bool bTaken = false;
		bool bKeepOperationOpenAfterPlan = false;
		bool bOperationCompleted = false;
	};

	struct FAsyncImportExecutionState
	{
		mutable std::mutex Mutex;
		FTaskCancellationSource Cancellation;
		FTaskScope Scope;
		FTaskScopeToken OperationScope;
		bool bOwnsScope = true;
		FTaskHandle Task;
		EAsyncImportPlanStatus Status = EAsyncImportPlanStatus::Pending;
		std::optional<FDetachedImportBuildResult> Result;
		bool bConsumed = false;
	};

	struct FImportJobOperationState
	{
		mutable std::mutex Mutex;
		uint64 OperationId = 0;
		FImportOperationOwner Owner;
		std::string ProviderId;
		EImportOperationLifetime Lifetime = EImportOperationLifetime::EditorOperation;
		FTaskCancellationSource Cancellation;
		FTaskScope Scope;
		FTaskHandle WorkerTask;
		FTaskHandle CompletionTask;
		std::shared_ptr<FAsyncImportProgressState> Progress;
		std::unique_ptr<IImportJob> Job;
		FProviderLease ProviderLease;
		std::optional<FImportJobWorkerResult> WorkerResult;
		std::optional<FImportOutcome> Outcome;
		bool bWorkerActive = false;
		bool bCompletionRejected = false;
		bool bReadyQueued = false;
		bool bFinalizing = false;
		bool bSupersedeRequested = false;
	};

	class FImportJobEditorContextImpl final : public FImportJobEditorContext
	{
	public:
		explicit FImportJobEditorContextImpl(
			const std::shared_ptr<FImportJobOperationState>& InState,
			IImportProgressReporter& InProgress)
			: State(InState), Progress(InProgress) {}

		auto IsCancellationRequested() const -> bool override
		{
			return State->Cancellation.IsCancellationRequested();
		}

		auto EnterFinalization() -> bool override
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Outcome || State->Cancellation.IsCancellationRequested()) return false;
			const EImportOperationState Current = State->Progress->CopySnapshot().State;
			if (Current == EImportOperationState::Finalizing) return true;
			if (!IsImportOperationTransitionAllowed(
				Current, EImportOperationState::Finalizing)) return false;
			State->bFinalizing = true;
			State->Progress->MarkState(EImportOperationState::Finalizing);
			return true;
		}

		auto GetProgressReporter() -> IImportProgressReporter& override
		{
			return Progress;
		}

	private:
		std::shared_ptr<FImportJobOperationState> State;
		IImportProgressReporter& Progress;
	};

	class FImportJobRegistry
	{
	public:
		auto Submit(std::unique_ptr<IImportJob> Job, std::string_view Title)
			-> FImportOperationHandle
		{
			CheckImportEditorMutationAllowed("SubmitImportJob");
			if (!Job) return {};
			auto State = std::make_shared<FImportJobOperationState>();
			State->OperationId = NextOperationId.fetch_add(1, std::memory_order_relaxed);
			State->Owner = Job->GetOwner();
			if (State->Owner.OwnerId.empty())
				State->Owner.OwnerId = std::format("job:{}", State->OperationId);
			State->ProviderId = Job->GetProviderId();
			const bool bRequiresLegacyProviderLease = Job->RequiresLegacyProviderLease();
			State->Lifetime = Job->GetLifetime();
			State->Scope = CreateTaskScope();
			State->Progress = std::make_shared<FAsyncImportProgressState>();
			State->Progress->Snapshot = {
				.OperationId = State->OperationId,
				.Revision = 1,
				.OwnerId = State->Owner.OwnerId,
				.ProviderId = State->ProviderId,
				.Title = Title.empty() ? "Importing asset" : std::string(Title),
				.Phase = EImportPhase::Snapshot,
				.State = EImportOperationState::Queued,
				.SourceIdentity = "root",
				.OutputIdentity = "request"};

			if (bRequiresLegacyProviderLease && !State->ProviderId.empty())
				State->ProviderLease = GetImportService().FindImporter(State->ProviderId);
			State->Job = std::move(Job);

			std::shared_ptr<FImportJobOperationState> Superseded;
			bool bAccepted = false;
			{
				std::lock_guard Lock(Mutex);
				bAccepted = bAdmissionOpen
					&& !ClosedProviders.contains(State->ProviderId)
					&& (!bRequiresLegacyProviderLease
						|| State->ProviderId.empty() || State->ProviderLease);
				if (bAccepted && State->Lifetime == EImportOperationLifetime::EphemeralPreview)
				{
					if (const auto It = LatestPreviewByOwner.find(State->Owner.OwnerId);
						It != LatestPreviewByOwner.end())
						if (const auto Prior = Operations.find(It->second);
							Prior != Operations.end()) Superseded = Prior->second;
					LatestPreviewByOwner[State->Owner.OwnerId] = State->OperationId;
				}
				Operations.emplace(State->OperationId, State);
			}
			if (Superseded) RequestCancel(Superseded, true);
			if (!bAccepted)
			{
				(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
				Complete(State, {
					.State = EImportOperationState::Rejected,
					.Diagnostic = State->ProviderId.empty()
						? "Asset import job admission is closed."
						: "Asset import provider is unavailable or closed."});
			}
			else QueueReady(State);
			return FImportOperationHandle(State);
		}

		auto Pump(uint32 MaximumEditorSteps) -> uint32
		{
			CheckImportEditorMutationAllowed("PumpImportOperations");
			uint32 Advanced = 0;
			std::vector<std::shared_ptr<FImportJobOperationState>> Deferred;
			while (Advanced < MaximumEditorSteps)
			{
				std::shared_ptr<FImportJobOperationState> State;
				{
					std::lock_guard Lock(Mutex);
					if (Ready.empty()) break;
					const uint64 Id = Ready.front();
					Ready.pop_front();
					const auto It = Operations.find(Id);
					if (It == Operations.end()) continue;
					State = It->second;
				}
				{
					std::lock_guard Lock(State->Mutex);
					State->bReadyQueued = false;
					if (State->Outcome) continue;
					if (State->bWorkerActive)
					{
						if ((State->bCompletionRejected
							|| State->Cancellation.IsCancellationRequested())
							&& State->WorkerTask.IsComplete()
							&& (!State->CompletionTask.IsValid()
								|| State->CompletionTask.IsComplete()))
						{
							State->bWorkerActive = false;
							State->WorkerResult.emplace(FImportJobWorkerResult{
								.bSucceeded = false,
								.bCanceled = State->Cancellation.IsCancellationRequested(),
								.Diagnostic = State->Cancellation.IsCancellationRequested()
									? "Import worker completion was canceled with its operation."
									: "The task scheduler rejected import completion publication."});
						}
						else
						{
							Deferred.push_back(State);
							continue;
						}
					}
				}
				Advance(State);
				++Advanced;
			}
			for (const auto& State : Deferred) QueueReady(State);
			return Advanced;
		}

		auto Cancel(const FImportOperationHandle& Handle) -> bool
		{
			return Handle.JobState && RequestCancel(Handle.JobState, false);
		}

		auto RunInline(std::unique_ptr<IImportJob> Job, std::string_view Title)
			-> FImportOutcome
		{
			CheckImportEditorMutationAllowed("RunImportJobInline");
			if (!Job) return {.State = EImportOperationState::Rejected,
				.Diagnostic = "Inline import requires a job."};
			auto State = std::make_shared<FImportJobOperationState>();
			State->OperationId = NextOperationId.fetch_add(1, std::memory_order_relaxed);
			State->Owner = Job->GetOwner();
			State->ProviderId = Job->GetProviderId();
			const bool bRequiresLegacyProviderLease = Job->RequiresLegacyProviderLease();
			State->Lifetime = Job->GetLifetime();
			State->Progress = std::make_shared<FAsyncImportProgressState>();
			State->Progress->Snapshot = {
				.OperationId = State->OperationId,
				.Revision = 1,
				.OwnerId = State->Owner.OwnerId,
				.ProviderId = State->ProviderId,
				.Title = Title.empty() ? "Inline asset import" : std::string(Title),
				.State = EImportOperationState::Running};
			if (bRequiresLegacyProviderLease && !State->ProviderId.empty())
				State->ProviderLease = GetImportService().FindImporter(State->ProviderId);
			if (bRequiresLegacyProviderLease
				&& !State->ProviderId.empty() && !State->ProviderLease)
				return {.State = EImportOperationState::Rejected,
					.Diagnostic = "Inline import provider is unavailable."};
			State->Job = std::move(Job);
			auto Reporter = std::make_shared<FAsyncImportProgressReporter>(State->Progress);
			FImportJobEditorContextImpl EditorContext(State, *Reporter);
			std::unique_ptr<IImportJobValue> Previous;
			for (uint32 StepIndex = 0; StepIndex < 1'024; ++StepIndex)
			{
				FImportJobEditorAdvance Next = State->Job->AdvanceOnEditor(
					EditorContext, std::move(Previous));
				if (!Next.IsValid())
				{
					State->Job.reset();
					State->ProviderLease = {};
					return {.State = EImportOperationState::Failed,
						.Diagnostic = "Inline import job returned an invalid continuation."};
				}
				if (Next.Kind == FImportJobEditorAdvance::EKind::Terminal)
				{
					FImportOutcome Outcome = std::move(*Next.Outcome);
					const EImportOperationState Current = State->Progress->CopySnapshot().State;
					if (!IsImportOperationTransitionAllowed(Current, Outcome.State))
						Outcome = {.State = EImportOperationState::Failed,
							.Diagnostic = "Inline import job attempted an invalid terminal transition."};
					State->Job.reset();
					State->ProviderLease = {};
					State->Progress->MarkState(Outcome.State, Outcome.Diagnostic);
					return Outcome;
				}
				FImportJobWorkerStep Worker = std::move(*Next.Worker);
				if (Worker.EstimatedResultBytes > MaximumImportJobDetachedValueBytes)
				{
					State->Job.reset();
					State->ProviderLease = {};
					return {.State = EImportOperationState::Rejected,
						.Diagnostic = "Import worker step exceeds the detached-value memory limit."};
				}
				const FTaskCancellationToken Token = State->Cancellation.GetToken();
				FImportJobWorkerResult Result;
				{
					FScopedImportCancellationToken TokenScope(Token);
					FImportJobWorkerContext WorkerContext{Token, *Reporter};
					Result = State->Job->ExecuteWorkerStep(
						WorkerContext, std::move(Worker.Input));
				}
				if (!Result.bSucceeded || Result.bCanceled)
				{
					FImportOutcome Outcome = State->Job->CompensateWorkerFailureOnEditor(
						EditorContext, std::move(Result));
					State->Job.reset();
					State->ProviderLease = {};
					return Outcome;
				}
				Previous = std::move(Result.Value);
			}
			State->Job.reset();
			State->ProviderLease = {};
			return {.State = EImportOperationState::Failed,
				.Diagnostic = "Inline import job exceeded the continuation limit."};
		}

		auto HasClaim(std::string_view Identity) const -> bool
		{
			std::vector<std::shared_ptr<FImportJobOperationState>> Snapshot;
			{
				std::lock_guard Lock(Mutex);
				for (const auto& [Id, State] : Operations) Snapshot.push_back(State);
			}
			return std::ranges::any_of(Snapshot, [Identity](const auto& State) {
				std::lock_guard StateLock(State->Mutex);
				return !State->Outcome && std::ranges::find(
					State->Owner.ConflictIdentities, Identity)
					!= State->Owner.ConflictIdentities.end();
			});
		}

		auto ReleasePreviewOwner(std::string_view OwnerId) -> void
		{
			CancelAndDrainMatching([OwnerId](const FImportJobOperationState& State) {
				return State.Lifetime == EImportOperationLifetime::EphemeralPreview
					&& State.Owner.OwnerId == OwnerId;
			});
		}

		auto CancelAndDrain(const FImportOperationHandle& Handle) -> void
		{
			const std::shared_ptr<FImportJobOperationState> Target = Handle.JobState;
			if (!Target) return;
			CancelAndDrainMatching([&](const FImportJobOperationState& State) {
				return &State == Target.get();
			});
		}

		auto CancelAndDrainOwner(std::string_view OwnerId) -> void
		{
			CancelAndDrainMatching([OwnerId](const FImportJobOperationState& State) {
				return State.Owner.OwnerId == OwnerId;
			});
		}

		auto CancelAndDrainProvider(std::string_view ProviderId) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				ClosedProviders.emplace(ProviderId);
			}
			CancelAndDrainMatching([ProviderId](const FImportJobOperationState& State) {
				return State.ProviderId == ProviderId;
			});
		}

		auto CancelAndDrainAll() -> void
		{
			CancelAndDrainMatching([](const FImportJobOperationState&) { return true; });
		}

		auto OpenProvider(std::string_view ProviderId) -> void
		{
			std::lock_guard Lock(Mutex);
			ClosedProviders.erase(std::string(ProviderId));
		}

		auto CloseAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			bAdmissionOpen = false;
		}

	private:
		auto QueueReady(const std::shared_ptr<FImportJobOperationState>& State) -> void
		{
			{
				std::lock_guard StateLock(State->Mutex);
				if (State->Outcome || State->bReadyQueued) return;
				State->bReadyQueued = true;
			}
			std::lock_guard Lock(Mutex);
			Ready.push_back(State->OperationId);
		}

		auto Advance(const std::shared_ptr<FImportJobOperationState>& State) -> void
		{
			bool bFinalizing = false;
			bool bSuperseded = false;
			{
				std::lock_guard Lock(State->Mutex);
				bFinalizing = State->bFinalizing;
				bSuperseded = State->bSupersedeRequested;
			}
			if (State->Cancellation.IsCancellationRequested() && !bFinalizing)
			{
				Complete(State, {.State = bSuperseded
					? EImportOperationState::Superseded : EImportOperationState::Canceled,
					.Diagnostic = bSuperseded
						? "Asset import job was superseded." : "Asset import job was canceled."});
				return;
			}
			const EImportOperationState Current = State->Progress->CopySnapshot().State;
			if (Current == EImportOperationState::Queued)
				State->Progress->MarkState(EImportOperationState::Running);

			std::unique_ptr<IImportJobValue> Previous;
			std::optional<FImportJobWorkerResult> WorkerFailure;
			{
				std::lock_guard Lock(State->Mutex);
				if (State->WorkerResult)
				{
					if (!State->WorkerResult->bSucceeded || State->WorkerResult->bCanceled)
					{
						if (State->Cancellation.IsCancellationRequested())
							State->WorkerResult->bCanceled = true;
						WorkerFailure.emplace(std::move(*State->WorkerResult));
						State->WorkerResult.reset();
					}
					else
					{
						Previous = std::move(State->WorkerResult->Value);
						State->WorkerResult.reset();
					}
				}
			}
			auto Reporter = std::make_shared<FAsyncImportProgressReporter>(State->Progress);
			FImportJobEditorContextImpl Context(State, *Reporter);
			if (WorkerFailure)
			{
				Complete(State, State->Job->CompensateWorkerFailureOnEditor(
					Context, std::move(*WorkerFailure)));
				return;
			}
			FImportJobEditorAdvance Next = State->Job->AdvanceOnEditor(
				Context, std::move(Previous));
			if (!Next.IsValid())
			{
				Complete(State, {.State = EImportOperationState::Failed,
					.Diagnostic = "Import job returned an invalid editor continuation."});
				return;
			}
			if (Next.Kind == FImportJobEditorAdvance::EKind::Terminal)
			{
				Complete(State, std::move(*Next.Outcome));
				return;
			}
			LaunchWorker(State, std::move(*Next.Worker), std::move(Reporter));
		}

		auto LaunchWorker(
			const std::shared_ptr<FImportJobOperationState>& State,
			FImportJobWorkerStep Step,
			std::shared_ptr<FAsyncImportProgressReporter> Reporter) -> void
		{
			if (Step.EstimatedResultBytes > MaximumImportJobDetachedValueBytes)
			{
				Complete(State, {.State = EImportOperationState::Rejected,
					.Diagnostic = "Import worker step exceeds the detached-value memory limit."});
				return;
			}
			if (State->Cancellation.IsCancellationRequested())
			{
				Complete(State, {.State = EImportOperationState::Canceled,
					.Diagnostic = "Asset import job was canceled before its worker step."});
				return;
			}
			FTaskLaunchOptions Options;
			Options.CancellationToken = State->Cancellation.GetToken();
			Options.Attribution = Step.Attribution;
			Options.Scope = State->Scope.GetToken();
			auto Producer = LaunchUniqueCancelableTask<FImportJobWorkerResult>(
				Step.Name.c_str(),
				[State, Input = std::move(Step.Input), Reporter](
					const FTaskCancellationToken& Token) mutable {
					FScopedImportCancellationToken TokenScope(Token);
					FImportJobWorkerContext Context{Token, *Reporter};
					if (Token.IsCancellationRequested())
						return FImportJobWorkerResult{.bSucceeded = false, .bCanceled = true,
							.Diagnostic = "Import worker step was canceled before execution."};
					return State->Job->ExecuteWorkerStep(Context, std::move(Input));
				}, Options, Step.EstimatedResultBytes);
			if (!Producer.IsValid())
			{
				Complete(State, {.State = EImportOperationState::Rejected,
					.Diagnostic = "The task scheduler rejected an import worker step."});
				return;
			}
			const FTaskHandle ProducerTask = Producer.GetTaskHandle();
			{
				std::lock_guard Lock(State->Mutex);
				State->bWorkerActive = true;
				State->WorkerTask = ProducerTask;
			}
			FTaskContinuationOptions CompletionOptions;
			CompletionOptions.Attribution = Step.Attribution;
			FTaskHandle Completion = ConsumeThenOutcome(
				std::move(Producer), "AssetImport.JobWorkerCompletion",
				[this, State](FUniqueTaskOutcome<FImportJobWorkerResult>&& Outcome) {
					{
						std::lock_guard Lock(State->Mutex);
						State->bWorkerActive = false;
						if (Outcome.State == ETaskState::Succeeded && Outcome.Result)
							State->WorkerResult.emplace(std::move(*Outcome.Result));
						else State->WorkerResult.emplace(FImportJobWorkerResult{
							.bSucceeded = false,
							.bCanceled = Outcome.State == ETaskState::Canceled,
							.Diagnostic = Outcome.Diagnostic.empty()
								? "Import worker step ended without a result." : Outcome.Diagnostic});
					}
					QueueReady(State);
				}, CompletionOptions);
			{
				std::lock_guard Lock(State->Mutex);
				State->CompletionTask = Completion;
			}
			if (!Completion.IsValid())
			{
				State->Cancellation.RequestCancellation();
				(void)CancelTask(ProducerTask);
				(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
				{
					std::lock_guard Lock(State->Mutex);
					State->bCompletionRejected = true;
				}
				QueueReady(State);
			}
		}

		auto RequestCancel(
			const std::shared_ptr<FImportJobOperationState>& State,
			bool bSuperseded) -> bool
		{
			FTaskHandle Task;
			bool bWorkerActive = false;
			{
				std::lock_guard Lock(State->Mutex);
				if (State->Outcome || State->bFinalizing) return false;
				State->Cancellation.RequestCancellation();
				State->bSupersedeRequested = State->bSupersedeRequested || bSuperseded;
				Task = State->WorkerTask;
				bWorkerActive = State->bWorkerActive;
			}
			State->Progress->MarkState(EImportOperationState::Canceling,
				bSuperseded ? "Superseding asset import job." : "Canceling asset import job.");
			if (Task.IsValid()) (void)CancelTask(Task);
			(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
			if (!bWorkerActive)
			{
				if (bSuperseded)
					Complete(State, {.State = EImportOperationState::Superseded,
						.Diagnostic = "Asset import job was superseded."});
				else QueueReady(State);
			}
			else QueueReady(State);
			return true;
		}

		auto Complete(
			const std::shared_ptr<FImportJobOperationState>& State,
			FImportOutcome Outcome) -> bool
		{
			if (!Outcome.IsTerminal()) return false;
			if (Outcome.PublishedAssetIdentities.size() > MaximumImportOutcomeIdentities
				|| Outcome.OrphanAssetIdentities.size() > MaximumImportOutcomeIdentities)
			{
				Outcome = {.State = EImportOperationState::Failed,
					.Diagnostic = "Import outcome exceeded its retained identity limit."};
			}
			{
				std::lock_guard Lock(State->Mutex);
				if (State->Outcome) return false;
				const EImportOperationState Current = State->Progress->CopySnapshot().State;
				if (!IsImportOperationTransitionAllowed(Current, Outcome.State)) return false;
				State->Outcome.emplace(Outcome);
				State->WorkerResult.reset();
				State->Job.reset();
				State->ProviderLease = {};
			}
			(void)State->Scope.Close(Outcome.State == EImportOperationState::Canceled
				|| Outcome.State == EImportOperationState::Superseded
				? ETaskScopeCloseMode::Cancel : ETaskScopeCloseMode::Drain);
			State->Progress->MarkState(Outcome.State, Outcome.Diagnostic);
			std::lock_guard Lock(Mutex);
			TerminalOrder.push_back(State->OperationId);
			while (TerminalOrder.size() > MaximumRetainedImportOperations)
			{
				const uint64 Oldest = TerminalOrder.front();
				TerminalOrder.pop_front();
				Operations.erase(Oldest);
			}
			return true;
		}

		template<typename TPredicate>
		auto CancelAndDrainMatching(TPredicate&& Predicate) -> void
		{
			std::vector<std::shared_ptr<FImportJobOperationState>> Matches;
			{
				std::lock_guard Lock(Mutex);
				for (const auto& [Id, State] : Operations) Matches.push_back(State);
			}
			std::erase_if(Matches, [&Predicate](const auto& State) {
				std::lock_guard StateLock(State->Mutex);
				return State->Outcome || !Predicate(*State);
			});
			for (const auto& State : Matches) RequestCancel(State, false);
			for (const auto& State : Matches) (void)State->Scope.Wait();
			for (const auto& State : Matches)
			{
				FTaskHandle Completion;
				{
					std::lock_guard Lock(State->Mutex);
					Completion = State->CompletionTask;
				}
				if (Completion.IsValid()) (void)WaitTask(Completion);
				{
					std::lock_guard Lock(State->Mutex);
					if (!State->Outcome && State->bWorkerActive
						&& (!Completion.IsValid() || Completion.IsComplete()))
					{
						State->bWorkerActive = false;
						if (!State->WorkerResult)
							State->WorkerResult.emplace(FImportJobWorkerResult{
								.bSucceeded = false,
								.bCanceled = true,
								.Diagnostic = "Import worker was canceled by a drain barrier."});
					}
				}
			}
			for (const auto& State : Matches) QueueReady(State);
			for (;;)
			{
				(void)Pump(std::numeric_limits<uint32>::max());
				const bool bAllTerminal = std::ranges::all_of(Matches, [](const auto& State) {
					std::lock_guard Lock(State->Mutex);
					return State->Outcome.has_value();
				});
				if (bAllTerminal) break;
				std::this_thread::yield();
			}
		}

		mutable std::mutex Mutex;
		std::unordered_map<uint64, std::shared_ptr<FImportJobOperationState>> Operations;
		std::unordered_map<std::string, uint64> LatestPreviewByOwner;
		std::unordered_set<std::string> ClosedProviders;
		std::deque<uint64> Ready;
		std::deque<uint64> TerminalOrder;
		std::atomic<uint64> NextOperationId{1ull << 63};
		bool bAdmissionOpen = true;
	};

	class FAsyncImportCoordinator
		{
		public:
			auto BeginExtendedOperation(
				std::string_view OwnerId,
				std::string_view ProviderId,
				std::string_view Title) -> FAsyncImportPlanHandle
			{
				auto State = std::make_shared<FAsyncImportRequestState>();
				State->Scope = CreateTaskScope();
				State->Serial = NextSerial.fetch_add(1, std::memory_order_relaxed);
				State->OwnerId = OwnerId.empty()
					? std::format("request:{}", State->Serial) : std::string(OwnerId);
				State->ProviderId = ProviderId;
				State->bKeepOperationOpenAfterPlan = true;
				State->bMailboxDrained = true;
				State->bTaken = true;
				State->Status = EAsyncImportPlanStatus::Succeeded;
				State->Progress = std::make_shared<FAsyncImportProgressState>();
				State->Progress->Snapshot = {
					.OperationId = State->Serial,
					.Revision = 1,
					.OwnerId = State->OwnerId,
					.ProviderId = State->ProviderId,
					.Title = Title.empty() ? "Importing asset" : std::string(Title),
					.Phase = EImportPhase::CandidateBuild,
					.State = EImportOperationState::Pending,
					.SourceIdentity = "root",
					.OutputIdentity = "request"};

				std::shared_ptr<FAsyncImportRequestState> Superseded;
				bool bAccepted = false;
				{
					std::lock_guard Lock(Mutex);
					const bool bProviderOpen = State->ProviderId.empty()
						|| !ClosedProviders.contains(State->ProviderId);
					bAccepted = bAdmissionOpen && bProviderOpen;
					if (const auto It = LatestByOwner.find(State->OwnerId);
						It != LatestByOwner.end())
						if (const auto Prior = Requests.find(It->second);
							Prior != Requests.end()) Superseded = Prior->second;
					LatestByOwner[State->OwnerId] = State->Serial;
					Requests.emplace(State->Serial, State);
				}
				if (Superseded)
				{
					bool bExtendedTaken = false;
					{
						std::lock_guard StateLock(Superseded->Mutex);
						bExtendedTaken = Superseded->bKeepOperationOpenAfterPlan
							&& Superseded->bTaken;
					}
					RequestCancellation(Superseded);
					if (bExtendedTaken)
						CompleteOperation(Superseded,
							EImportOperationState::Superseded,
							"Asset import operation was superseded.");
				}
				if (!bAccepted)
				{
					(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
					State->Status = EAsyncImportPlanStatus::Rejected;
					State->bOperationCompleted = true;
					State->Outcome = MakeOperationOutcome(
						EImportOperationState::Rejected,
						"Asynchronous import admission is closed; work was not accepted.");
					State->Progress->MarkState(EImportOperationState::Rejected,
						"Asynchronous import admission is closed; work was not accepted.");
				}
				return FAsyncImportPlanHandle(State);
			}

			auto Launch(FImportPlanRequest Request, std::string_view OwnerId,
				bool bKeepOperationOpenAfterPlan)
				-> FAsyncImportPlanHandle
			{
				auto State = std::make_shared<FAsyncImportRequestState>();
				State->Scope = CreateTaskScope();
				State->Serial = NextSerial.fetch_add(1, std::memory_order_relaxed);
				State->OwnerId = OwnerId.empty()
					? std::format("request:{}", State->Serial) : std::string(OwnerId);
				State->ProviderId = Request.ProviderId;
				State->bKeepOperationOpenAfterPlan = bKeepOperationOpenAfterPlan;
				State->Progress = std::make_shared<FAsyncImportProgressState>();
				State->Progress->Snapshot = {
					.OperationId = State->Serial,
					.Revision = 1,
					.OwnerId = State->OwnerId,
					.ProviderId = State->ProviderId,
					.Title = "Preparing asset import",
					.Phase = EImportPhase::Snapshot,
					.State = EImportOperationState::Pending,
					.SourceIdentity = "root",
					.OutputIdentity = "request"};
				auto ProgressReporter =
					std::make_shared<FAsyncImportProgressReporter>(State->Progress);
				Request.Progress = ProgressReporter.get();

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
					State->bOperationCompleted = true;
					State->Outcome = MakeOperationOutcome(
						EImportOperationState::Rejected, State->Result->Message, &*State->Result);
					State->Progress->MarkState(
						EImportOperationState::Rejected, State->Result->Message);
					return FAsyncImportPlanHandle(State);
				}

				FTaskLaunchOptions Options;
				Options.CancellationToken = State->Cancellation.GetToken();
				Options.Attribution = GetPreparePlanAttribution();
				Options.Scope = State->Scope.GetToken();
				constexpr uint64 EstimatedImportPlanResultBytes = 64ull * 1'024ull;
				auto Producer = LaunchUniqueCancelableTask<FImportPlanResult>(
					"AssetImport.PreparePlan",
					[Request = std::move(Request), ProgressReporter = std::move(ProgressReporter)](
						const FTaskCancellationToken& Token) mutable {
						(void)ProgressReporter;
						FScopedImportCancellationToken TokenScope(Token);
						if (Token.IsCancellationRequested())
						{
							return MakeTerminalResult(
								EImportDiagnosticCategory::Canceled,
								"Asynchronous import preparation was canceled before it started.");
						}
						FImportPlanResult Result = GetImportService().CreateImportPlan(Request);
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
						State->bOperationCompleted = true;
						State->Outcome = MakeOperationOutcome(
							EImportOperationState::Rejected, State->Result->Message, &*State->Result);
						State->Progress->MarkState(
							EImportOperationState::Rejected, State->Result->Message);
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
						// Extended operations keep admission open for detached execution tasks.
						// Their terminal publication or a cancellation barrier closes the scope.
						if (!State->bKeepOperationOpenAfterPlan)
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
							State->bOperationCompleted = true;
							State->Outcome = MakeOperationOutcome(
								EImportOperationState::Rejected, State->Result->Message, &*State->Result);
							State->Progress->MarkState(
								EImportOperationState::Rejected, State->Result->Message);
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
					const bool bKeepOpen = State->bKeepOperationOpenAfterPlan
						&& State->Status == EAsyncImportPlanStatus::Succeeded;
					if (!bKeepOpen)
					{
						State->bOperationCompleted = true;
						const EImportOperationState TerminalState = ToOperationState(State->Status);
						const std::string_view Diagnostic = State->Result
							? std::string_view(State->Result->Message) : std::string_view{};
						State->Outcome = MakeOperationOutcome(
							TerminalState, Diagnostic, State->Result ? &*State->Result : nullptr);
						State->Progress->MarkState(TerminalState, Diagnostic);
					}
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
				if (!Handle.State->bKeepOperationOpenAfterPlan
					|| Status != EAsyncImportPlanStatus::Succeeded)
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
				if (Handle.State->bKeepOperationOpenAfterPlan)
					CompleteOperation(Handle.State, EImportOperationState::Canceled,
						"Asset import operation was canceled and drained.");
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
			friend class FAsyncImportPlanHandle;

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
					if (Handle.State->bKeepOperationOpenAfterPlan)
						CompleteOperation(Handle.State, EImportOperationState::Canceled,
							"Asset import operation was canceled and drained.");
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
					if (State->bOperationCompleted) return false;
					if (State->bKeepOperationOpenAfterPlan
						&& (State->bTaken || (State->bMailboxDrained
							&& State->Status == EAsyncImportPlanStatus::Succeeded)))
					{
						State->Cancellation.RequestCancellation();
						State->Progress->MarkState(EImportOperationState::Canceling,
							"Canceling asset import execution.");
						(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
						return true;
					}
					if (State->bTaken || State->bMailboxDrained) return false;
					State->Cancellation.RequestCancellation();
					State->Progress->MarkState(EImportOperationState::Canceling,
						"Canceling asset import preparation.");
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

			auto CompleteOperation(
				const std::shared_ptr<FAsyncImportRequestState>& State,
				EImportOperationState TerminalState,
				std::string_view Diagnostic) -> bool
			{
				if (!State || (TerminalState != EImportOperationState::Succeeded
					&& TerminalState != EImportOperationState::Failed
					&& TerminalState != EImportOperationState::Canceled
					&& TerminalState != EImportOperationState::Superseded
					&& TerminalState != EImportOperationState::Rejected)) return false;
				const FImportOperationSnapshot Snapshot = State->Progress->CopySnapshot();
				if (!IsImportOperationTransitionAllowed(Snapshot.State, TerminalState))
					return false;
				{
					std::lock_guard StateLock(State->Mutex);
					if (State->bOperationCompleted) return false;
					State->bOperationCompleted = true;
					State->Outcome = MakeOperationOutcome(TerminalState, Diagnostic);
				}
				(void)State->Scope.Close(TerminalState == EImportOperationState::Canceled
					? ETaskScopeCloseMode::Cancel : ETaskScopeCloseMode::Drain);
				State->Progress->MarkState(TerminalState, Diagnostic);
				RemoveIfOwned(State);
				return true;
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

		auto GetImportJobRegistry() -> FImportJobRegistry&
		{
			static FImportJobRegistry Registry;
			return Registry;
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

	auto FAsyncImportPlanHandle::GetOperationHandle() const -> FImportOperationHandle
	{
		return FImportOperationHandle(State);
	}

	auto FImportOperationHandle::GetOperationId() const -> uint64
	{
		return LegacyState ? LegacyState->Serial
			: JobState ? JobState->OperationId : 0;
	}

	auto FImportOperationHandle::GetSnapshot() const -> FImportOperationSnapshot
	{
		const auto Progress = LegacyState ? LegacyState->Progress
			: JobState ? JobState->Progress : nullptr;
		return Progress ? Progress->CopySnapshot() : FImportOperationSnapshot{};
	}

	auto FImportOperationHandle::GetProgressHistory() const
		-> std::vector<FImportOperationSnapshot>
	{
		const auto Progress = LegacyState ? LegacyState->Progress
			: JobState ? JobState->Progress : nullptr;
		return Progress ? Progress->CopyHistory()
			: std::vector<FImportOperationSnapshot>{};
	}

	auto FImportOperationHandle::TryGetOutcome(FImportOutcome& OutOutcome) const -> bool
	{
		if (LegacyState)
		{
			std::lock_guard Lock(LegacyState->Mutex);
			if (!LegacyState->Outcome) return false;
			OutOutcome = *LegacyState->Outcome;
			return true;
		}
		if (JobState)
		{
			std::lock_guard Lock(JobState->Mutex);
			if (!JobState->Outcome) return false;
			OutOutcome = *JobState->Outcome;
			return true;
		}
		return false;
	}

	auto FImportOperationHandle::RequestCancel() const -> bool
	{
		if (LegacyState)
			return GetAsyncImportCoordinator().Cancel(FAsyncImportPlanHandle(LegacyState));
		return JobState && GetImportJobRegistry().Cancel(*this);
	}

	auto FImportOperationHandle::SetRunningInBackground(
		bool bRunningInBackground) const -> bool
	{
		const auto Progress = LegacyState ? LegacyState->Progress
			: JobState ? JobState->Progress : nullptr;
		return Progress && Progress->SetBackground(bRunningInBackground);
	}

	auto FAsyncImportPlanHandle::GetOperationSnapshot() const -> FImportOperationSnapshot
	{
		return State && State->Progress
			? State->Progress->CopySnapshot() : FImportOperationSnapshot{};
	}

	auto FAsyncImportPlanHandle::GetProgressHistory() const
		-> std::vector<FImportOperationSnapshot>
	{
		return State && State->Progress
			? State->Progress->CopyHistory() : std::vector<FImportOperationSnapshot>{};
	}

	auto FAsyncImportPlanHandle::SetRunningInBackground(bool bRunningInBackground) const -> bool
	{
		return State && State->Progress
			&& State->Progress->SetBackground(bRunningInBackground);
	}

	auto FAsyncImportPlanHandle::CreateProgressReporter() const
		-> std::shared_ptr<IImportProgressReporter>
	{
		return State && State->Progress
			? std::make_shared<FAsyncImportProgressReporter>(State->Progress)
			: std::shared_ptr<IImportProgressReporter>{};
	}

	auto FAsyncImportPlanHandle::GetOperationTaskScope() const -> FTaskScopeToken
	{
		return State ? State->Scope.GetToken() : FTaskScopeToken{};
	}

	auto FAsyncImportPlanHandle::IsCancellationRequested() const -> bool
	{
		return State && State->Cancellation.IsCancellationRequested();
	}

	auto FAsyncImportPlanHandle::CompleteOperation(
		EImportOperationState TerminalState, std::string_view Diagnostic) const -> bool
	{
		return State && GetAsyncImportCoordinator().CompleteOperation(
			State, TerminalState, Diagnostic);
	}

	auto FImportService::LaunchAsyncImportPlan(
		FImportPlanRequest Request,
		std::string_view OwnerId,
		bool bKeepOperationOpenAfterPlan) -> FAsyncImportPlanHandle
	{
		return GetAsyncImportCoordinator().Launch(
			std::move(Request), OwnerId, bKeepOperationOpenAfterPlan);
	}

	auto FImportService::SubmitImportJob(
		std::unique_ptr<IImportJob> Job,
		std::string_view Title) -> FImportOperationHandle
	{
		return GetImportJobRegistry().Submit(std::move(Job), Title);
	}

	auto FImportService::PumpImportOperations(uint32 MaximumEditorSteps) -> uint32
	{
		return GetImportJobRegistry().Pump(MaximumEditorSteps);
	}

	auto FImportService::CancelAndDrainImportOperation(
		const FImportOperationHandle& Handle) -> void
	{
		GetImportJobRegistry().CancelAndDrain(Handle);
	}

	auto FImportService::RunImportJobInline(
		std::unique_ptr<IImportJob> Job,
		std::string_view Title) -> FImportOutcome
	{
		return GetImportJobRegistry().RunInline(std::move(Job), Title);
	}

	auto FImportService::CancelImportOperation(
		const FImportOperationHandle& Handle) -> bool
	{
		return Handle.RequestCancel();
	}

	auto FImportService::HasActiveImportClaim(std::string_view Identity) const -> bool
	{
		return GetImportJobRegistry().HasClaim(Identity);
	}

	auto FImportService::ReleaseImportPreviewOwner(std::string_view OwnerId) -> void
	{
		GetImportJobRegistry().ReleasePreviewOwner(OwnerId);
	}

	auto FImportService::BeginAsyncImportOperation(
		std::string_view OwnerId,
		std::string_view ProviderId,
		std::string_view Title) -> FAsyncImportPlanHandle
	{
		return GetAsyncImportCoordinator().BeginExtendedOperation(
			OwnerId, ProviderId, Title);
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

	auto FImportService::CancelAsyncImport(const FAsyncImportPlanHandle& Handle) -> bool
	{
		return GetAsyncImportCoordinator().Cancel(Handle);
	}

	auto FImportService::CancelAndDrainAsyncImport(const FAsyncImportPlanHandle& Handle)
		-> EAsyncImportPlanStatus
	{
		return GetAsyncImportCoordinator().CancelAndDrain(Handle);
	}

	auto FImportService::CancelAndDrainAsyncImportsForOwner(std::string_view OwnerId) -> void
	{
		GetImportJobRegistry().CancelAndDrainOwner(OwnerId);
		GetAsyncImportCoordinator().CancelAndDrainOwner(OwnerId);
	}

	auto FImportService::CancelAndDrainAsyncImportsForProvider(std::string_view ProviderId) -> void
	{
		GetImportJobRegistry().CancelAndDrainProvider(ProviderId);
		GetAsyncImportCoordinator().CancelAndDrainProvider(ProviderId);
	}

	auto FImportService::CancelAndDrainAllAsyncImports() -> void
	{
		GetImportJobRegistry().CancelAndDrainAll();
		GetAsyncImportCoordinator().CancelAndDrainAll();
	}

	auto FImportService::OpenAsyncImporterAdmission(std::string_view ProviderId) -> void
	{
		GetImportJobRegistry().OpenProvider(ProviderId);
		GetAsyncImportCoordinator().OpenProvider(ProviderId);
	}

	auto LaunchAsyncImportExecution(FAsyncImportExecutionRequest Request)
		-> FAsyncImportExecutionHandle
	{
		CheckImportEditorMutationAllowed("LaunchAsyncImportExecution");
		auto State = std::make_shared<FAsyncImportExecutionState>();
		State->bOwnsScope = Request.OperationScope == FTaskScopeToken{};
		if (State->bOwnsScope) State->Scope = CreateTaskScope();
		State->OperationScope = State->bOwnsScope
			? State->Scope.GetToken() : std::move(Request.OperationScope);
		if (!Request.Build)
		{
			State->Status = EAsyncImportPlanStatus::Rejected;
			State->Result = FDetachedImportBuildResult{
				.Message = "Detached import execution requires a build callback."};
			return FAsyncImportExecutionHandle(std::move(State));
		}

		FTaskLaunchOptions LaunchOptions;
		LaunchOptions.CancellationToken = State->Cancellation.GetToken();
		LaunchOptions.Scope = State->OperationScope;
		LaunchOptions.Attribution = Request.Attribution;
		auto Producer = LaunchUniqueCancelableTask<FDetachedImportBuildResult>(
			"AssetImport.DetachedExecution",
			[Build = std::move(Request.Build)](
				const FTaskCancellationToken& Token) mutable {
				const FScopedImportWorkerPreparation WorkerPreparation;
				if (Token.IsCancellationRequested())
					return FDetachedImportBuildResult{
						.Message = "Detached import execution was canceled before it started."};
				return Build(Token);
			}, LaunchOptions, Request.EstimatedResultBytes);
		if (!Producer.IsValid())
		{
			if (State->bOwnsScope)
				(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
			State->Status = EAsyncImportPlanStatus::Rejected;
			State->Result = FDetachedImportBuildResult{
				.Message = "The task scheduler rejected detached import execution."};
			return FAsyncImportExecutionHandle(std::move(State));
		}

		const FTaskHandle ProducerTask = Producer.GetTaskHandle();
		FTaskContinuationOptions PublisherOptions;
		PublisherOptions.Attribution = Request.Attribution;
		State->Task = ConsumeThenOutcome(
			std::move(Producer), "AssetImport.PublishDetachedExecution",
			[WeakState = std::weak_ptr<FAsyncImportExecutionState>(State)](
				FUniqueTaskOutcome<FDetachedImportBuildResult>&& Outcome) {
				auto State = WeakState.lock();
				if (!State) return;
				std::lock_guard Lock(State->Mutex);
				if (Outcome.State == ETaskState::Succeeded && Outcome.Result)
				{
					State->Result = std::move(*Outcome.Result);
					State->Status = State->Result->bSucceeded
						? EAsyncImportPlanStatus::Succeeded
						: State->Result->bCanceled
							|| State->Cancellation.IsCancellationRequested()
							? EAsyncImportPlanStatus::Canceled
							: EAsyncImportPlanStatus::Failed;
				}
				else
				{
					State->Status = Outcome.State == ETaskState::Canceled
						? EAsyncImportPlanStatus::Canceled
						: EAsyncImportPlanStatus::Failed;
					State->Result = FDetachedImportBuildResult{.Message =
						Outcome.Diagnostic.empty()
							? "Detached import execution did not produce a result."
							: std::move(Outcome.Diagnostic)};
				}
				if (State->bOwnsScope)
					(void)State->Scope.Close(ETaskScopeCloseMode::Drain);
			}, PublisherOptions);
		if (!State->Task.IsValid())
		{
			State->Cancellation.RequestCancellation();
			(void)CancelTask(ProducerTask);
			if (State->bOwnsScope)
				(void)State->Scope.Close(ETaskScopeCloseMode::Cancel);
			State->Status = EAsyncImportPlanStatus::Rejected;
			State->Result = FDetachedImportBuildResult{
				.Message = "The task scheduler rejected detached import result publication."};
		}
		return FAsyncImportExecutionHandle(std::move(State));
	}

	auto PollAsyncImportExecution(
		FAsyncImportExecutionHandle& Handle,
		FDetachedImportBuildResult& OutResult) -> EAsyncImportPlanStatus
	{
		if (!Handle.State) return EAsyncImportPlanStatus::Invalid;
		std::lock_guard Lock(Handle.State->Mutex);
		const EAsyncImportPlanStatus Status = Handle.State->Status;
		if (Status == EAsyncImportPlanStatus::Pending || Handle.State->bConsumed)
			return Status;
		if (Handle.State->Result) OutResult = std::move(*Handle.State->Result);
		Handle.State->Result.reset();
		Handle.State->Task = {};
		Handle.State->bConsumed = true;
		return Status;
	}

	auto CancelAndDrainAsyncImportExecution(
		FAsyncImportExecutionHandle& Handle) -> void
	{
		if (!Handle.State) return;
		Handle.State->Cancellation.RequestCancellation();
		if (Handle.State->Task.IsValid()) (void)CancelTask(Handle.State->Task);
		if (Handle.State->bOwnsScope)
		{
			(void)Handle.State->Scope.Close(ETaskScopeCloseMode::Cancel);
			(void)Handle.State->Scope.Wait();
		}
		else if (Handle.State->Task.IsValid())
			(void)WaitTask(Handle.State->Task);
		std::lock_guard Lock(Handle.State->Mutex);
		Handle.State->Task = {};
		Handle.State->Result.reset();
		Handle.State->Status = EAsyncImportPlanStatus::Canceled;
		Handle.State->bConsumed = true;
	}

	auto FImportService::CloseAsyncAdmission() -> void
	{
		GetImportJobRegistry().CloseAdmission();
		GetAsyncImportCoordinator().CloseAdmission();
	}

	auto IsImportCancellationRequested() -> bool
	{
		return GImportCancellationToken
			&& GImportCancellationToken->IsCancellationRequested();
	}

	auto IsImportWorkerPreparation() -> bool
	{
		return GImportWorkerPreparationDepth > 0;
	}

	FScopedImportWorkerPreparation::FScopedImportWorkerPreparation()
	{
		++GImportWorkerPreparationDepth;
	}

	FScopedImportWorkerPreparation::~FScopedImportWorkerPreparation()
	{
		check(GImportWorkerPreparationDepth > 0);
		--GImportWorkerPreparationDepth;
	}

	auto CheckImportEditorMutationAllowed(std::string_view Operation) -> void
	{
		checkf(!IsImportWorkerPreparation(),
			"Import editor mutation '{}' is prohibited on a preparation worker.", Operation);
	}
}
