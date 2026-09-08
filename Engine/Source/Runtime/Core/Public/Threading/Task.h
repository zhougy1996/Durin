#pragma once

#include "CoreAPI.h"

#include "HAL/Platform.h"
#include "Templates/MoveOnlyFunction.h"
#include "Threading/TaskAdmission.h"

namespace Durin
{
	namespace Private
	{
		struct FTaskAttributionAccess;
		struct FTaskScopeAccess;
		struct FTaskRuntimeAccess;
	}

	class FTaskAttribution
	{
	public:
		constexpr FTaskAttribution() = default;
		auto operator==(const FTaskAttribution&) const -> bool = default;

	private:
		constexpr FTaskAttribution(uint16 InOwnerId, uint16 InCategoryId)
			: OwnerId(InOwnerId), CategoryId(InCategoryId)
		{
		}

		friend struct Private::FTaskAttributionAccess;
		friend CORE_API auto RegisterTaskAttribution(std::string_view Owner, std::string_view Category) -> FTaskAttribution;

		uint16 OwnerId = 0;
		uint16 CategoryId = 0;
	};

	CORE_API auto RegisterTaskAttribution(std::string_view Owner, std::string_view Category) -> FTaskAttribution;

	using FTaskFunction = std::function<void()>;
	class FTaskCancellationToken;
	using FParallelForFunction = std::function<void(uint64)>;
	class FParallelForCancellationToken;
	using FCancelableParallelForFunction = std::function<void(uint64, const FParallelForCancellationToken&)>;

	class FTaskCancellationState;
	class FTaskGenerationState;
	class FTaskHandle;
	class FTaskScheduler;
	class FTaskScope;
	class FTaskScopeState;
	class FTaskStateData;
	template<typename T>
	class TUniqueTaskHandle;
	template<typename T>
	class TUniqueTaskResultState;
	struct FTaskLaunchOptions;
	struct FTaskContinuationOptions;
	struct FTaskWaitResult;
	struct FParallelForOptions;
	struct FParallelForResult;

	// Describes a task handle query or one state in the accepted task lifecycle.
	enum class ETaskState : uint8
	{
		Invalid,
		Waiting,
		Queued,
		Running,
		Succeeded,
		Failed,
		Canceled,
	};

	enum class ETaskWaitStatus : uint8
	{
		Completed,
		InvalidTask,
		SelfWait,
		DependencyCycle,
		UnsupportedThread,
	};

	struct FTaskWaitResult
	{
		ETaskWaitStatus WaitStatus = ETaskWaitStatus::InvalidTask;
		ETaskState TaskState = ETaskState::Invalid;
	};

	enum class ETaskTarget : uint8
	{
		AnyWorker,
		GameThreadDeferred,
		BlockingIO,
	};

	enum class ETaskPriority : uint8
	{
		High,
		Normal,
		Low,
	};

	enum class ETaskShutdownMode : uint8
	{
		Drain,
		Cancel,
	};

	enum class ETaskScopeCloseMode : uint8
	{
		Drain,
		Cancel,
	};

	enum class ETaskScopeCloseResult : uint8
	{
		Closed,
		EscalatedToCancel,
		AlreadyClosed,
		Invalid,
	};

	enum class ETaskScopeWaitResult : uint8
	{
		Quiescent,
		TimedOut,
		ScopeOpen,
		UnsupportedThread,
		Invalid,
	};

	enum class ETaskScopeState : uint8
	{
		Invalid,
		Open,
		ClosingDrain,
		ClosingCancel,
		QuiescentDrain,
		QuiescentCancel,
	};

	enum class ETaskDependencyKind : uint8
	{
		Success,
		Completion,
	};

	enum class ETaskTerminalReason : uint8
	{
		None,
		DependencyFailed,
		DependencyCanceled,
		CancellationRequested,
		DispatchRejected,
		CapacityExhausted,
		Superseded,
		StaleGeneration,
		CallbackFailure,
		ShutdownCanceled,
	};

	namespace Private
	{
		// Allocation-free weak accounting binding keeps post-admission ownership transfer infallible.
		struct FTaskResultAccounting
		{
			std::weak_ptr<FTaskStateData> State;
			CORE_API auto operator()(uint64 Bytes) const -> void;
			explicit operator bool() const { return !State.expired(); }
		};
		CORE_API auto MakeTaskResultAccounting(const FTaskHandle& Task) -> FTaskResultAccounting;
		using FMoveOnlyTaskFunction = TMoveOnlyFunction<void(const FTaskCancellationToken&)>;

		template<typename F, typename... Args>
		concept CTaskInvocable = std::is_move_constructible_v<std::decay_t<F>>
			&& std::is_destructible_v<std::decay_t<F>>
			&& std::is_invocable_v<std::decay_t<F>&, Args...>;

