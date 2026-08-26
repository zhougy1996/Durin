#include "AssetForge/Operations/ImportOperation.h"
#include "AssetForge/ImportService.h"
#include "AssetForge/Operations/ImportJob.h"
#include "ImportServicePrivate.h"

#include "Threading/Task.h"

namespace Durin::AssetForge
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
		std::optional<FImportJobWorkerResult> WorkerResult;
		std::optional<FImportOutcome> Outcome;
		bool bWorkerActive = false;
		bool bCompletionRejected = false;
		bool bReadyQueued = false;
		bool bFinalizing = false;
		bool bSupersedeRequested = false;
		std::function<bool()> RequestCancellation;
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

	class FImportJobStore final
		: public std::enable_shared_from_this<FImportJobStore>
	{
	public:
		auto Submit(std::unique_ptr<IImportJob> Job, std::string_view Title)
			-> std::shared_ptr<FImportJobOperationState>
		{
			CheckImportEditorMutationAllowed("SubmitImportJob");
			if (!Job) return {};
			auto State = std::make_shared<FImportJobOperationState>();
			State->OperationId = NextOperationId.fetch_add(1, std::memory_order_relaxed);
			State->Owner = Job->GetOwner();
			if (State->Owner.OwnerId.empty())
				State->Owner.OwnerId = std::format("job:{}", State->OperationId);
			State->ProviderId = Job->GetProviderId();
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

			State->Job = std::move(Job);
			State->RequestCancellation = [
				WeakStore = weak_from_this(),
				WeakState = std::weak_ptr<FImportJobOperationState>(State)] {
				const auto Store = WeakStore.lock();
				const auto Operation = WeakState.lock();
				return Store && Operation
					&& Store->RequestCancel(Operation, false);
			};

			std::shared_ptr<FImportJobOperationState> Superseded;
			bool bAccepted = false;
			{
				std::lock_guard Lock(Mutex);
				bAccepted = bAdmissionOpen
					&& !ClosedProviders.contains(State->ProviderId);
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
			return State;
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

		auto Cancel(const std::shared_ptr<FImportJobOperationState>& State) -> bool
		{
			return Owns(State) && RequestCancel(State, false);
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
			State->Lifetime = Job->GetLifetime();
			State->Progress = std::make_shared<FAsyncImportProgressState>();
			State->Progress->Snapshot = {
				.OperationId = State->OperationId,
				.Revision = 1,
				.OwnerId = State->Owner.OwnerId,
				.ProviderId = State->ProviderId,
				.Title = Title.empty() ? "Inline asset import" : std::string(Title),
				.State = EImportOperationState::Running};
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
					State->Progress->MarkState(Outcome.State, Outcome.Diagnostic);
					return Outcome;
				}
				FImportJobWorkerStep Worker = std::move(*Next.Worker);
				if (Worker.EstimatedResultBytes > MaximumImportJobDetachedValueBytes)
				{
					State->Job.reset();
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
					return Outcome;
				}
				Previous = std::move(Result.Value);
			}
			State->Job.reset();
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

		auto CancelAndDrain(
			const std::shared_ptr<FImportJobOperationState>& Target) -> void
		{
			if (!Owns(Target)) return;
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
		auto Owns(const std::shared_ptr<FImportJobOperationState>& State) const
			-> bool
		{
			if (!State) return false;
			std::lock_guard Lock(Mutex);
			const auto It = Operations.find(State->OperationId);
			return It != Operations.end() && It->second == State;
		}

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
				std::move(Producer), "Durin.AssetForge.JobWorkerCompletion",
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

	FImportService::FImpl::FImpl()
		: AsyncJobs(std::make_shared<FImportJobStore>())
	{
	}

	FImportService::FImpl::~FImpl() = default;

	auto FImportOperationHandle::GetOperationId() const -> uint64
	{
		return JobState ? JobState->OperationId : 0;
	}

	auto FImportOperationHandle::GetSnapshot() const -> FImportOperationSnapshot
	{
		const auto Progress = JobState ? JobState->Progress : nullptr;
		return Progress ? Progress->CopySnapshot() : FImportOperationSnapshot{};
	}

	auto FImportOperationHandle::GetProgressHistory() const
		-> std::vector<FImportOperationSnapshot>
	{
		const auto Progress = JobState ? JobState->Progress : nullptr;
		return Progress ? Progress->CopyHistory()
			: std::vector<FImportOperationSnapshot>{};
	}

	auto FImportOperationHandle::TryGetOutcome(FImportOutcome& OutOutcome) const -> bool
	{
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
		return JobState && JobState->RequestCancellation
			&& JobState->RequestCancellation();
	}

	auto FImportOperationHandle::SetRunningInBackground(
		bool bRunningInBackground) const -> bool
	{
		const auto Progress = JobState ? JobState->Progress : nullptr;
		return Progress && Progress->SetBackground(bRunningInBackground);
	}

	auto FImportService::SubmitImportJob(
		std::unique_ptr<IImportJob> Job,
		std::string_view Title) -> FImportOperationHandle
	{
		return FImportOperationHandle(Impl->AsyncJobs->Submit(std::move(Job), Title));
	}

	auto FImportService::PumpImportOperations(uint32 MaximumEditorSteps) -> uint32
	{
		return Impl->AsyncJobs->Pump(MaximumEditorSteps);
	}

	auto FImportService::CancelAndDrainImportOperation(
		const FImportOperationHandle& Handle) -> void
	{
		Impl->AsyncJobs->CancelAndDrain(Handle.JobState);
	}

	auto FImportService::RunImportJobInline(
		std::unique_ptr<IImportJob> Job,
		std::string_view Title) -> FImportOutcome
	{
		return Impl->AsyncJobs->RunInline(std::move(Job), Title);
	}

	auto FImportService::CancelImportOperation(
		const FImportOperationHandle& Handle) -> bool
	{
		return Impl->AsyncJobs->Cancel(Handle.JobState);
	}

	auto FImportService::HasActiveImportClaim(std::string_view Identity) const -> bool
	{
		return Impl->AsyncJobs->HasClaim(Identity);
	}

	auto FImportService::ReleaseImportPreviewOwner(std::string_view OwnerId) -> void
	{
		Impl->AsyncJobs->ReleasePreviewOwner(OwnerId);
	}

	auto FImportService::CancelAndDrainAsyncImportsForOwner(std::string_view OwnerId) -> void
	{
		Impl->AsyncJobs->CancelAndDrainOwner(OwnerId);
	}

	auto FImportService::CancelAndDrainAsyncImportsForProvider(std::string_view ProviderId) -> void
	{
		Impl->AsyncJobs->CancelAndDrainProvider(ProviderId);
	}

	auto FImportService::CancelAndDrainAllAsyncImports() -> void
	{
		Impl->AsyncJobs->CancelAndDrainAll();
	}

	auto FImportService::OpenAsyncImporterAdmission(std::string_view ProviderId) -> void
	{
		Impl->AsyncJobs->OpenProvider(ProviderId);
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
			"Durin.AssetForge.DetachedExecution",
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
			std::move(Producer), "Durin.AssetForge.PublishDetachedExecution",
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
		Impl->AsyncJobs->CloseAdmission();
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
