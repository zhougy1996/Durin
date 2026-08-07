#include "Threading/Task.h"

#include "Threading/QueuedThreadPool.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		enum class ETaskSchedulerLifetime : uint8
		{
			Stopped,
			Running,
			ShuttingDown,
		};

		std::atomic<uint64> GNextTaskId = 1;
		std::mutex GTaskSchedulerMutex;
		std::condition_variable GTaskSchedulerCV;
		std::shared_ptr<FTaskScheduler> GTaskScheduler;
		ETaskSchedulerLifetime GTaskSchedulerLifetime = ETaskSchedulerLifetime::Stopped;
		uint64 GCompletedTaskSchedulerShutdowns = 0;
		FTaskSchedulerDiagnostics GLastTaskSchedulerDiagnostics;

		thread_local FTaskStateData* GCurrentTaskState = nullptr;
		thread_local FTaskScheduler* GCurrentTaskScheduler = nullptr;
		thread_local uint32 GParallelForDepth = 0;

		constexpr double WorkerWaitSliceSeconds = 0.001;
		constexpr double LongWaitThresholdSeconds = 0.1;

		auto MonotonicNanoseconds() -> uint64
		{
			return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()
			).count());
		}

		auto IsTerminalState(ETaskState State) -> bool
		{
			return State == ETaskState::Succeeded || State == ETaskState::Failed || State == ETaskState::Canceled;
		}
	} // namespace

	class FTaskCancellationState
	{
	public:
		auto IsCancellationRequested() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bCancellationRequested;
		}

		auto RegisterTask(const std::shared_ptr<FTaskStateData>& Task) -> void;
		auto UnregisterTask(uint64 TaskId) -> void;
		auto RequestCancellation() -> void;

	private:
		mutable std::mutex Mutex;
		std::unordered_map<uint64, std::weak_ptr<FTaskStateData>> Tasks;
		bool bCancellationRequested = false;
	};

	// Owns one scheduler task node independently of public handles and pool work items.
	class FTaskStateData final : public std::enable_shared_from_this<FTaskStateData>
	{
	public:
		FTaskStateData(
			const char* InDebugName,
			std::weak_ptr<FTaskScheduler> InScheduler,
			FCancelableTaskFunction&& InFunction,
			std::function<void(ETaskState)>&& InCompletionFunction,
			const FTaskCancellationToken& InCancellationToken,
			const std::vector<std::shared_ptr<FTaskStateData>>& InPrerequisites,
			uint64 InParentTaskId,
			ETaskDependencyKind InDependencyKind,
			bool bInAggregatePrerequisites
		)
			: TaskId(GNextTaskId.fetch_add(1, std::memory_order::acq_rel))
			, ParentTaskId(InParentTaskId)
			, DebugName(InDebugName ? InDebugName : "Task")
			, Scheduler(std::move(InScheduler))
			, SharedCancellationState(InCancellationToken.SharedState)
			, PendingFunction(std::move(InFunction))
			, CompletionFunction(std::move(InCompletionFunction))
			, bHasResultStorage(static_cast<bool>(CompletionFunction))
			, RemainingPrerequisites(static_cast<uint32>(InPrerequisites.size()))
			, State(InPrerequisites.empty() ? ETaskState::Queued : ETaskState::Waiting)
			, DependencyKind(InDependencyKind)
			, bAggregatePrerequisites(bInAggregatePrerequisites)
			, EnqueueTimeNanoseconds(MonotonicNanoseconds())
		{
			Prerequisites.reserve(InPrerequisites.size());
			PrerequisiteTaskIds.reserve(InPrerequisites.size());
			for (const std::shared_ptr<FTaskStateData>& Prerequisite : InPrerequisites)
			{
				Prerequisites.emplace_back(Prerequisite);
				PrerequisiteTaskIds.emplace_back(Prerequisite->GetTaskId());
				PrerequisiteDependencyKinds.emplace_back(InDependencyKind);
			}
		}

		auto RegisterDependent(const std::shared_ptr<FTaskStateData>& Dependent) -> ETaskState
		{
			std::lock_guard Lock(Mutex);
			if (!IsTerminalState(State))
			{
				Dependents.emplace_back(Dependent);
			}
			return State;
		}

		auto OnPrerequisiteTerminal(ETaskState PrerequisiteState, uint64 PrerequisiteTaskId) -> void;
		auto TakeFunctionForQueue() -> FCancelableTaskFunction
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Queued)
			{
				return {};
			}
			return std::move(PendingFunction);
		}

		auto TryMarkRunning() -> bool
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Queued)
			{
				return false;
			}
			State = ETaskState::Running;
			StartTimeNanoseconds = MonotonicNanoseconds();
			ExecutingThreadName = GetCurrentThreadName();
			if (FRunnableThread* CurrentThread = GetCurrentThread())
			{
				ExecutingThreadId = CurrentThread->GetThreadId();
			}
			return true;
		}

		auto MakeCancellationToken() -> FTaskCancellationToken
		{
			return FTaskCancellationToken(SharedCancellationState, weak_from_this());
		}

		auto IsCancellationRequested() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bCancellationRequested;
		}

		auto DependsOn(const FTaskStateData* PotentialPrerequisite) const -> bool
		{
			std::vector<std::shared_ptr<FTaskStateData>> PendingPrerequisites;
			{
				std::lock_guard Lock(Mutex);
				PendingPrerequisites.reserve(Prerequisites.size());
				for (const std::weak_ptr<FTaskStateData>& Prerequisite : Prerequisites)
				{
					if (std::shared_ptr<FTaskStateData> PinnedPrerequisite = Prerequisite.lock())
					{
						PendingPrerequisites.emplace_back(std::move(PinnedPrerequisite));
					}
				}
			}

			std::unordered_set<const FTaskStateData*> VisitedPrerequisites;
			while (!PendingPrerequisites.empty())
			{
				std::shared_ptr<FTaskStateData> Prerequisite = std::move(PendingPrerequisites.back());
				PendingPrerequisites.pop_back();
				if (Prerequisite.get() == PotentialPrerequisite)
				{
					return true;
				}
				if (!VisitedPrerequisites.emplace(Prerequisite.get()).second)
				{
					continue;
				}

				std::lock_guard Lock(Prerequisite->Mutex);
				for (const std::weak_ptr<FTaskStateData>& TransitivePrerequisite : Prerequisite->Prerequisites)
				{
					if (std::shared_ptr<FTaskStateData> PinnedPrerequisite = TransitivePrerequisite.lock())
					{
						PendingPrerequisites.emplace_back(std::move(PinnedPrerequisite));
					}
				}
			}
			return false;
		}

		auto RequestCancellation(std::string InDiagnostic, ETaskTerminalReason InReason = ETaskTerminalReason::CancellationRequested, uint64 InDirectBlockingTaskId = 0) -> bool;
		auto MarkSucceeded() -> void;
		auto MarkFailed(std::string InDiagnostic) -> void;
		auto MarkCanceled(std::string InDiagnostic) -> void;

		auto Wait() -> ETaskState
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [this]() {
				return IsTerminalState(State);
			});
			return State;
		}

		auto WaitFor(double TimeoutSeconds) -> ETaskState
		{
			std::unique_lock Lock(Mutex);
			CV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), [this]() {
				return IsTerminalState(State);
			});
			return State;
		}

		auto GetState() const -> ETaskState
		{
			std::lock_guard Lock(Mutex);
			return State;
		}

		auto GetDiagnostic() const -> std::string
		{
			std::lock_guard Lock(Mutex);
			return Diagnostic;
		}

		auto GetDiagnostics() const -> FTaskDiagnostics
		{
			std::lock_guard Lock(Mutex);
			FTaskDiagnostics Snapshot;
			Snapshot.TaskId = TaskId;
			Snapshot.ParentTaskId = ParentTaskId;
			Snapshot.PrerequisiteTaskIds = PrerequisiteTaskIds;
			Snapshot.PrerequisiteDependencyKinds = PrerequisiteDependencyKinds;
			Snapshot.DirectBlockingTaskId = DirectBlockingTaskId;
			Snapshot.EnqueueTimeNanoseconds = EnqueueTimeNanoseconds;
			Snapshot.StartTimeNanoseconds = StartTimeNanoseconds;
			Snapshot.FinishTimeNanoseconds = FinishTimeNanoseconds;
			Snapshot.ExecutingThreadId = ExecutingThreadId;
			Snapshot.DebugName = DebugName;
			Snapshot.ExecutingThreadName = ExecutingThreadName;
			Snapshot.Diagnostic = Diagnostic;
			Snapshot.State = State;
			Snapshot.Target = ETaskTarget::AnyWorker;
			Snapshot.TerminalReason = TerminalReason;
			Snapshot.bHasResultStorage = bHasResultStorage;
			return Snapshot;
		}

		auto GetDebugName() const -> const char* { return DebugName.c_str(); }
		auto GetTaskId() const -> uint64 { return TaskId; }
		auto PinScheduler() const -> std::shared_ptr<FTaskScheduler> { return Scheduler.lock(); }
		auto GetSharedCancellationState() const -> const std::shared_ptr<FTaskCancellationState>& { return SharedCancellationState; }

	private:
		auto PublishTerminal(ETaskState TerminalState, std::string InDiagnostic, ETaskTerminalReason InReason = ETaskTerminalReason::None, uint64 InDirectBlockingTaskId = 0) -> bool;
		auto PublishTerminalLocked(ETaskState TerminalState, std::string InDiagnostic, std::vector<std::shared_ptr<FTaskStateData>>& OutDependents, std::function<void(ETaskState)>& OutCompletionFunction, ETaskTerminalReason InReason = ETaskTerminalReason::None, uint64 InDirectBlockingTaskId = 0) -> bool;
		auto FinishTerminalPublication(ETaskState TerminalState, std::vector<std::shared_ptr<FTaskStateData>>&& Dependents) -> void;

		uint64 TaskId = 0;
		uint64 ParentTaskId = 0;
		std::string DebugName;
		std::weak_ptr<FTaskScheduler> Scheduler;
		std::shared_ptr<FTaskCancellationState> SharedCancellationState;
		mutable std::mutex Mutex;
		std::condition_variable CV;
		FCancelableTaskFunction PendingFunction;
		std::function<void(ETaskState)> CompletionFunction;
		bool bHasResultStorage = false;
		std::vector<std::weak_ptr<FTaskStateData>> Prerequisites;
		std::vector<uint64> PrerequisiteTaskIds;
		std::vector<ETaskDependencyKind> PrerequisiteDependencyKinds;
		std::vector<std::weak_ptr<FTaskStateData>> Dependents;
		uint32 RemainingPrerequisites = 0;
		ETaskState State = ETaskState::Queued;
		ETaskDependencyKind DependencyKind = ETaskDependencyKind::Success;
		ETaskTerminalReason TerminalReason = ETaskTerminalReason::None;
		uint64 DirectBlockingTaskId = 0;
		uint64 BlockingPrerequisiteTaskId = 0;
		ETaskState BlockingPrerequisiteState = ETaskState::Succeeded;
		bool bAggregatePrerequisites = false;
		bool bCancellationRequested = false;
		std::string CancellationDiagnostic;
		ETaskTerminalReason CancellationReason = ETaskTerminalReason::CancellationRequested;
		uint64 CancellationDirectBlockingTaskId = 0;
		std::string Diagnostic;
		uint64 EnqueueTimeNanoseconds = 0;
		uint64 StartTimeNanoseconds = 0;
		uint64 FinishTimeNanoseconds = 0;
		uint32 ExecutingThreadId = 0;
		std::string ExecutingThreadName;
	};

	// Owns the process scheduler's pool and every accepted nonterminal node.
	class FTaskScheduler final : public std::enable_shared_from_this<FTaskScheduler>
	{
	public:
		static auto Create(uint32 InNumThreads) -> std::shared_ptr<FTaskScheduler>
		{
			auto Scheduler = std::shared_ptr<FTaskScheduler>(new FTaskScheduler());
			const uint32 NumThreads = InNumThreads > 0 ? InNumThreads : GetDefaultThreadPoolThreadCount();
			if (!Scheduler->Pool.Create(NumThreads, "EngineWorker"))
			{
				return nullptr;
			}

			Scheduler->WorkerCount = NumThreads;
			Scheduler->bAcceptingTasks = true;
			return Scheduler;
		}

		auto Submit(
			const char* Name,
			FCancelableTaskFunction&& Function,
			std::function<void(ETaskState)>&& CompletionFunction,
			const FTaskLaunchOptions& Options,
			ETaskDependencyKind DependencyKind = ETaskDependencyKind::Success,
			bool bAggregatePrerequisites = false) -> std::shared_ptr<FTaskStateData>
		{
			std::shared_ptr<FTaskStateData> State;
			std::vector<std::shared_ptr<FTaskStateData>> PrerequisiteStates;
			{
				std::lock_guard Lock(Mutex);
				if (!bAcceptingTasks)
				{
					RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
					return {};
				}

				for (const FTaskHandle& Prerequisite : Options.Prerequisites)
				{
					if (!Prerequisite.State || Prerequisite.State->PinScheduler().get() != this)
					{
						RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
						return {};
					}
					PrerequisiteStates.emplace_back(Prerequisite.State);
				}

				State = std::make_shared<FTaskStateData>(
					Name,
					weak_from_this(),
					std::move(Function),
					std::move(CompletionFunction),
					Options.CancellationToken,
					PrerequisiteStates,
					GCurrentTaskState ? GCurrentTaskState->GetTaskId() : 0,
					DependencyKind,
					bAggregatePrerequisites
				);
				ActiveTasks.emplace(State->GetTaskId(), State);
				AllTasks.emplace(State->GetTaskId(), State);
			}

			if (const std::shared_ptr<FTaskCancellationState>& CancellationState = State->GetSharedCancellationState())
			{
				CancellationState->RegisterTask(State);
			}

			for (const std::shared_ptr<FTaskStateData>& Prerequisite : PrerequisiteStates)
			{
				const ETaskState PrerequisiteState = Prerequisite->RegisterDependent(State);
				if (IsTerminalState(PrerequisiteState))
				{
					State->OnPrerequisiteTerminal(PrerequisiteState, Prerequisite->GetTaskId());
				}
			}

			if (Options.Prerequisites.empty())
			{
				QueueTask(State);
			}

			DURIN_TRACE("Task accepted. (task: {}, id: {}, prerequisites: {})", State->GetDebugName(), State->GetTaskId(), Options.Prerequisites.size());
			return State;
		}

		auto QueueTask(const std::shared_ptr<FTaskStateData>& State) -> void
		{
			FCancelableTaskFunction Function = State->TakeFunctionForQueue();
			if (!Function)
			{
				return;
			}

			const bool bAccepted = Pool.Enqueue(
				State->GetDebugName(),
				[State, Scheduler = this, Function = std::move(Function)]() mutable {
					if (!State->TryMarkRunning())
					{
						return;
					}
					Scheduler->OnWorkerStarted();

					FTaskStateData* PreviousTaskState = GCurrentTaskState;
					FTaskScheduler* PreviousTaskScheduler = GCurrentTaskScheduler;
					GCurrentTaskState = State.get();
					GCurrentTaskScheduler = Scheduler;

					try
					{
						Function(State->MakeCancellationToken());
						State->MarkSucceeded();
					}
					catch (const std::exception& Exception)
					{
						State->MarkFailed(Exception.what());
					}
					catch (...)
					{
						State->MarkFailed("Task callable threw an unknown exception.");
					}

					GCurrentTaskState = PreviousTaskState;
					GCurrentTaskScheduler = PreviousTaskScheduler;
					Scheduler->OnWorkerFinished();
				},
				[State]() {
					State->RequestCancellation("Task was discarded during scheduler shutdown.", ETaskTerminalReason::ShutdownCanceled);
				}
			);

			if (!bAccepted)
			{
				State->RequestCancellation("Task could not be queued because scheduler shutdown had begun.", ETaskTerminalReason::DispatchRejected);
			}
		}

		auto CloseAdmission() -> void
		{
			std::lock_guard Lock(Mutex);
			bAcceptingTasks = false;
		}

		auto Shutdown(bool bWaitForQueuedWork) -> void
		{
			CloseAdmission();
			if (!bWaitForQueuedWork)
			{
				std::vector<std::shared_ptr<FTaskStateData>> TasksToCancel;
				{
					std::lock_guard Lock(Mutex);
					TasksToCancel.reserve(ActiveTasks.size());
					for (const auto& [TaskId, Task] : ActiveTasks)
					{
						TasksToCancel.push_back(Task);
					}
				}
				for (const std::shared_ptr<FTaskStateData>& Task : TasksToCancel)
				{
					Task->RequestCancellation("Task was canceled during scheduler shutdown.", ETaskTerminalReason::ShutdownCanceled);
				}

				Pool.Destroy(false);
			}

			{
				std::unique_lock Lock(Mutex);
				if (!QuiescenceCV.wait_for(Lock, std::chrono::duration<double>(LongWaitThresholdSeconds), [this]() {
					return ActiveTasks.empty();
				}))
				{
					std::vector<std::shared_ptr<FTaskStateData>> NonterminalTasks;
					NonterminalTasks.reserve(ActiveTasks.size());
					for (const auto& [TaskId, Task] : ActiveTasks)
					{
						NonterminalTasks.emplace_back(Task);
					}
					LongWaitCount.fetch_add(1, std::memory_order::acq_rel);
					Lock.unlock();
					for (const std::shared_ptr<FTaskStateData>& Task : NonterminalTasks)
					{
						const FTaskDiagnostics Diagnostics = Task->GetDiagnostics();
						DURIN_WARN(
							"Task scheduler shutdown is waiting for a nonterminal task. (task: {}, id: {}, state: {})",
							Diagnostics.DebugName,
							Diagnostics.TaskId,
							static_cast<uint32>(Diagnostics.State)
						);
					}
					Lock.lock();
					QuiescenceCV.wait(Lock, [this]() {
						return ActiveTasks.empty();
					});
				}
			}

			if (bWaitForQueuedWork)
			{
				Pool.Destroy(true);
			}
		}

		auto OnTaskTerminal(uint64 TaskId, ETaskState TerminalState) -> void
		{
			std::lock_guard Lock(Mutex);
			const size_t RemovedTaskCount = ActiveTasks.erase(TaskId);
			check(RemovedTaskCount == 1);
			CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			if (TerminalState == ETaskState::Failed)
			{
				FailedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}
			else if (TerminalState == ETaskState::Canceled)
			{
				CanceledTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}
			if (ActiveTasks.empty())
			{
				QuiescenceCV.notify_all();
			}
		}

		auto TryExecuteOneQueuedTask() -> bool
		{
			return Pool.TryExecuteOneQueuedTask();
		}

		auto RecordRejectedTask() -> void
		{
			RejectedTaskCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto RecordLongWait(const char* WaiterName, const FTaskDiagnostics& Target, uint64 ElapsedNanoseconds) -> void
		{
			std::lock_guard Lock(Mutex);
			LastLongWaiterName = WaiterName ? WaiterName : "Unknown";
			LastLongWaitTargetName = Target.DebugName;
			LastLongWaitTargetTaskId = Target.TaskId;
			LastLongWaitTargetState = Target.State;
			LastLongWaitElapsedNanoseconds = ElapsedNanoseconds;
			LongWaitCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto OnWorkerStarted() -> void
		{
			ActiveWorkerCount.fetch_add(1, std::memory_order::acq_rel);
		}

		auto OnWorkerFinished() -> void
		{
			const uint32 PreviousCount = ActiveWorkerCount.fetch_sub(1, std::memory_order::acq_rel);
			check(PreviousCount > 0);
		}

		auto GetDiagnostics(bool bInRunning) -> FTaskSchedulerDiagnostics
		{
			FTaskSchedulerDiagnostics Snapshot;
			Snapshot.WorkerCount = WorkerCount;
			Snapshot.QueueDepth = Pool.GetNumQueuedTasks();
			Snapshot.ActiveWorkerCount = ActiveWorkerCount.load(std::memory_order::acquire);
			Snapshot.CompletedTaskCount = CompletedTaskCount.load(std::memory_order::acquire);
			Snapshot.FailedTaskCount = FailedTaskCount.load(std::memory_order::acquire);
			Snapshot.CanceledTaskCount = CanceledTaskCount.load(std::memory_order::acquire);
			Snapshot.RejectedTaskCount = RejectedTaskCount.load(std::memory_order::acquire);
			Snapshot.LongWaitCount = LongWaitCount.load(std::memory_order::acquire);
			Snapshot.bRunning = bInRunning;

			std::lock_guard Lock(Mutex);
			Snapshot.LastLongWaiterName = LastLongWaiterName;
			Snapshot.LastLongWaitTargetName = LastLongWaitTargetName;
			Snapshot.LastLongWaitTargetTaskId = LastLongWaitTargetTaskId;
			Snapshot.LastLongWaitTargetState = LastLongWaitTargetState;
			Snapshot.LastLongWaitElapsedNanoseconds = LastLongWaitElapsedNanoseconds;
			Snapshot.NonterminalTaskCount = ActiveTasks.size();
			Snapshot.NonterminalTasks.reserve(ActiveTasks.size());
			for (const auto& [TaskId, Task] : ActiveTasks)
			{
				Snapshot.NonterminalTasks.emplace_back(Task->GetDiagnostics());
			}
			for (auto Iterator = AllTasks.begin(); Iterator != AllTasks.end();)
			{
				if (std::shared_ptr<FTaskStateData> Task = Iterator->second.lock())
				{
					if (IsTerminalState(Task->GetState()))
					{
						++Snapshot.RetainedTerminalHandleCount;
						if (Task->GetDiagnostics().bHasResultStorage)
						{
							++Snapshot.RetainedTerminalResultCount;
						}
					}
					++Iterator;
				}
				else
				{
					Iterator = AllTasks.erase(Iterator);
				}
			}
			return Snapshot;
		}

	private:
		FQueuedThreadPool Pool;
		mutable std::mutex Mutex;
		std::condition_variable QuiescenceCV;
		std::unordered_map<uint64, std::shared_ptr<FTaskStateData>> ActiveTasks;
		std::unordered_map<uint64, std::weak_ptr<FTaskStateData>> AllTasks;
		std::atomic<uint32> ActiveWorkerCount = 0;
		std::atomic<uint64> CompletedTaskCount = 0;
		std::atomic<uint64> FailedTaskCount = 0;
		std::atomic<uint64> CanceledTaskCount = 0;
		std::atomic<uint64> RejectedTaskCount = 0;
		std::atomic<uint64> LongWaitCount = 0;
		std::string LastLongWaiterName;
		std::string LastLongWaitTargetName;
		uint64 LastLongWaitTargetTaskId = 0;
		uint64 LastLongWaitElapsedNanoseconds = 0;
		ETaskState LastLongWaitTargetState = ETaskState::Invalid;
		uint32 WorkerCount = 0;
		bool bAcceptingTasks = false;
	};

	auto FTaskStateData::PublishTerminalLocked(
		ETaskState TerminalState,
		std::string InDiagnostic,
		std::vector<std::shared_ptr<FTaskStateData>>& OutDependents,
		std::function<void(ETaskState)>& OutCompletionFunction,
		ETaskTerminalReason InReason,
		uint64 InDirectBlockingTaskId
	) -> bool
	{
		if (IsTerminalState(State))
		{
			return false;
		}

		const bool bValidTransition =
			(State == ETaskState::Running && (TerminalState == ETaskState::Succeeded || TerminalState == ETaskState::Failed || TerminalState == ETaskState::Canceled))
			|| ((State == ETaskState::Waiting || State == ETaskState::Queued) && TerminalState == ETaskState::Canceled);
		check(bValidTransition);
		State = TerminalState;
		FinishTimeNanoseconds = MonotonicNanoseconds();
		Diagnostic = std::move(InDiagnostic);
		TerminalReason = InReason;
		DirectBlockingTaskId = InDirectBlockingTaskId;
		PendingFunction = {};
		OutCompletionFunction = std::move(CompletionFunction);
		for (const std::weak_ptr<FTaskStateData>& Dependent : Dependents)
		{
			if (std::shared_ptr<FTaskStateData> PinnedDependent = Dependent.lock())
			{
				OutDependents.emplace_back(std::move(PinnedDependent));
			}
		}
		Dependents.clear();
		return true;
	}

	auto FTaskStateData::PublishTerminal(
		ETaskState TerminalState,
		std::string InDiagnostic,
		ETaskTerminalReason InReason,
		uint64 InDirectBlockingTaskId) -> bool
	{
		std::vector<std::shared_ptr<FTaskStateData>> DependentsToNotify;
		std::function<void(ETaskState)> Function;
		{
			std::lock_guard Lock(Mutex);
			if (!PublishTerminalLocked(TerminalState, std::move(InDiagnostic), DependentsToNotify, Function, InReason, InDirectBlockingTaskId))
			{
				return false;
			}
		}
		if (Function) Function(TerminalState);
		FinishTerminalPublication(TerminalState, std::move(DependentsToNotify));
		return true;
	}

	auto FTaskStateData::FinishTerminalPublication(ETaskState TerminalState, std::vector<std::shared_ptr<FTaskStateData>>&& Dependents) -> void
	{
		CV.notify_all();
		if (SharedCancellationState)
		{
			SharedCancellationState->UnregisterTask(TaskId);
		}
		if (std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock())
		{
			PinnedScheduler->OnTaskTerminal(TaskId, TerminalState);
		}
		for (const std::shared_ptr<FTaskStateData>& Dependent : Dependents)
		{
			Dependent->OnPrerequisiteTerminal(TerminalState, TaskId);
		}
	}

	auto FTaskStateData::OnPrerequisiteTerminal(ETaskState PrerequisiteState, uint64 PrerequisiteTaskId) -> void
	{
		if (bAggregatePrerequisites)
		{
			bool bShouldQueue = false;
			bool bShouldCancel = false;
			ETaskState BlockingState = ETaskState::Succeeded;
			uint64 BlockingTaskId = 0;
			{
				std::lock_guard Lock(Mutex);
				if (State != ETaskState::Waiting)
				{
					return;
				}
				check(RemainingPrerequisites > 0);
				--RemainingPrerequisites;
				if (DependencyKind == ETaskDependencyKind::Success && PrerequisiteState != ETaskState::Succeeded)
				{
					const bool bPreferPrerequisite = BlockingPrerequisiteTaskId == 0
						|| (PrerequisiteState == ETaskState::Failed && BlockingPrerequisiteState != ETaskState::Failed)
						|| (PrerequisiteState == BlockingPrerequisiteState && PrerequisiteTaskId < BlockingPrerequisiteTaskId);
					if (bPreferPrerequisite)
					{
						BlockingPrerequisiteTaskId = PrerequisiteTaskId;
						BlockingPrerequisiteState = PrerequisiteState;
					}
				}
				if (RemainingPrerequisites == 0)
				{
					BlockingState = BlockingPrerequisiteState;
					BlockingTaskId = BlockingPrerequisiteTaskId;
					bShouldCancel = DependencyKind == ETaskDependencyKind::Success && BlockingTaskId != 0;
					if (!bShouldCancel)
					{
						State = ETaskState::Queued;
						bShouldQueue = true;
					}
				}
			}

			if (bShouldCancel)
			{
				RequestCancellation(
					"Task was canceled because prerequisite " + std::to_string(BlockingTaskId)
						+ (BlockingState == ETaskState::Failed ? " failed." : " was canceled."),
					BlockingState == ETaskState::Failed ? ETaskTerminalReason::DependencyFailed : ETaskTerminalReason::DependencyCanceled,
					BlockingTaskId
				);
			}
			else if (bShouldQueue)
			{
				if (std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock())
				{
					PinnedScheduler->QueueTask(shared_from_this());
				}
				else
				{
					RequestCancellation("Task scheduler was unavailable when prerequisites completed.", ETaskTerminalReason::DispatchRejected);
				}
			}
			return;
		}

		if (PrerequisiteState != ETaskState::Succeeded)
		{
			RequestCancellation(
				"Task was canceled because prerequisite " + std::to_string(PrerequisiteTaskId)
					+ (PrerequisiteState == ETaskState::Failed ? " failed." : " was canceled."),
				PrerequisiteState == ETaskState::Failed ? ETaskTerminalReason::DependencyFailed : ETaskTerminalReason::DependencyCanceled,
				PrerequisiteTaskId
			);
			return;
		}

		bool bShouldQueue = false;
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Waiting)
			{
				return;
			}
			check(RemainingPrerequisites > 0);
			--RemainingPrerequisites;
			if (RemainingPrerequisites == 0)
			{
				State = ETaskState::Queued;
				bShouldQueue = true;
			}
		}

		if (bShouldQueue)
		{
			if (std::shared_ptr<FTaskScheduler> PinnedScheduler = Scheduler.lock())
			{
				PinnedScheduler->QueueTask(shared_from_this());
			}
			else
			{
				RequestCancellation("Task scheduler was unavailable when prerequisites completed.", ETaskTerminalReason::DispatchRejected);
			}
		}
	}

	auto FTaskStateData::RequestCancellation(std::string InDiagnostic, ETaskTerminalReason InReason, uint64 InDirectBlockingTaskId) -> bool
	{
		std::vector<std::shared_ptr<FTaskStateData>> DependentsToNotify;
		std::function<void(ETaskState)> Function;
		bool bPublishedTerminal = false;
		{
			std::lock_guard Lock(Mutex);
			if (IsTerminalState(State))
			{
				return false;
			}

			bCancellationRequested = true;
			if (CancellationDiagnostic.empty())
			{
				CancellationDiagnostic = std::move(InDiagnostic);
				CancellationReason = InReason;
				CancellationDirectBlockingTaskId = InDirectBlockingTaskId;
			}
			if (State == ETaskState::Waiting || State == ETaskState::Queued)
			{
				bPublishedTerminal = PublishTerminalLocked(ETaskState::Canceled, CancellationDiagnostic, DependentsToNotify, Function, InReason, InDirectBlockingTaskId);
			}
		}

		if (bPublishedTerminal)
		{
			if (Function) Function(ETaskState::Canceled);
			FinishTerminalPublication(ETaskState::Canceled, std::move(DependentsToNotify));
		}
		return true;
	}

	auto FTaskStateData::MarkSucceeded() -> void
	{
		std::vector<std::shared_ptr<FTaskStateData>> DependentsToNotify;
		std::function<void(ETaskState)> Function;
		ETaskState TerminalState = ETaskState::Succeeded;
		{
			std::lock_guard Lock(Mutex);
			if (State != ETaskState::Running)
			{
				return;
			}
			if (bCancellationRequested || (SharedCancellationState && SharedCancellationState->IsCancellationRequested()))
			{
				TerminalState = ETaskState::Canceled;
			}
			PublishTerminalLocked(
				TerminalState,
				TerminalState == ETaskState::Canceled ? (CancellationDiagnostic.empty() ? "Task returned after cancellation was requested." : CancellationDiagnostic) : std::string{},
				DependentsToNotify,
				Function,
				TerminalState == ETaskState::Canceled ? CancellationReason : ETaskTerminalReason::None,
				TerminalState == ETaskState::Canceled ? CancellationDirectBlockingTaskId : 0
			);
		}
		if (Function) Function(TerminalState);
		FinishTerminalPublication(TerminalState, std::move(DependentsToNotify));
	}

	auto FTaskStateData::MarkFailed(std::string InDiagnostic) -> void
	{
		PublishTerminal(ETaskState::Failed, std::move(InDiagnostic), ETaskTerminalReason::CallbackFailure);
	}

	auto FTaskStateData::MarkCanceled(std::string InDiagnostic) -> void
	{
		RequestCancellation(std::move(InDiagnostic));
	}

	auto FTaskCancellationState::RegisterTask(const std::shared_ptr<FTaskStateData>& Task) -> void
	{
		bool bAlreadyCanceled = false;
		{
			std::lock_guard Lock(Mutex);
			bAlreadyCanceled = bCancellationRequested;
			if (!bAlreadyCanceled)
			{
				Tasks.emplace(Task->GetTaskId(), Task);
			}
		}
		if (bAlreadyCanceled)
		{
			Task->RequestCancellation("Task cancellation was requested by its shared source.");
		}
	}

	auto FTaskCancellationState::UnregisterTask(uint64 TaskId) -> void
	{
		std::lock_guard Lock(Mutex);
		Tasks.erase(TaskId);
	}

	auto FTaskCancellationState::RequestCancellation() -> void
	{
		std::vector<std::shared_ptr<FTaskStateData>> TasksToCancel;
		{
			std::lock_guard Lock(Mutex);
			if (bCancellationRequested)
			{
				return;
			}
			bCancellationRequested = true;
			TasksToCancel.reserve(Tasks.size());
			for (const auto& RegisteredTask : Tasks)
			{
				if (std::shared_ptr<FTaskStateData> PinnedTask = RegisteredTask.second.lock())
				{
					TasksToCancel.emplace_back(std::move(PinnedTask));
				}
			}
			Tasks.clear();
		}
		for (const std::shared_ptr<FTaskStateData>& Task : TasksToCancel)
		{
			Task->RequestCancellation("Task cancellation was requested by its shared source.");
		}
	}

	FTaskCancellationToken::FTaskCancellationToken() = default;

	FTaskCancellationToken::FTaskCancellationToken(
		std::shared_ptr<FTaskCancellationState> InSharedState,
		std::weak_ptr<FTaskStateData> InTaskState
	)
		: SharedState(std::move(InSharedState))
		, TaskState(std::move(InTaskState))
	{
	}

	auto FTaskCancellationToken::IsCancellationRequested() const -> bool
	{
		if (std::shared_ptr<FTaskStateData> PinnedTask = TaskState.lock(); PinnedTask && PinnedTask->IsCancellationRequested())
		{
			return true;
		}
		return SharedState && SharedState->IsCancellationRequested();
	}

	FTaskCancellationSource::FTaskCancellationSource()
		: State(std::make_shared<FTaskCancellationState>())
	{
	}

	auto FTaskCancellationSource::GetToken() const -> FTaskCancellationToken
	{
		return FTaskCancellationToken(State, {});
	}

	auto FTaskCancellationSource::IsCancellationRequested() const -> bool
	{
		return State && State->IsCancellationRequested();
	}

	auto FTaskCancellationSource::RequestCancellation() -> void
	{
		if (State)
		{
			State->RequestCancellation();
		}
	}

	FParallelForCancellationToken::FParallelForCancellationToken(FTaskCancellationToken InGroupToken, FTaskCancellationToken InExternalToken)
		: GroupToken(std::move(InGroupToken))
		, ExternalToken(std::move(InExternalToken))
	{
	}

	auto FParallelForCancellationToken::IsCancellationRequested() const -> bool
	{
		return GroupToken.IsCancellationRequested() || ExternalToken.IsCancellationRequested();
	}

	FTaskHandle::FTaskHandle() = default;

	FTaskHandle::FTaskHandle(std::shared_ptr<FTaskStateData> InState)
		: State(std::move(InState))
	{
	}

	auto FTaskHandle::IsValid() const -> bool
	{
		return State != nullptr;
	}

	auto FTaskHandle::IsComplete() const -> bool
	{
		return State && IsTerminalState(State->GetState());
	}

	auto FTaskHandle::GetState() const -> ETaskState
	{
		return State ? State->GetState() : ETaskState::Invalid;
	}

	auto FTaskHandle::GetDebugName() const -> const char*
	{
		return State ? State->GetDebugName() : "";
	}

	auto FTaskHandle::GetTaskId() const -> uint64
	{
		return State ? State->GetTaskId() : 0;
	}

	auto FTaskHandle::GetDiagnostic() const -> std::string
	{
		return State ? State->GetDiagnostic() : std::string{};
	}

	auto FTaskHandle::GetDiagnostics() const -> FTaskDiagnostics
	{
		return State ? State->GetDiagnostics() : FTaskDiagnostics{};
	}

	auto InitializeTaskScheduler(uint32 InNumThreads) -> bool
	{
		std::lock_guard Lock(GTaskSchedulerMutex);
		if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running)
		{
			DURIN_WARN("Task scheduler initialization ignored because it is already running.");
			return true;
		}
		if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::ShuttingDown)
		{
			DURIN_ERROR("Task scheduler initialization rejected while shutdown is in progress.");
			return false;
		}

		GTaskScheduler = FTaskScheduler::Create(InNumThreads);
		if (!GTaskScheduler)
		{
			DURIN_ERROR("Task scheduler initialization failed.");
			return false;
		}

		GLastTaskSchedulerDiagnostics = {};
		GTaskSchedulerLifetime = ETaskSchedulerLifetime::Running;
		DURIN_DEBUG("Task scheduler initialized. (workers: {})", InNumThreads > 0 ? InNumThreads : GetDefaultThreadPoolThreadCount());
		return true;
	}

	auto ShutdownTaskScheduler(bool bWaitForQueuedWork) -> void
	{
		std::shared_ptr<FTaskScheduler> SchedulerToDestroy;
		{
			std::unique_lock Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::ShuttingDown)
			{
				const uint64 ObservedCompletedShutdowns = GCompletedTaskSchedulerShutdowns;
				GTaskSchedulerCV.wait(Lock, [ObservedCompletedShutdowns]() {
					return GCompletedTaskSchedulerShutdowns > ObservedCompletedShutdowns;
				});
				return;
			}
			if (GTaskSchedulerLifetime == ETaskSchedulerLifetime::Stopped)
			{
				return;
			}

			GTaskSchedulerLifetime = ETaskSchedulerLifetime::ShuttingDown;
			SchedulerToDestroy = GTaskScheduler;
			SchedulerToDestroy->CloseAdmission();
		}

		SchedulerToDestroy->Shutdown(bWaitForQueuedWork);
		FTaskSchedulerDiagnostics ShutdownDiagnostics;
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			check(GTaskScheduler == SchedulerToDestroy);
			ShutdownDiagnostics = SchedulerToDestroy->GetDiagnostics(false);
			GLastTaskSchedulerDiagnostics = ShutdownDiagnostics;
			GTaskScheduler.reset();
			GTaskSchedulerLifetime = ETaskSchedulerLifetime::Stopped;
			++GCompletedTaskSchedulerShutdowns;
		}
		GTaskSchedulerCV.notify_all();
		DURIN_DEBUG(
			"Task scheduler shut down. (drained: {}, completed: {}, failed: {}, canceled: {}, rejected: {}, retained terminal handles: {})",
			bWaitForQueuedWork,
			ShutdownDiagnostics.CompletedTaskCount,
			ShutdownDiagnostics.FailedTaskCount,
			ShutdownDiagnostics.CanceledTaskCount,
			ShutdownDiagnostics.RejectedTaskCount,
			ShutdownDiagnostics.RetainedTerminalHandleCount
		);
	}

	auto IsTaskSchedulerRunning() -> bool
	{
		std::lock_guard Lock(GTaskSchedulerMutex);
		return GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running;
	}

	auto GetTaskSchedulerDiagnostics() -> FTaskSchedulerDiagnostics
	{
		std::lock_guard Lock(GTaskSchedulerMutex);
		if (GTaskScheduler)
		{
			return GTaskScheduler->GetDiagnostics(GTaskSchedulerLifetime == ETaskSchedulerLifetime::Running);
		}
		return GLastTaskSchedulerDiagnostics;
	}

	auto LaunchTask(const char* Name, FTaskFunction&& Function, const FTaskLaunchOptions& Options) -> FTaskHandle
	{
		if (!Function)
		{
			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskScheduler)
			{
				GTaskScheduler->RecordRejectedTask();
			}
			DURIN_WARN("Task launch failed because the task function is empty. (task: {})", Name ? Name : "");
			return {};
		}
		return LaunchCancelableTask(
			Name,
			[Function = std::move(Function)](const FTaskCancellationToken&) mutable {
				Function();
			},
			Options
		);
	}

	auto LaunchCancelableTask(const char* Name, FCancelableTaskFunction&& Function, const FTaskLaunchOptions& Options) -> FTaskHandle
	{
		return Private::LaunchCancelableTaskWithCompletion(Name, std::move(Function), {}, Options);
	}

	namespace Private
	{
		auto LaunchCancelableTaskWithCompletion(
			const char* Name,
			FCancelableTaskFunction&& Function,
			std::function<void(ETaskState)>&& CompletionFunction,
			const FTaskLaunchOptions& Options) -> FTaskHandle
		{
			if (!Function)
			{
				std::lock_guard Lock(GTaskSchedulerMutex);
				if (GTaskScheduler)
				{
					GTaskScheduler->RecordRejectedTask();
				}
				DURIN_WARN("Task launch failed because the task function is empty. (task: {})", Name ? Name : "");
				return {};
			}

			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running)
			{
				if (GTaskScheduler)
				{
					GTaskScheduler->RecordRejectedTask();
				}
				DURIN_WARN("Task launch failed because the task scheduler is not running. (task: {})", Name ? Name : "");
				return {};
			}

			std::shared_ptr<FTaskStateData> State = GTaskScheduler->Submit(
				Name,
				std::move(Function),
				std::move(CompletionFunction),
				Options
			);
			if (!State)
			{
				DURIN_WARN("Task launch failed because its prerequisites were invalid or scheduler admission was closed. (task: {})", Name ? Name : "");
				return {};
			}
			return FTaskHandle(std::move(State));
		}

		auto LaunchContinuationTask(
			const FTaskHandle& Predecessor,
			const char* Name,
			FCancelableTaskFunction&& Function,
			std::function<void(ETaskState)>&& CompletionFunction,
			const FTaskContinuationOptions& Options,
			ETaskDependencyKind DependencyKind) -> FTaskHandle
		{
			if (!Function)
			{
				std::lock_guard Lock(GTaskSchedulerMutex);
				if (GTaskScheduler) GTaskScheduler->RecordRejectedTask();
				return {};
			}
			if (Options.Target != ETaskTarget::AnyWorker)
			{
				std::lock_guard Lock(GTaskSchedulerMutex);
				if (GTaskScheduler) GTaskScheduler->RecordRejectedTask();
				DURIN_WARN("Task continuation target is unavailable in the worker-only implementation. (task: {})", Name ? Name : "");
				return {};
			}

			std::vector<FTaskHandle> Prerequisites;
			Prerequisites.reserve(Options.Prerequisites.size() + 1);
			auto AppendUnique = [&Prerequisites](const FTaskHandle& Candidate) {
				if (std::ranges::none_of(Prerequisites, [&Candidate](const FTaskHandle& Existing) {
					return Existing.GetTaskId() == Candidate.GetTaskId();
				}))
				{
					Prerequisites.emplace_back(Candidate);
				}
			};
			AppendUnique(Predecessor);
			for (const FTaskHandle& Prerequisite : Options.Prerequisites) AppendUnique(Prerequisite);

			FTaskLaunchOptions LaunchOptions;
			LaunchOptions.Prerequisites = Prerequisites;
			LaunchOptions.CancellationToken = Options.CancellationToken;

			std::lock_guard Lock(GTaskSchedulerMutex);
			if (GTaskSchedulerLifetime != ETaskSchedulerLifetime::Running)
			{
				if (GTaskScheduler) GTaskScheduler->RecordRejectedTask();
				return {};
			}
			std::shared_ptr<FTaskStateData> State = GTaskScheduler->Submit(
				Name,
				std::move(Function),
				std::move(CompletionFunction),
				LaunchOptions,
				DependencyKind,
				true
			);
			return State ? FTaskHandle(std::move(State)) : FTaskHandle{};
		}
	} // namespace Private

	auto CancelTask(const FTaskHandle& Task) -> bool
	{
		return Task.State && Task.State->RequestCancellation("Task cancellation was requested through its handle.");
	}

	auto WaitTask(const FTaskHandle& Task) -> ETaskState
	{
		if (!Task.State)
		{
			return ETaskState::Invalid;
		}

		ETaskState State = Task.State->GetState();
		if (IsTerminalState(State))
		{
			return State;
		}

		if (GCurrentTaskState == Task.State.get())
		{
			DURIN_WARN("Task wait rejected because a task cannot wait for itself. (task: {}, id: {})", Task.State->GetDebugName(), Task.State->GetTaskId());
			return State;
		}

		if (GCurrentTaskState && Task.State->DependsOn(GCurrentTaskState))
		{
			DURIN_WARN("Task wait rejected because the target depends on the current task. (task: {}, id: {})", Task.State->GetDebugName(), Task.State->GetTaskId());
			return State;
		}

		if (IsInRenderingThread())
		{
			DURIN_WARN("Task wait rejected on the rendering thread. (task: {}, id: {})", Task.State->GetDebugName(), Task.State->GetTaskId());
			return State;
		}

		std::shared_ptr<FTaskScheduler> Scheduler = Task.State->PinScheduler();
		const auto WaitStartTime = std::chrono::steady_clock::now();
		bool bReportedLongWait = false;
		auto ReportLongWait = [&](bool bThresholdElapsed = false) {
			if (bReportedLongWait || (!bThresholdElapsed && std::chrono::duration<double>(std::chrono::steady_clock::now() - WaitStartTime).count() < LongWaitThresholdSeconds))
			{
				return;
			}
			bReportedLongWait = true;
			if (Scheduler)
			{
				const char* WaiterName = GCurrentTaskState
					? GCurrentTaskState->GetDebugName()
					: (GetCurrentThread() ? GetCurrentThread()->GetThreadName() : "ExternalThread");
				const FTaskDiagnostics TargetDiagnostics = Task.State->GetDiagnostics();
				const uint64 ElapsedNanoseconds = static_cast<uint64>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - WaitStartTime).count()
				);
				Scheduler->RecordLongWait(
					WaiterName,
					TargetDiagnostics,
					ElapsedNanoseconds
				);
				DURIN_WARN(
					"Long task wait detected. (waiter: {}, target: {}, target id: {}, state: {}, elapsed milliseconds: {})",
					WaiterName,
					TargetDiagnostics.DebugName,
					TargetDiagnostics.TaskId,
					static_cast<uint32>(TargetDiagnostics.State),
					ElapsedNanoseconds / 1'000'000
				);
			}
		};
		if (GCurrentTaskState && Scheduler && GCurrentTaskScheduler == Scheduler.get())
		{
			while (!IsTerminalState(State))
			{
				if (Scheduler->TryExecuteOneQueuedTask())
				{
					State = Task.State->GetState();
					ReportLongWait();
					continue;
				}
				State = Task.State->WaitFor(WorkerWaitSliceSeconds);
				ReportLongWait();
			}
			return State;
		}

		State = Task.State->WaitFor(LongWaitThresholdSeconds);
		if (!IsTerminalState(State))
		{
			ReportLongWait(true);
			State = Task.State->Wait();
		}
		return State;
	}

	auto WaitAll(std::span<const FTaskHandle> Tasks) -> std::vector<ETaskState>
	{
		std::vector<ETaskState> Outcomes;
		Outcomes.reserve(Tasks.size());
		for (const FTaskHandle& Task : Tasks)
		{
			Outcomes.push_back(WaitTask(Task));
		}
		return Outcomes;
	}

	auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function, const FParallelForOptions& Options) -> FParallelForResult
	{
		if (!Function)
		{
			return {ETaskState::Invalid, "ParallelFor function is empty.", 0};
		}
		return ParallelForCancelable(
			Name,
			Num,
			[Function = std::move(Function)](uint64 Index, const FParallelForCancellationToken&) mutable {
				Function(Index);
			},
			Options
		);
	}

	auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options) -> FParallelForResult
	{
		if (!Function)
		{
			return {ETaskState::Invalid, "ParallelFor function is empty.", 0};
		}
		if (Num == 0)
		{
			return {ETaskState::Succeeded, {}, 0};
		}
		if (Options.CancellationToken.IsCancellationRequested())
		{
			return {ETaskState::Canceled, "ParallelFor cancellation was requested before execution.", 0};
		}

		struct FParallelForDepthScope
		{
			FParallelForDepthScope() { ++GParallelForDepth; }
			~FParallelForDepthScope()
			{
				check(GParallelForDepth > 0);
				--GParallelForDepth;
			}
		};

		struct FSharedParallelForState
		{
			std::mutex Mutex;
			FTaskCancellationSource CancellationSource;
			std::shared_ptr<FCancelableParallelForFunction> Function;
			uint64 LowestFailedChunkStart = std::numeric_limits<uint64>::max();
			std::string FailureDiagnostic;
		};

		const bool bNested = GParallelForDepth > 0;
		FParallelForDepthScope CallerDepthScope;
		auto SharedState = std::make_shared<FSharedParallelForState>();
		SharedState->Function = std::make_shared<FCancelableParallelForFunction>(std::move(Function));
		const FTaskCancellationToken ExternalToken = Options.CancellationToken;

		auto RecordFailure = [SharedState](uint64 ChunkStart, uint64 ChunkEnd, std::string Diagnostic) {
			std::lock_guard Lock(SharedState->Mutex);
			if (ChunkStart < SharedState->LowestFailedChunkStart)
			{
				SharedState->LowestFailedChunkStart = ChunkStart;
				SharedState->FailureDiagnostic = "ParallelFor chunk [" + std::to_string(ChunkStart) + ", "
					+ std::to_string(ChunkEnd) + ") failed: " + std::move(Diagnostic);
			}
		};

		auto ExecuteChunk = [SharedState, ExternalToken, RecordFailure](uint64 ChunkStart, uint64 ChunkEnd, const FTaskCancellationToken& TaskToken) {
			FParallelForDepthScope ChunkDepthScope;
			const FParallelForCancellationToken CancellationToken(TaskToken, ExternalToken);
			for (uint64 Index = ChunkStart; Index < ChunkEnd; ++Index)
			{
				if (CancellationToken.IsCancellationRequested())
				{
					SharedState->CancellationSource.RequestCancellation();
					return;
				}
				try
				{
					(*SharedState->Function)(Index, CancellationToken);
				}
				catch (const std::exception& Exception)
				{
					RecordFailure(ChunkStart, ChunkEnd, Exception.what());
					SharedState->CancellationSource.RequestCancellation();
					throw;
				}
				catch (...)
				{
					RecordFailure(ChunkStart, ChunkEnd, "callable threw an unknown exception.");
					SharedState->CancellationSource.RequestCancellation();
					throw;
				}
			}
			if (CancellationToken.IsCancellationRequested())
			{
				SharedState->CancellationSource.RequestCancellation();
			}
		};

		const FTaskSchedulerDiagnostics SchedulerDiagnostics = GetTaskSchedulerDiagnostics();
		const uint64 MinBatchSize = std::max<uint64>(1, Options.MinBatchSize);
		const uint64 BatchLimitedChunks = 1 + (Num - 1) / MinBatchSize;
		const uint64 WorkerLimitedChunks = SchedulerDiagnostics.bRunning ? static_cast<uint64>(SchedulerDiagnostics.WorkerCount) + 1 : 1;
		const uint32 ChunkCount = static_cast<uint32>(std::min<uint64>(Num, std::min(BatchLimitedChunks, WorkerLimitedChunks)));
		const uint32 EffectiveChunkCount = bNested ? 1 : std::max<uint32>(1, ChunkCount);

		auto GetChunkRange = [Num, EffectiveChunkCount](uint32 ChunkIndex) {
			const uint64 BaseChunkSize = Num / EffectiveChunkCount;
			const uint64 LargerChunkCount = Num % EffectiveChunkCount;
			const uint64 ChunkStart = static_cast<uint64>(ChunkIndex) * BaseChunkSize + std::min<uint64>(ChunkIndex, LargerChunkCount);
			const uint64 ChunkSize = BaseChunkSize + (ChunkIndex < LargerChunkCount ? 1 : 0);
			return std::pair<uint64, uint64>{ChunkStart, ChunkStart + ChunkSize};
		};

		std::vector<FTaskHandle> WorkerTasks;
		WorkerTasks.reserve(EffectiveChunkCount - 1);
		FTaskLaunchOptions LaunchOptions;
		LaunchOptions.CancellationToken = SharedState->CancellationSource.GetToken();
		bool bLaunchFailed = false;
		for (uint32 ChunkIndex = 1; ChunkIndex < EffectiveChunkCount; ++ChunkIndex)
		{
			const auto [ChunkStart, ChunkEnd] = GetChunkRange(ChunkIndex);
			FTaskHandle Task = LaunchCancelableTask(
				Name ? Name : "ParallelFor",
				[ExecuteChunk, ChunkStart, ChunkEnd](const FTaskCancellationToken& TaskToken) {
					ExecuteChunk(ChunkStart, ChunkEnd, TaskToken);
				},
				LaunchOptions
			);
			if (!Task.IsValid())
			{
				bLaunchFailed = true;
				SharedState->CancellationSource.RequestCancellation();
				break;
			}
			WorkerTasks.emplace_back(std::move(Task));
		}

		const auto [CallerChunkStart, CallerChunkEnd] = GetChunkRange(0);
		try
		{
			ExecuteChunk(CallerChunkStart, CallerChunkEnd, SharedState->CancellationSource.GetToken());
		}
		catch (...)
		{
			// ExecuteChunk records the stable failure before preserving task-style exception precedence.
		}

		const std::vector<ETaskState> WorkerOutcomes = WaitAll(WorkerTasks);
		bool bAnyCanceled = bLaunchFailed || SharedState->CancellationSource.IsCancellationRequested() || Options.CancellationToken.IsCancellationRequested();
		for (ETaskState Outcome : WorkerOutcomes)
		{
			bAnyCanceled = bAnyCanceled || Outcome == ETaskState::Canceled || Outcome == ETaskState::Invalid;
		}

		{
			std::lock_guard Lock(SharedState->Mutex);
			if (!SharedState->FailureDiagnostic.empty())
			{
				return {ETaskState::Failed, SharedState->FailureDiagnostic, EffectiveChunkCount};
			}
		}
		if (bAnyCanceled)
		{
			return {
				ETaskState::Canceled,
				bLaunchFailed ? "ParallelFor could not launch every worker chunk." : "ParallelFor cancellation prevented full range coverage.",
				EffectiveChunkCount
			};
		}
		return {ETaskState::Succeeded, {}, EffectiveChunkCount};
	}
} // namespace Durin