		template<typename F, typename... Args>
		concept CTaskResultInvocable = CTaskInvocable<F, Args...>
			&& (std::is_void_v<std::invoke_result_t<std::decay_t<F>&, Args...>>
				|| (std::is_object_v<std::invoke_result_t<std::decay_t<F>&, Args...>>
					&& !std::is_reference_v<std::invoke_result_t<std::decay_t<F>&, Args...>>));

		template<typename T, typename F, typename... Args>
		concept CExactTaskResultInvocable = CTaskInvocable<F, Args...>
			&& std::same_as<std::remove_cvref_t<std::invoke_result_t<std::decay_t<F>&, Args...>>, T>;

		struct FTaskHandleFactory;
		struct FUniqueTaskAccess;
		// Reports rejection before acceptance; accepted work can still fail during execution.
		CORE_API auto TryLaunchCancelableTaskWithCompletion(const char* Name, FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskLaunchOptions& Options, uint64 EstimatedResultBytes = 0) -> Tasks::TTaskAdmission<FTaskHandle>;
		CORE_API auto TryLaunchContinuationTask(const FTaskHandle& Predecessor, const char* Name, FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskContinuationOptions& Options, ETaskDependencyKind DependencyKind, uint64 EstimatedResultBytes = 0) -> Tasks::TTaskAdmission<FTaskHandle>;
		CORE_API auto ValidateTaskExecution(ETaskTarget Target, ETaskPriority Priority, uint64 PayloadBytes, bool bQueueOnSaturation = false) -> std::optional<Tasks::FTaskAdmissionError>;
		CORE_API auto MakeTaskRetainedResultBytesSetter(const FTaskHandle& Task) -> std::function<void(uint64)>;
		// Native-test seam for pausing after the raw terminal transition and before completion publication.
		// Injects bad_alloc at checkpoints 1-5 before acceptance or 6 during dispatch; zero disables it.
		CORE_API auto SetTaskAdmissionAllocationFailureForTests(int32 Checkpoint) -> void;
		CORE_API auto SetTaskTerminalPublicationTestHook(std::function<void(uint64)>&& Hook) -> void;
		// Native-test seam for pausing after the active cohort is pinned and scheduler locks are released.
		CORE_API auto SetTaskSchedulerSnapshotTestHook(std::function<void()>&& Hook) -> void;
		// Native-test seam for restoring the process-scoped attribution registry after capacity qualification.
		CORE_API auto ResetTaskAttributionRegistryForTests() -> void;
	}

	// A copied, thread-safe view of one task's identity, relationships, timing, and outcome.
	struct FTaskDiagnostics
	{
		uint64 TaskId = 0;
		uint64 ScopeId = 0;
		uint64 ParentTaskId = 0;
		std::vector<uint64> PrerequisiteTaskIds;
		std::vector<ETaskDependencyKind> PrerequisiteDependencyKinds;
		uint64 DirectBlockingTaskId = 0;
		uint64 EnqueueTimeNanoseconds = 0;
		uint64 DispatchTimeNanoseconds = 0;
		uint64 QueueResidencyNanoseconds = 0;
		uint64 StartTimeNanoseconds = 0;
		uint64 FinishTimeNanoseconds = 0;
		uint32 ExecutingThreadId = 0;
		std::string DebugName;
		std::string ExecutingThreadName;
		std::string Diagnostic;
		ETaskState State = ETaskState::Invalid;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		ETaskTerminalReason TerminalReason = ETaskTerminalReason::None;
		uint64 EstimatedPayloadBytes = 0;
		uint64 EstimatedResultBytes = 0;
		uint64 RetainedResultBytes = 0;
		uint64 CoalescingOwnerDomain = 0;
		uint64 CoalescingWorkId = 0;
		uint64 CoalescingGeneration = 0;
		bool bHasResultStorage = false;
		uint16 AttributionOwnerId = 0;
		uint16 AttributionCategoryId = 0;
		std::string AttributionOwner;
		std::string AttributionCategory;
		uint64 CallableStorageBytes = 0;
		uint64 ExecutionNanoseconds = 0;
	};

	struct FTaskScopeDiagnostics
	{
		ETaskScopeState State = ETaskScopeState::Invalid;
		uint64 ScopeId = 0;
		uint64 AcceptedCount = 0;
		uint64 RejectedCount = 0;
		uint64 SucceededCount = 0;
		uint64 FailedCount = 0;
		uint64 CanceledCount = 0;
		uint64 CurrentActiveCount = 0;
		uint64 PeakActiveCount = 0;
		uint64 CurrentRetainedResultCount = 0;
		uint64 PeakRetainedResultCount = 0;
		std::vector<FTaskDiagnostics> NonterminalTasks;
		uint64 NonterminalSnapshotTruncationCount = 0;
	};

	struct FTaskOwnerCategoryDiagnostics
	{
		uint16 OwnerId = 0;
		uint16 CategoryId = 0;
		std::string Owner;
		std::string Category;
		uint64 AcceptedCount = 0;
		uint64 SucceededCount = 0;
		uint64 FailedCount = 0;
		uint64 CanceledCount = 0;
		uint64 RejectedCount = 0;
		uint64 DependencyFailedCount = 0;
		uint64 DependencyCanceledCount = 0;
		uint64 CancellationRequestedCount = 0;
		uint64 DispatchRejectedCount = 0;
		uint64 CapacityExhaustedCount = 0;
		uint64 SupersededCount = 0;
		uint64 StaleGenerationCount = 0;
		uint64 CallbackFailureCount = 0;
		uint64 ShutdownCanceledCount = 0;
		uint64 CurrentWaitingCount = 0;
		uint64 CurrentQueuedCount = 0;
		uint64 CurrentRunningCount = 0;
		uint64 CurrentNonterminalCount = 0;
		uint64 ParallelForOperationCount = 0;
		uint64 CurrentCallableBytes = 0;
		uint64 PeakCallableBytes = 0;
		uint64 CurrentPayloadBytes = 0;
		uint64 PeakPayloadBytes = 0;
		uint64 CurrentResultBytes = 0;
		uint64 PeakResultBytes = 0;
		uint64 CurrentRetainedUniqueResultBytes = 0;
		uint64 PeakRetainedUniqueResultBytes = 0;
		std::array<uint64, 32> QueueResidencyHistogram{};
		std::array<uint64, 32> ExecutionHistogram{};
		std::array<uint64, 32> CallableBytesHistogram{};
		std::array<uint64, 32> PayloadBytesHistogram{};
		std::array<uint64, 32> ResultBytesHistogram{};
	};

	// Scheduler-owned logical storage, excluding domain allocations behind captures/results.
	struct FTaskExecutorStorageDiagnostics
	{
		uint64 CurrentNodes = 0;
		uint64 PeakNodes = 0;
		uint64 CurrentBytes = 0;
		uint64 PeakBytes = 0;
		uint64 OverloadCrossings = 0;
		uint64 CurrentPendingNodes = 0;
		uint64 PeakPendingNodes = 0;
		uint64 CurrentPendingBytes = 0;
		uint64 PeakPendingBytes = 0;
		uint64 CurrentRunningBodies = 0;
		uint64 PeakRunningBodies = 0;
	};

	// Aggregate counters and currently nonterminal nodes for one scheduler lifetime.
	struct FTaskSchedulerDiagnostics
	{
		uint32 WorkerCount = 0;
		std::array<FTaskExecutorStorageDiagnostics, 3> ExecutorStorage;
		uint64 TaskReservationCapacity = 0;
		uint64 CurrentTaskReservationCount = 0;
		uint64 PeakTaskReservationCount = 0;
		uint32 QueueDepth = 0;
		uint32 BlockingIOWorkerCount = 0;
		uint32 BlockingIOQueueDepth = 0;
		uint32 BlockingIOReservations = 0;
		uint32 BlockingIOCapacity = 0;
		uint32 ActiveWorkerCount = 0;
		uint64 CompletedTaskCount = 0;
		uint64 FailedTaskCount = 0;
		uint64 CanceledTaskCount = 0;
		uint64 RejectedTaskCount = 0;
		uint64 CapacityRejectedTaskCount = 0;
		uint64 LiveScopeCount = 0;
		uint64 OpenScopeCount = 0;
		uint64 NonquiescentScopeCount = 0;
		uint64 AbandonedOpenScopeCount = 0;
		uint64 ScopeRejectedTaskCount = 0;
		uint64 LongWaitCount = 0;
		uint64 NonterminalTaskCount = 0;
		uint64 RetainedTerminalHandleCount = 0;
		uint64 RetainedTerminalResultCount = 0;
		uint64 RetainedUniqueResultBytes = 0;
		uint64 LastLongWaitTargetTaskId = 0;
		uint64 LastLongWaitElapsedNanoseconds = 0;
		std::string LastLongWaiterName;
		std::string LastLongWaitTargetName;
		ETaskState LastLongWaitTargetState = ETaskState::Invalid;
		bool bRunning = false;
		std::vector<FTaskDiagnostics> NonterminalTasks;
		uint64 AttributionRegistrationOverflowCount = 0;
		std::vector<FTaskOwnerCategoryDiagnostics> OwnerCategoryDiagnostics;
	};

	struct FTaskCoalescingKey
	{
		uint64 OwnerDomain = 0;
		uint64 WorkId = 0;
		uint64 Generation = 0;

		auto operator==(const FTaskCoalescingKey&) const -> bool = default;
	};

	class FTaskGenerationToken
	{
	public:
		FTaskGenerationToken() = default;

		CORE_API auto IsCurrent() const -> bool;
		CORE_API auto IsConstrained() const -> bool;

	private:
		FTaskGenerationToken(std::shared_ptr<FTaskGenerationState> InState, uint64 InGeneration);

		friend class FTaskGenerationSource;

		std::shared_ptr<FTaskGenerationState> State;
		uint64 Generation = 0;
	};

	class FTaskGenerationSource
	{
	public:
		CORE_API FTaskGenerationSource();

		CORE_API auto Capture() const -> FTaskGenerationToken;
		CORE_API auto Advance() -> uint64;
		CORE_API auto GetGeneration() const -> uint64;

	private:
		std::shared_ptr<FTaskGenerationState> State;
	};

	struct FGameThreadDeferredWorkQueueConfig
	{
		uint32 MaxQueuedEntries = 1'024;
		uint64 MaxQueuedPayloadBytes = 8ull * 1'024ull * 1'024ull;
		uint64 MaxPayloadBytesPerEntry = 1ull * 1'024ull * 1'024ull;
		uint32 FrameMaxCallbacks = 64;
		double FrameMaxSeconds = 0.001;
		double LongCallbackSeconds = 0.002;
	};

	struct FGameThreadDeferredPumpBudget
	{
		uint32 MaxCallbacks = 64;
		double MaxSeconds = 0.001;
		bool bUnlimited = false;
	};

	struct FGameThreadDeferredPumpResult
	{
		uint32 ExecutedCallbacks = 0;
		uint32 TerminalEntriesSkipped = 0;
		uint64 ElapsedNanoseconds = 0;
	};

	struct FGameThreadDeferredWorkQueueDiagnostics
	{
		uint32 QueueDepth = 0;
		uint32 PeakQueueDepth = 0;
		std::array<uint32, 3> PriorityDepths{};
		uint64 QueuedPayloadBytes = 0;
		uint64 PeakQueuedPayloadBytes = 0;
		uint64 AcceptedCount = 0;
		uint64 RejectedCount = 0;
		uint64 OverloadCrossings = 0;
		uint64 SupersededCount = 0;
		uint64 CanceledCount = 0;
		uint64 ExpiredGenerationCount = 0;
		uint64 CallbackFailureCount = 0;
		uint64 PumpCount = 0;
		uint64 PumpedCallbackCount = 0;
		uint64 PumpTimeNanoseconds = 0;
		uint64 LongCallbackCount = 0;
		uint64 LastLongCallbackTaskId = 0;
		uint64 LastLongCallbackNanoseconds = 0;
		uint64 OldestEntryAgeNanoseconds = 0;
		uint64 AdapterGeneration = 0;
		uint64 ReentrantPumpCount = 0;
		bool bInstalled = false;
		bool bAccepting = false;
	};

	// Observes cancellation requested for one task or a caller-owned task group.
	class FTaskCancellationToken
	{
	public:
		CORE_API FTaskCancellationToken();

		CORE_API auto IsCancellationRequested() const -> bool;

	private:
		FTaskCancellationToken(std::shared_ptr<FTaskCancellationState> InSharedState, std::weak_ptr<FTaskStateData> InTaskState);

		friend class FTaskCancellationSource;
		friend class FTaskStateData;

		std::shared_ptr<FTaskCancellationState> SharedState;
		std::weak_ptr<FTaskStateData> TaskState;
	};

	// Requests cooperative cancellation for every task launched with its token.
	class FTaskCancellationSource
	{
	public:
		CORE_API FTaskCancellationSource();

		CORE_API auto GetToken() const -> FTaskCancellationToken;
		CORE_API auto IsCancellationRequested() const -> bool;
		CORE_API auto RequestCancellation() -> void;

	private:
		std::shared_ptr<FTaskCancellationState> State;
	};

	class FTaskScopeToken
	{
	public:
		FTaskScopeToken() = default;
		auto operator==(const FTaskScopeToken&) const -> bool = default;

	private:
		explicit FTaskScopeToken(std::shared_ptr<FTaskScopeState> InState)
			: State(std::move(InState))
		{
		}

		friend class FTaskScope;
		friend class FTaskScheduler;
		friend class FTaskStateData;
		friend struct Private::FTaskScopeAccess;
		friend CORE_API auto CreateTaskScope() -> FTaskScope;

		std::shared_ptr<FTaskScopeState> State;
	};

	class FTaskScope
	{
	public:
		FTaskScope() = default;
		CORE_API ~FTaskScope();
		FTaskScope(const FTaskScope&) = delete;
		auto operator=(const FTaskScope&) -> FTaskScope& = delete;
		CORE_API FTaskScope(FTaskScope&& Other) noexcept;
		CORE_API auto operator=(FTaskScope&& Other) noexcept -> FTaskScope&;

		CORE_API auto IsValid() const -> bool;
		CORE_API auto GetToken() const -> FTaskScopeToken;
		CORE_API auto Close(ETaskScopeCloseMode Mode) -> ETaskScopeCloseResult;
		CORE_API auto Wait() const -> ETaskScopeWaitResult;
		CORE_API auto WaitFor(double TimeoutSeconds) const -> ETaskScopeWaitResult;
		CORE_API auto GetDiagnostics() const -> FTaskScopeDiagnostics;

	private:
		explicit FTaskScope(std::shared_ptr<FTaskScopeState> InState)
			: State(std::move(InState))
		{
		}

		friend CORE_API auto CreateTaskScope() -> FTaskScope;

		std::shared_ptr<FTaskScopeState> State;
	};

	namespace Private
	{
		// Reports callable storage retained by GameThreadDeferred for one task scope.
		struct FTaskScopeDeferredWorkSnapshot
		{
			uint32 RetainedCallableCount = 0;
			uint64 RetainedCallableBytes = 0;
		};

		// Reports one bounded selected-scope Game Thread pump or cancellation pass.
		struct FTaskScopeDeferredPumpResult
		{
			uint32 ExecutedCallbacks = 0;
			uint32 CanceledCallbacks = 0;
			uint32 DestroyedCallables = 0;
			bool bReentrant = false;
		};

		CORE_API auto IsExecutingTaskScope(const FTaskScopeToken& Scope) -> bool;
		CORE_API auto ProcessGameThreadDeferredScope(
			const FTaskScopeToken& Scope,
			bool bCancel,
			const FGameThreadDeferredPumpBudget& Budget
		) -> FTaskScopeDeferredPumpResult;
		CORE_API auto GetGameThreadDeferredScopeSnapshot(
			const FTaskScopeToken& Scope
		) -> FTaskScopeDeferredWorkSnapshot;
		CORE_API auto WaitForTaskScopeWorkerCallables(
			const FTaskScopeToken& Scope,
			double TimeoutSeconds
		) -> bool;
		CORE_API auto GetTaskScopeWorkerCallableCount(const FTaskScopeToken& Scope) -> uint32;
	}

	// Observes both ParallelFor group cancellation and caller-provided cancellation.
	class FParallelForCancellationToken
	{
	public:
		CORE_API auto IsCancellationRequested() const -> bool;

	private:
		FParallelForCancellationToken(FTaskCancellationToken InGroupToken, FTaskCancellationToken InExternalToken);

		friend CORE_API auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options) -> FParallelForResult;

		FTaskCancellationToken GroupToken;
		FTaskCancellationToken ExternalToken;
	};

	// Shares the completion state of an asynchronously launched task.
	class FTaskHandle
	{
	public:
		CORE_API FTaskHandle();

		CORE_API auto IsValid() const -> bool;
		CORE_API auto IsComplete() const -> bool;
		CORE_API auto GetState() const -> ETaskState;
		CORE_API auto GetDebugName() const -> const char*;
		CORE_API auto GetTaskId() const -> uint64;
		CORE_API auto GetDiagnostic() const -> std::string;
		CORE_API auto GetDiagnostics() const -> FTaskDiagnostics;

	private:
		explicit FTaskHandle(std::shared_ptr<FTaskStateData> InState);

		friend class FTaskScheduler;
		friend struct Private::FTaskRuntimeAccess;
		friend CORE_API auto Private::TryLaunchCancelableTaskWithCompletion(const char* Name, Private::FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskLaunchOptions& Options, uint64 EstimatedResultBytes) -> Tasks::TTaskAdmission<FTaskHandle>;
		friend CORE_API auto Private::TryLaunchContinuationTask(const FTaskHandle& Predecessor, const char* Name, Private::FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskContinuationOptions& Options, ETaskDependencyKind DependencyKind, uint64 EstimatedResultBytes) -> Tasks::TTaskAdmission<FTaskHandle>;
		friend CORE_API auto Private::MakeTaskResultAccounting(const FTaskHandle& Task) -> Private::FTaskResultAccounting;
		friend CORE_API auto Private::MakeTaskRetainedResultBytesSetter(const FTaskHandle& Task) -> std::function<void(uint64)>;
		friend CORE_API auto CancelTask(const FTaskHandle& Task) -> bool;
		friend CORE_API auto WaitTask(const FTaskHandle& Task) -> FTaskWaitResult;

		std::shared_ptr<FTaskStateData> State;
	};

	namespace Private
	{
		// Binding this preallocated internal hook never admits another scheduled node.
		struct FTaskTerminalHook
		{
			std::function<void(ETaskState)> Function;
			std::shared_ptr<FTaskTerminalHook> Next;
		};
		struct FTaskRuntimeAccess
		{
			CORE_API static auto GetCurrentTaskId() -> uint64;
			CORE_API static auto GroupState(const FTaskScopeToken& Scope) -> ETaskState;
			CORE_API static auto CloseGroup(const FTaskScopeToken& Scope, ETaskScopeCloseMode Mode) -> ETaskScopeCloseResult;
			CORE_API static auto WaitGroupFor(const FTaskScopeToken& Scope, double Seconds) -> ETaskScopeWaitResult;
			CORE_API static auto WaitGroup(const FTaskScopeToken& Scope) -> FTaskWaitResult;
			CORE_API static auto GroupDiagnostics(const FTaskScopeToken& Scope) -> FTaskScopeDiagnostics;
			CORE_API static auto DiagnoseGroupDestruction(const FTaskScopeToken& Scope) -> void;
			CORE_API static auto IsCancellationRequested(const FTaskHandle& Task) -> bool;
			CORE_API static auto CancelCurrent() -> void;
			CORE_API static auto GetScope(const FTaskHandle& Task) -> FTaskScopeToken;
			CORE_API static auto GetAttribution(const FTaskHandle& Task) -> FTaskAttribution;
			CORE_API static auto BindTerminal(const FTaskHandle& Task, std::shared_ptr<FTaskTerminalHook> Hook) -> void;
			CORE_API static auto CompleteExternal(const FTaskHandle& Task, ETaskState State) -> void;
			CORE_API static auto BindDynamicDependency(const FTaskHandle& Task, const FTaskHandle& Inner, bool bCancelInner = true) -> std::optional<Tasks::FTaskAdmissionError>;
		};
	}

	// Immutable launch-time relationships and optional shared cancellation.
	struct FTaskLaunchOptions
	{
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		std::span<const FTaskHandle> Prerequisites;
		FTaskCancellationToken CancellationToken;
		FTaskAttribution Attribution;
		FTaskScopeToken Scope;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		uint64 EstimatedPayloadBytes = 0;
		// Internal ordinary construction queues beyond checked reservation limits.
		bool bQueueOnSaturation = false;
		// Internal completion sources stay counted until producer acknowledgement.
		uint64 ExpectedParentTaskId = 0;
		bool bExternalCompletion = false;
		bool bUnknownExecutionRequirement = true;
	};

	struct FTaskContinuationOptions
	{
		bool bQueueOnSaturation = false;
		std::span<const FTaskHandle> Prerequisites;
		FTaskCancellationToken CancellationToken;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		uint64 EstimatedPayloadBytes = 0;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		FTaskAttribution Attribution;
		FTaskScopeToken Scope;
	};

	struct FParallelForOptions
	{
		// Conservative default remains serial until framework qualification selects a crossover.
		uint64 MinBatchSize = std::numeric_limits<uint64>::max();
		FTaskCancellationToken CancellationToken;
		FTaskAttribution Attribution;
		FTaskScopeToken Scope;
	};

	struct FParallelForResult
	{
		ETaskState State = ETaskState::Invalid;
		std::string Diagnostic;
		uint32 ChunkCount = 0;
	};

	struct FTaskSchedulerConfig
	{
		uint32 NumWorkerThreads = 0;
		uint64 MaxNonterminalTasks = 16'384;
		uint32 NumBlockingIOThreads = 2;
		uint32 MaxBlockingIOTasks = 128;
	};

	// Starts the process-owned CPU scheduler. Engine lifecycle starts it once during PreInit.
	CORE_API auto InitializeTaskScheduler(uint32 InNumThreads = 0) -> bool;
	CORE_API auto InitializeTaskScheduler(const FTaskSchedulerConfig& Config) -> bool;
	CORE_API auto CreateTaskScope() -> FTaskScope;
	// Closes admission and either drains or discards all accepted work before returning.
	CORE_API auto ShutdownTaskScheduler(bool bWaitForQueuedWork = true) -> void;
	CORE_API auto InitializeGameThreadDeferredExecutor(const FGameThreadDeferredWorkQueueConfig& Config = {}) -> bool;
	CORE_API auto PumpGameThreadDeferredWork() -> FGameThreadDeferredPumpResult;
	CORE_API auto PumpGameThreadDeferredWork(const FGameThreadDeferredPumpBudget& Budget) -> FGameThreadDeferredPumpResult;
	CORE_API auto GetGameThreadDeferredWorkQueueDiagnostics() -> FGameThreadDeferredWorkQueueDiagnostics;
	CORE_API auto ShutdownTaskSystem(ETaskShutdownMode Mode = ETaskShutdownMode::Drain) -> void;
	CORE_API auto IsTaskSchedulerRunning() -> bool;
	// Returns the live lifetime snapshot, or the final snapshot after shutdown.
	CORE_API auto GetTaskSchedulerDiagnostics() -> FTaskSchedulerDiagnostics;
	// Publishes fixed task aggregates at the engine profiling/frame boundary.
	CORE_API auto PublishTaskSchedulerProfilerPlots() -> void;

	CORE_API auto CancelTask(const FTaskHandle& Task) -> bool;
	CORE_API auto WaitTask(const FTaskHandle& Task) -> FTaskWaitResult;
	// Returns one wait result for each input handle, including invalid handles and rejected waits.
	CORE_API auto WaitAll(std::span<const FTaskHandle> Tasks) -> std::vector<FTaskWaitResult>;

	// Executes [0, Num) in bounded contiguous chunks and includes the calling thread.
	CORE_API auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;
	CORE_API auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;

	template<typename T>
	class TUniqueTaskResultState
	{
	public:
		explicit TUniqueTaskResultState(uint64 InEstimatedResultBytes)
			: EstimatedResultBytes(InEstimatedResultBytes)
		{
		}

		~TUniqueTaskResultState()
		{
			Discard();
		}

		auto BindProducer(const FTaskHandle& InProducer) -> void
		{
			auto Setter = Private::MakeTaskResultAccounting(InProducer);
			std::lock_guard Lock(Mutex);
			if (!RetainedBytesSetter) RetainedBytesSetter = Setter;
			if (bPublished && Value) RetainedBytesSetter(EstimatedResultBytes);
		}

		auto SetPending(T&& InValue) -> void
		{
			std::unique_ptr<T> PendingValue;
			try { PendingValue = std::make_unique<T>(std::move(InValue)); }
			catch (const std::bad_alloc&) { requiref(false, "Task result allocation failed."); std::terminate(); }
			std::lock_guard Lock(Mutex);
			check(!Value && !bCompleted);
			Value = std::move(PendingValue);
		}

		auto Complete(ETaskState State) -> void
		{
			std::unique_ptr<T> DetachedValue;
			{
				std::lock_guard Lock(Mutex);
				bCompleted = true;
				if (State == ETaskState::Succeeded && Value)
				{
					bPublished = true;
					if (RetainedBytesSetter) RetainedBytesSetter(EstimatedResultBytes);
				}
				else
				{
					bDiscarded = true;
					DetachedValue = std::move(Value);
				}
			}
		}

		auto ReserveClaim() -> uint64
		{
			std::lock_guard Lock(Mutex);
			if (ClaimState != EClaimState::Unclaimed) return 0;
			ClaimState = EClaimState::Reserved;
			ReservationToken = NextReservationToken++;
			return ReservationToken;
		}

		auto RollbackClaim(uint64 Token) -> bool
		{
			std::lock_guard Lock(Mutex);
			if (ClaimState != EClaimState::Reserved || ReservationToken != Token) return false;
			ClaimState = EClaimState::Unclaimed;
			ReservationToken = 0;
			return true;
		}

		// Native accounting changes share the ownership lock; no arbitrary callback runs here.
		auto CommitClaim(uint64 Token, const FTaskHandle& Consumer, bool bTransferAccounting = true) -> bool
		{
			auto ConsumerSetter = Private::MakeTaskResultAccounting(Consumer);
			std::lock_guard Lock(Mutex);
			if (ClaimState != EClaimState::Reserved || ReservationToken != Token) return false;
			ClaimState = EClaimState::Claimed;
			ReservationToken = 0;
			if (!bTransferAccounting) return true;
			if (RetainedBytesSetter) RetainedBytesSetter(0);
			RetainedBytesSetter = ConsumerSetter;
			if (bPublished && Value) ConsumerSetter(EstimatedResultBytes);
			return true;
		}

		// Observers retain storage; unique owners must not consume or discard while a reference is in use.
		auto PeekPublished() const -> const T*
		{
			std::lock_guard Lock(Mutex);
			return bPublished && !bConsumed && !bDiscarded ? Value.get() : nullptr;
		}

		auto TakePublished() -> std::unique_ptr<T>
		{
			std::unique_ptr<T> Result;
			{
				std::lock_guard Lock(Mutex);
				if (!bPublished || !Value || bConsumed || bDiscarded) return {};
				bConsumed = true;
				if (RetainedBytesSetter) RetainedBytesSetter(0);
				Result = std::move(Value);
			}
			return Result;
		}

		auto Discard() -> void
		{
			std::unique_ptr<T> DetachedValue;
			{
				std::lock_guard Lock(Mutex);
				if (!Value) return;
				bDiscarded = true;
				if (RetainedBytesSetter) RetainedBytesSetter(0);
				DetachedValue = std::move(Value);
			}
		}

		auto GetEstimatedResultBytes() const -> uint64 { return EstimatedResultBytes; }

	private:
		enum class EClaimState : uint8
		{
			Unclaimed,
			Reserved,
			Claimed,
		};

		mutable std::mutex Mutex;
		std::unique_ptr<T> Value;
		Private::FTaskResultAccounting RetainedBytesSetter;
		uint64 EstimatedResultBytes = 0;
		uint64 ReservationToken = 0;
		uint64 NextReservationToken = 1;
		EClaimState ClaimState = EClaimState::Unclaimed;
		bool bCompleted = false;
		bool bPublished = false;
		bool bConsumed = false;
		bool bDiscarded = false;
	};

	template<typename T>
	class TUniqueTaskHandle
	{
	public:
		TUniqueTaskHandle() = default;
		~TUniqueTaskHandle()
		{
			if (ResultState) ResultState->Discard();
		}
		TUniqueTaskHandle(const TUniqueTaskHandle&) = delete;
		auto operator=(const TUniqueTaskHandle&) -> TUniqueTaskHandle& = delete;
		TUniqueTaskHandle(TUniqueTaskHandle&&) noexcept = default;
		auto operator=(TUniqueTaskHandle&& Other) noexcept -> TUniqueTaskHandle&
		{
			if (this == &Other) return *this;
			if (ResultState) ResultState->Discard();
			Task = std::move(Other.Task);
			ResultState = std::move(Other.ResultState);
			ClaimTombstone = std::move(Other.ClaimTombstone);
			return *this;
		}

		auto IsValid() const -> bool { return Task.IsValid(); }
		auto IsComplete() const -> bool { return Task.IsComplete(); }
		auto GetState() const -> ETaskState { return Task.GetState(); }
		auto GetDebugName() const -> const char* { return Task.GetDebugName(); }
		auto GetTaskId() const -> uint64 { return Task.GetTaskId(); }
		auto GetDiagnostic() const -> std::string { return Task.GetDiagnostic(); }
		auto GetDiagnostics() const -> FTaskDiagnostics { return Task.GetDiagnostics(); }
		auto GetTaskHandle() const -> const FTaskHandle& { return Task; }

	private:
		TUniqueTaskHandle(FTaskHandle InTask, std::shared_ptr<TUniqueTaskResultState<T>> InResultState)
			: Task(std::move(InTask)), ResultState(std::move(InResultState))
		{
		}

		auto InvalidateAfterClaim() -> void
		{
			ClaimTombstone = ResultState;
			Task = {};
			ResultState.reset();
		}

		friend struct Private::FTaskHandleFactory;
		friend struct Private::FUniqueTaskAccess;

		FTaskHandle Task;
		std::shared_ptr<TUniqueTaskResultState<T>> ResultState;
		std::weak_ptr<TUniqueTaskResultState<T>> ClaimTombstone;
	};

	namespace Private
	{
		struct FUniqueTaskAccess
		{
			template<typename T>
			static auto GetTask(TUniqueTaskHandle<T>& Handle) -> FTaskHandle& { return Handle.Task; }
			template<typename T>
			static auto GetResultState(TUniqueTaskHandle<T>& Handle) -> std::shared_ptr<TUniqueTaskResultState<T>>& { return Handle.ResultState; }
			template<typename T>
			static auto GetResultState(const TUniqueTaskHandle<T>& Handle) -> const std::shared_ptr<TUniqueTaskResultState<T>>& { return Handle.ResultState; }
			template<typename T>
			static auto HasClaimTombstone(TUniqueTaskHandle<T>& Handle) -> bool { return !Handle.ClaimTombstone.expired(); }
			template<typename T>
			static auto InvalidateAfterClaim(TUniqueTaskHandle<T>& Handle) -> void { Handle.InvalidateAfterClaim(); }
		};

		struct FTaskHandleFactory
		{
			template<typename T>
			static auto MakeUnique(FTaskHandle Task, std::shared_ptr<TUniqueTaskResultState<T>> ResultState) -> TUniqueTaskHandle<T>
			{
				return TUniqueTaskHandle<T>(std::move(Task), std::move(ResultState));
			}
		};

	}
} // namespace Durin
