#pragma once

#include "CoreAPI.h"

#include "HAL/Platform.h"
#include "Templates/MoveOnlyFunction.h"

namespace Durin
{
	namespace Private
	{
		struct FTaskAttributionAccess;
		struct FTaskScopeAccess;
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
	using FCancelableTaskFunction = std::function<void(const FTaskCancellationToken&)>;
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
	class TTaskResultState;
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
		CORE_API auto LaunchCancelableTaskWithCompletion(const char* Name, FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskLaunchOptions& Options, uint64 EstimatedResultBytes = 0) -> FTaskHandle;
		CORE_API auto LaunchContinuationTask(const FTaskHandle& Predecessor, const char* Name, FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskContinuationOptions& Options, ETaskDependencyKind DependencyKind, uint64 EstimatedResultBytes = 0) -> FTaskHandle;
		CORE_API auto MakeTaskRetainedResultBytesSetter(const FTaskHandle& Task) -> std::function<void(uint64)>;
		// Native-test seam for pausing after the raw terminal transition and before completion publication.
		CORE_API auto SetTaskTerminalPublicationTestHook(std::function<void(uint64)>&& Hook) -> void;
		// Native-test seam for pausing after the active cohort is pinned and scheduler locks are released.
		CORE_API auto SetTaskSchedulerSnapshotTestHook(std::function<void()>&& Hook) -> void;
		// Native-test seam for restoring the process-scoped attribution registry after capacity qualification.
		CORE_API auto ResetTaskAttributionRegistryForTests() -> void;
		CORE_API auto RecordDuplicateUniqueConsumerClaim() -> void;
		CORE_API auto RecordRejectedUniqueTask(const char* Name, const char* Diagnostic, FTaskAttribution Attribution = {}) -> void;
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

	// Aggregate counters and currently nonterminal nodes for one scheduler lifetime.
	struct FTaskSchedulerDiagnostics
	{
		uint32 WorkerCount = 0;
		uint64 TaskReservationCapacity = 0;
		uint64 CurrentTaskReservationCount = 0;
		uint64 PeakTaskReservationCount = 0;
		uint32 QueueDepth = 0;
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
		uint64 DuplicateUniqueConsumerClaimCount = 0;
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
		friend CORE_API auto LaunchTask(const char* Name, FTaskFunction&& Function, const FTaskLaunchOptions& Options) -> FTaskHandle;
		friend CORE_API auto LaunchCancelableTask(const char* Name, FCancelableTaskFunction&& Function, const FTaskLaunchOptions& Options) -> FTaskHandle;
		friend CORE_API auto Private::LaunchCancelableTaskWithCompletion(const char* Name, Private::FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskLaunchOptions& Options, uint64 EstimatedResultBytes) -> FTaskHandle;
		friend CORE_API auto Private::LaunchContinuationTask(const FTaskHandle& Predecessor, const char* Name, Private::FMoveOnlyTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskContinuationOptions& Options, ETaskDependencyKind DependencyKind, uint64 EstimatedResultBytes) -> FTaskHandle;
		friend CORE_API auto Private::MakeTaskRetainedResultBytesSetter(const FTaskHandle& Task) -> std::function<void(uint64)>;
		friend CORE_API auto CancelTask(const FTaskHandle& Task) -> bool;
		friend CORE_API auto WaitTask(const FTaskHandle& Task) -> FTaskWaitResult;

		std::shared_ptr<FTaskStateData> State;
	};

	template<typename T>
	class TTaskHandle
	{
	public:
		TTaskHandle() = default;

		auto IsValid() const -> bool { return Task.IsValid(); }
		auto IsComplete() const -> bool { return Task.IsComplete(); }
		auto GetState() const -> ETaskState { return Task.GetState(); }
		auto GetDebugName() const -> const char* { return Task.GetDebugName(); }
		auto GetTaskId() const -> uint64 { return Task.GetTaskId(); }
		auto GetDiagnostic() const -> std::string { return Task.GetDiagnostic(); }
		auto GetDiagnostics() const -> FTaskDiagnostics { return Task.GetDiagnostics(); }
		auto GetTaskHandle() const -> const FTaskHandle& { return Task; }
		auto GetResultShared() const -> std::shared_ptr<const T>
		{
			return Task.GetState() == ETaskState::Succeeded && ResultState
				? ResultState->GetPublished()
				: std::shared_ptr<const T>{};
		}

	private:
		TTaskHandle(FTaskHandle InTask, std::shared_ptr<TTaskResultState<T>> InResultState)
			: Task(std::move(InTask)), ResultState(std::move(InResultState))
		{
		}

		friend struct Private::FTaskHandleFactory;
		template<typename U>
		friend class TTaskHandle;
		template<typename U>
		friend auto LaunchTask(const char*, std::function<U()>&&, const FTaskLaunchOptions&) -> TTaskHandle<U>;
		template<typename U>
		friend auto LaunchCancelableTask(const char*, std::function<U(const FTaskCancellationToken&)>&&, const FTaskLaunchOptions&) -> TTaskHandle<U>;
		template<typename U, typename F>
		friend auto Then(const TTaskHandle<U>&, const char*, F&&, const FTaskContinuationOptions&);
		template<typename U, typename F>
		friend auto ThenOutcome(const TTaskHandle<U>&, const char*, F&&, const FTaskContinuationOptions&);

		FTaskHandle Task;
		std::shared_ptr<TTaskResultState<T>> ResultState;
	};

	template<typename T>
	struct FTaskOutcome
	{
		FTaskHandle Task;
		std::shared_ptr<const T> Result;
		std::string Diagnostic;
		ETaskState State = ETaskState::Invalid;
		ETaskTerminalReason Reason = ETaskTerminalReason::None;
	};

	template<>
	struct FTaskOutcome<void>
	{
		FTaskHandle Task;
		std::string Diagnostic;
		ETaskState State = ETaskState::Invalid;
		ETaskTerminalReason Reason = ETaskTerminalReason::None;
	};

	template<typename... Ts>
	struct TTaskAggregateOutcome
	{
		std::tuple<FTaskOutcome<Ts>...> Outcomes;
		std::string Diagnostic;
		uint64 BlockingTaskId = 0;
		ETaskState State = ETaskState::Invalid;
		ETaskTerminalReason Reason = ETaskTerminalReason::None;
	};

	// Immutable launch-time relationships and optional shared cancellation.
	struct FTaskLaunchOptions
	{
		std::span<const FTaskHandle> Prerequisites;
		FTaskCancellationToken CancellationToken;
		FTaskAttribution Attribution;
		FTaskScopeToken Scope;
	};

	struct FTaskContinuationOptions
	{
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

	CORE_API auto LaunchTask(const char* Name, FTaskFunction&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle;
	CORE_API auto LaunchCancelableTask(const char* Name, FCancelableTaskFunction&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle;

	template<typename F>
	requires (!std::same_as<std::decay_t<F>, FTaskFunction>
		&& Private::CTaskInvocable<F>
		&& std::is_void_v<std::invoke_result_t<std::decay_t<F>&>>)
	auto LaunchTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle
	{
		return Private::LaunchCancelableTaskWithCompletion(
			Name,
			[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { std::invoke(Function); },
			{},
			Options);
	}

	template<typename F>
	requires (!std::same_as<std::decay_t<F>, FCancelableTaskFunction>
		&& Private::CTaskInvocable<F, const FTaskCancellationToken&>
		&& std::is_void_v<std::invoke_result_t<std::decay_t<F>&, const FTaskCancellationToken&>>)
	auto LaunchCancelableTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle
	{
		return Private::LaunchCancelableTaskWithCompletion(Name, std::forward<F>(Function), {}, Options);
	}
	// Returns false only for an invalid or already-terminal task.
	CORE_API auto CancelTask(const FTaskHandle& Task) -> bool;
	CORE_API auto WaitTask(const FTaskHandle& Task) -> FTaskWaitResult;
	// Returns one wait result for each input handle, including invalid handles and rejected waits.
	CORE_API auto WaitAll(std::span<const FTaskHandle> Tasks) -> std::vector<FTaskWaitResult>;

	// Executes [0, Num) in bounded contiguous chunks and includes the calling thread.
	CORE_API auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;
	CORE_API auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;

	template<typename T>
	class TTaskResultState
	{
	public:
		auto SetPending(T&& Value) -> void
		{
			std::lock_guard Lock(Mutex);
			Pending = std::make_shared<T>(std::move(Value));
		}

		auto Complete(ETaskState State) -> void
		{
			{
				std::lock_guard Lock(Mutex);
				if (State == ETaskState::Succeeded)
				{
					Published = std::move(Pending);
				}
				else
				{
					Pending.reset();
				}
				bCompleted = true;
			}
			CV.notify_all();
		}

		auto GetPublished() const -> std::shared_ptr<const T>
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [this]() { return bCompleted; });
			return Published;
		}

	private:
		mutable std::mutex Mutex;
		mutable std::condition_variable CV;
		std::shared_ptr<T> Pending;
		std::shared_ptr<const T> Published;
		bool bCompleted = false;
	};

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
			bool bRetained = false;
			auto Setter = Private::MakeTaskRetainedResultBytesSetter(InProducer);
			{
				std::lock_guard Lock(Mutex);
				if (!RetainedBytesSetter) RetainedBytesSetter = Setter;
				bRetained = bPublished && static_cast<bool>(Value);
			}
			if (bRetained) Setter(EstimatedResultBytes);
		}

		auto SetPending(T&& InValue) -> void
		{
			auto PendingValue = std::make_unique<T>(std::move(InValue));
			std::lock_guard Lock(Mutex);
			check(!Value && !bCompleted);
			Value = std::move(PendingValue);
		}

		auto Complete(ETaskState State) -> void
		{
			std::unique_ptr<T> DetachedValue;
			std::function<void(uint64)> Setter;
			{
				std::lock_guard Lock(Mutex);
				bCompleted = true;
				if (State == ETaskState::Succeeded && Value)
				{
					bPublished = true;
					Setter = RetainedBytesSetter;
				}
				else
				{
					bDiscarded = true;
					DetachedValue = std::move(Value);
				}
			}
			if (Setter) Setter(EstimatedResultBytes);
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

		auto CommitClaim(uint64 Token, const FTaskHandle& Consumer) -> bool
		{
			std::function<void(uint64)> PreviousSetter;
			auto ConsumerSetter = Private::MakeTaskRetainedResultBytesSetter(Consumer);
			bool bRetained = false;
			{
				std::lock_guard Lock(Mutex);
				if (ClaimState != EClaimState::Reserved || ReservationToken != Token) return false;
				ClaimState = EClaimState::Claimed;
				ReservationToken = 0;
				PreviousSetter = RetainedBytesSetter;
				RetainedBytesSetter = ConsumerSetter;
				bRetained = bPublished && static_cast<bool>(Value);
			}
			if (PreviousSetter) PreviousSetter(0);
			if (bRetained) ConsumerSetter(EstimatedResultBytes);
			return true;
		}

		auto TakePublished() -> std::unique_ptr<T>
		{
			std::unique_ptr<T> Result;
			std::function<void(uint64)> PreviousSetter;
			{
				std::lock_guard Lock(Mutex);
				if (!bPublished || !Value || bConsumed || bDiscarded) return {};
				bConsumed = true;
				PreviousSetter = RetainedBytesSetter;
				Result = std::move(Value);
			}
			if (PreviousSetter) PreviousSetter(0);
			return Result;
		}

		auto Discard() -> void
		{
			std::unique_ptr<T> DetachedValue;
			std::function<void(uint64)> PreviousSetter;
			{
				std::lock_guard Lock(Mutex);
				if (!Value) return;
				bDiscarded = true;
				PreviousSetter = RetainedBytesSetter;
				DetachedValue = std::move(Value);
			}
			if (PreviousSetter) PreviousSetter(0);
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
		std::function<void(uint64)> RetainedBytesSetter;
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

	template<typename T>
	struct FUniqueTaskOutcome
	{
		FTaskHandle Task;
		std::optional<T> Result;
		std::string Diagnostic;
		ETaskState State = ETaskState::Invalid;
		ETaskTerminalReason Reason = ETaskTerminalReason::None;
	};

	namespace Private
	{
		template<typename T>
		auto MakeTaskOutcome(const TTaskHandle<T>& Handle) -> FTaskOutcome<T>
		{
			const FTaskDiagnostics Diagnostics = Handle.GetDiagnostics();
			return {Handle.GetTaskHandle(), Handle.GetResultShared(), Diagnostics.Diagnostic,
				Diagnostics.State, Diagnostics.TerminalReason};
		}

		template<typename... Ts>
		auto MakeFanInContinuationOptions(
			const std::tuple<TTaskHandle<Ts>...>& Predecessors,
			const FTaskContinuationOptions& Options,
			std::vector<FTaskHandle>& PrerequisiteStorage) -> FTaskContinuationOptions
		{
			PrerequisiteStorage.reserve(sizeof...(Ts) + Options.Prerequisites.size());
			std::apply([&PrerequisiteStorage](const auto&... Handle) {
				(PrerequisiteStorage.emplace_back(Handle.GetTaskHandle()), ...);
			}, Predecessors);
			PrerequisiteStorage.insert(PrerequisiteStorage.end(), Options.Prerequisites.begin(), Options.Prerequisites.end());

			FTaskContinuationOptions AdjustedOptions = Options;
			AdjustedOptions.Prerequisites = PrerequisiteStorage;
			return AdjustedOptions;
		}

		template<typename... Ts>
		auto MakeTaskAggregateOutcome(const std::tuple<TTaskHandle<Ts>...>& Predecessors) -> TTaskAggregateOutcome<Ts...>
		{
			TTaskAggregateOutcome<Ts...> Aggregate;
			Aggregate.Outcomes = std::apply([](const auto&... Handle) {
				return std::make_tuple(MakeTaskOutcome(Handle)...);
			}, Predecessors);

			Aggregate.State = ETaskState::Succeeded;
			auto ConsiderOutcome = [&Aggregate](const auto& Outcome) {
				check(Outcome.State == ETaskState::Succeeded
					|| Outcome.State == ETaskState::Failed
					|| Outcome.State == ETaskState::Canceled);
				if (Outcome.State == ETaskState::Succeeded)
				{
					return;
				}
				const uint64 TaskId = Outcome.Task.GetTaskId();
				const bool bSelect = Aggregate.State == ETaskState::Succeeded
					|| (Outcome.State == ETaskState::Failed && Aggregate.State != ETaskState::Failed)
					|| (Outcome.State == Aggregate.State && TaskId < Aggregate.BlockingTaskId);
				if (bSelect)
				{
					Aggregate.State = Outcome.State;
					Aggregate.BlockingTaskId = TaskId;
					Aggregate.Diagnostic = Outcome.Diagnostic;
					Aggregate.Reason = Outcome.Reason;
				}
			};
			std::apply([&ConsiderOutcome](const auto&... Outcome) { (ConsiderOutcome(Outcome), ...); }, Aggregate.Outcomes);
			return Aggregate;
		}

		struct FUniqueTaskAccess
		{
			template<typename T>
			static auto GetTask(TUniqueTaskHandle<T>& Handle) -> FTaskHandle& { return Handle.Task; }
			template<typename T>
			static auto GetResultState(TUniqueTaskHandle<T>& Handle) -> std::shared_ptr<TUniqueTaskResultState<T>>& { return Handle.ResultState; }
			template<typename T>
			static auto HasClaimTombstone(TUniqueTaskHandle<T>& Handle) -> bool { return !Handle.ClaimTombstone.expired(); }
			template<typename T>
			static auto InvalidateAfterClaim(TUniqueTaskHandle<T>& Handle) -> void { Handle.InvalidateAfterClaim(); }
		};

		struct FTaskHandleFactory
		{
			template<typename T>
			static auto Make(FTaskHandle Task, std::shared_ptr<TTaskResultState<T>> ResultState) -> TTaskHandle<T>
			{
				return TTaskHandle<T>(std::move(Task), std::move(ResultState));
			}

			template<typename T>
			static auto MakeUnique(FTaskHandle Task, std::shared_ptr<TUniqueTaskResultState<T>> ResultState) -> TUniqueTaskHandle<T>
			{
				return TUniqueTaskHandle<T>(std::move(Task), std::move(ResultState));
			}
		};

		template<typename T, typename F>
		auto MakeTypedTaskHandle(
			const char* Name,
			F&& Function,
			const FTaskLaunchOptions& Options) -> TTaskHandle<T>
		{
			auto ResultState = std::make_shared<TTaskResultState<T>>();
			FTaskHandle Task = Private::LaunchCancelableTaskWithCompletion(
				Name,
				[Function = std::forward<F>(Function), ResultState](const FTaskCancellationToken& Token) mutable {
					ResultState->SetPending(std::invoke(Function, Token));
				},
				[ResultState](ETaskState State) { ResultState->Complete(State); },
				Options
			);
			return Task.IsValid()
				? FTaskHandleFactory::Make(std::move(Task), std::move(ResultState))
				: TTaskHandle<T>{};
		}

		template<typename U>
		using TContinuationHandle = std::conditional_t<std::is_void_v<U>, FTaskHandle, TTaskHandle<U>>;

		template<typename U, typename F>
		auto LaunchContinuationResult(
			const FTaskHandle& Predecessor,
			const char* Name,
			F&& Function,
			const FTaskContinuationOptions& Options,
			ETaskDependencyKind DependencyKind) -> TContinuationHandle<U>
		{
			if constexpr (std::is_void_v<U>)
			{
				return Private::LaunchContinuationTask(
					Predecessor,
					Name,
					[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { Function(); },
					{},
					Options,
					DependencyKind
				);
			}
			else
			{
				auto ResultState = std::make_shared<TTaskResultState<U>>();
				FTaskHandle Task = Private::LaunchContinuationTask(
					Predecessor,
					Name,
					[Function = std::forward<F>(Function), ResultState](const FTaskCancellationToken&) mutable {
						ResultState->SetPending(Function());
					},
					[ResultState](ETaskState State) { ResultState->Complete(State); },
					Options,
					DependencyKind
				);
				return Task.IsValid()
					? FTaskHandleFactory::Make(std::move(Task), std::move(ResultState))
					: TTaskHandle<U>{};
			}
		}
	} // namespace Private

	template<typename T>
	auto LaunchTask(const char* Name, std::function<T()>&& Function, const FTaskLaunchOptions& Options = {}) -> TTaskHandle<T>
	{
		static_assert(!std::is_void_v<T>);
		if (!Function)
		{
			(void)LaunchTask(Name, FTaskFunction{}, Options);
			return {};
		}
		return Private::MakeTypedTaskHandle<T>(Name,
			[Function = std::move(Function)](const FTaskCancellationToken&) mutable { return Function(); }, Options);
	}

	template<typename T>
	auto LaunchCancelableTask(const char* Name, std::function<T(const FTaskCancellationToken&)>&& Function, const FTaskLaunchOptions& Options = {}) -> TTaskHandle<T>
	{
		static_assert(!std::is_void_v<T>);
		if (!Function)
		{
			(void)LaunchCancelableTask(Name, FCancelableTaskFunction{}, Options);
			return {};
		}
		return Private::MakeTypedTaskHandle<T>(Name, std::move(Function), Options);
	}

	template<typename T, typename F>
	requires (!std::same_as<std::decay_t<F>, std::function<T()>>
		&& !std::is_void_v<T>
		&& Private::CExactTaskResultInvocable<T, F>)
	auto LaunchTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> TTaskHandle<T>
	{
		return Private::MakeTypedTaskHandle<T>(Name,
			[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable -> T {
				return std::invoke(Function);
			}, Options);
	}

	template<typename T, typename F>
	requires (!std::same_as<std::decay_t<F>, std::function<T(const FTaskCancellationToken&)>>
		&& !std::is_void_v<T>
		&& Private::CExactTaskResultInvocable<T, F, const FTaskCancellationToken&>)
	auto LaunchCancelableTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> TTaskHandle<T>
	{
		return Private::MakeTypedTaskHandle<T>(Name, std::forward<F>(Function), Options);
	}

	template<typename T, typename F>
	requires (!std::is_void_v<T>
		&& !std::is_reference_v<T>
		&& std::is_object_v<T>
		&& std::is_move_constructible_v<T>
		&& std::is_destructible_v<T>
		&& Private::CExactTaskResultInvocable<T, F>)
	auto LaunchUniqueTask(
		const char* Name,
		F&& Function,
		const FTaskLaunchOptions& Options = {},
		uint64 EstimatedResultBytes = sizeof(T)) -> TUniqueTaskHandle<T>
	{
		if (EstimatedResultBytes == 0)
		{
			if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>)
			{
				EstimatedResultBytes = sizeof(T);
			}
			else
			{
				Private::RecordRejectedUniqueTask(Name,
					"Unique task launch failed because retained result bytes must be non-zero for this result type.", Options.Attribution);
				return {};
			}
		}

		auto ResultState = std::make_shared<TUniqueTaskResultState<T>>(EstimatedResultBytes);
		FTaskHandle Task = Private::LaunchCancelableTaskWithCompletion(
			Name,
			[Function = std::forward<F>(Function), ResultState](const FTaskCancellationToken&) mutable {
				ResultState->SetPending(std::invoke(Function));
			},
			[ResultState](ETaskState State) { ResultState->Complete(State); },
			Options,
			EstimatedResultBytes);
		if (!Task.IsValid()) return {};
		ResultState->BindProducer(Task);
		return Private::FTaskHandleFactory::MakeUnique(std::move(Task), std::move(ResultState));
	}

	template<typename T, typename F>
	requires (!std::is_void_v<T>
		&& !std::is_reference_v<T>
		&& std::is_object_v<T>
		&& std::is_move_constructible_v<T>
		&& std::is_destructible_v<T>
		&& Private::CExactTaskResultInvocable<T, F, const FTaskCancellationToken&>)
	auto LaunchUniqueCancelableTask(
		const char* Name,
		F&& Function,
		const FTaskLaunchOptions& Options = {},
		uint64 EstimatedResultBytes = sizeof(T)) -> TUniqueTaskHandle<T>
	{
		if (EstimatedResultBytes == 0)
		{
			if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>) EstimatedResultBytes = sizeof(T);
			else
			{
				Private::RecordRejectedUniqueTask(Name,
					"Unique task launch failed because retained result bytes must be non-zero for this result type.", Options.Attribution);
				return {};
			}
		}

		auto ResultState = std::make_shared<TUniqueTaskResultState<T>>(EstimatedResultBytes);
		FTaskHandle Task = Private::LaunchCancelableTaskWithCompletion(
			Name,
			[Function = std::forward<F>(Function), ResultState](const FTaskCancellationToken& Token) mutable {
				ResultState->SetPending(std::invoke(Function, Token));
			},
			[ResultState](ETaskState State) { ResultState->Complete(State); },
			Options,
			EstimatedResultBytes);
		if (!Task.IsValid()) return {};
		ResultState->BindProducer(Task);
		return Private::FTaskHandleFactory::MakeUnique(std::move(Task), std::move(ResultState));
	}

	template<typename T, typename F>
	requires Private::CTaskInvocable<F, T&&>
		&& std::is_void_v<std::invoke_result_t<std::decay_t<F>&, T&&>>
	auto ConsumeThen(
		TUniqueTaskHandle<T>&& Predecessor,
		const char* Name,
		F&& Function,
		const FTaskContinuationOptions& Options = {}) -> FTaskHandle
	{
		auto& PredecessorTask = Private::FUniqueTaskAccess::GetTask(Predecessor);
		std::shared_ptr<TUniqueTaskResultState<T>> ResultState = Private::FUniqueTaskAccess::GetResultState(Predecessor);
		if (!PredecessorTask.IsValid() || !ResultState)
		{
			if (Private::FUniqueTaskAccess::HasClaimTombstone(Predecessor)) Private::RecordDuplicateUniqueConsumerClaim();
			return {};
		}

		FTaskContinuationOptions AdjustedOptions = Options;
		const uint64 ResultBytes = ResultState->GetEstimatedResultBytes();
		if (Options.Target == ETaskTarget::GameThreadDeferred)
		{
			if (Options.EstimatedPayloadBytes > std::numeric_limits<uint64>::max() - ResultBytes)
			{
				Private::RecordRejectedUniqueTask(Name,
					"Unique task consumer dispatch rejected because payload and retained result bytes overflow uint64.", Options.Attribution);
				return {};
			}
			AdjustedOptions.EstimatedPayloadBytes += ResultBytes;
		}

		const uint64 ClaimToken = ResultState->ReserveClaim();
		if (ClaimToken == 0)
		{
			Private::RecordDuplicateUniqueConsumerClaim();
			return {};
		}

		FTaskHandle Consumer = Private::LaunchContinuationTask(
			PredecessorTask,
			Name,
			[ResultState, Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable {
				std::unique_ptr<T> Value = ResultState->TakePublished();
				check(Value);
				std::invoke(Function, std::move(*Value));
			},
			[ResultState](ETaskState) { ResultState->Discard(); },
			AdjustedOptions,
			ETaskDependencyKind::Success,
			ResultBytes);
		if (!Consumer.IsValid())
		{
			ResultState->RollbackClaim(ClaimToken);
			return {};
		}
		if (!ResultState->CommitClaim(ClaimToken, Consumer))
		{
			Private::RecordDuplicateUniqueConsumerClaim();
			CancelTask(Consumer);
			return {};
		}
		Private::FUniqueTaskAccess::InvalidateAfterClaim(Predecessor);
		return Consumer;
	}

	template<typename T, typename F>
	requires Private::CTaskInvocable<F, FUniqueTaskOutcome<T>&&>
		&& std::is_void_v<std::invoke_result_t<std::decay_t<F>&, FUniqueTaskOutcome<T>&&>>
	auto ConsumeThenOutcome(
		TUniqueTaskHandle<T>&& Predecessor,
		const char* Name,
		F&& Function,
		const FTaskContinuationOptions& Options = {}) -> FTaskHandle
	{
		auto& PredecessorTask = Private::FUniqueTaskAccess::GetTask(Predecessor);
		std::shared_ptr<TUniqueTaskResultState<T>> ResultState = Private::FUniqueTaskAccess::GetResultState(Predecessor);
		if (!PredecessorTask.IsValid() || !ResultState)
		{
			if (Private::FUniqueTaskAccess::HasClaimTombstone(Predecessor)) Private::RecordDuplicateUniqueConsumerClaim();
			return {};
		}

		FTaskContinuationOptions AdjustedOptions = Options;
		const uint64 ResultBytes = ResultState->GetEstimatedResultBytes();
		if (Options.Target == ETaskTarget::GameThreadDeferred)
		{
			if (Options.EstimatedPayloadBytes > std::numeric_limits<uint64>::max() - ResultBytes)
			{
				Private::RecordRejectedUniqueTask(Name,
					"Unique task consumer dispatch rejected because payload and retained result bytes overflow uint64.", Options.Attribution);
				return {};
			}
			AdjustedOptions.EstimatedPayloadBytes += ResultBytes;
		}

		const uint64 ClaimToken = ResultState->ReserveClaim();
		if (ClaimToken == 0)
		{
			Private::RecordDuplicateUniqueConsumerClaim();
			return {};
		}
		const FTaskHandle ProducerTask = PredecessorTask;
		FTaskHandle Consumer = Private::LaunchContinuationTask(
			ProducerTask,
			Name,
			[ProducerTask, ResultState, Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable {
				const FTaskDiagnostics Diagnostics = ProducerTask.GetDiagnostics();
				FUniqueTaskOutcome<T> Outcome{
					.Task = ProducerTask,
					.Diagnostic = Diagnostics.Diagnostic,
					.State = Diagnostics.State,
					.Reason = Diagnostics.TerminalReason,
				};
				if (Diagnostics.State == ETaskState::Succeeded)
				{
					std::unique_ptr<T> Value = ResultState->TakePublished();
					check(Value);
					Outcome.Result.emplace(std::move(*Value));
				}
				std::invoke(Function, std::move(Outcome));
			},
			[ResultState](ETaskState) { ResultState->Discard(); },
			AdjustedOptions,
			ETaskDependencyKind::Completion,
			ResultBytes);
		if (!Consumer.IsValid())
		{
			ResultState->RollbackClaim(ClaimToken);
			return {};
		}
		if (!ResultState->CommitClaim(ClaimToken, Consumer))
		{
			Private::RecordDuplicateUniqueConsumerClaim();
			CancelTask(Consumer);
			return {};
		}
		Private::FUniqueTaskAccess::InvalidateAfterClaim(Predecessor);
		return Consumer;
	}

	template<typename T, typename F>
	requires Private::CTaskResultInvocable<F, const T&>
	auto Then(const TTaskHandle<T>& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<std::decay_t<F>&, const T&>;
		return Private::LaunchContinuationResult<U>(Predecessor.GetTaskHandle(), Name,
			[Predecessor, Function = std::forward<F>(Function)]() mutable -> U {
				auto Result = Predecessor.GetResultShared();
				check(Result);
				return std::invoke(Function, *Result);
			}, Options, ETaskDependencyKind::Success);
	}

	template<typename F>
	requires Private::CTaskResultInvocable<F>
	auto Then(const FTaskHandle& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<std::decay_t<F>&>;
		return Private::LaunchContinuationResult<U>(Predecessor, Name,
			[Function = std::forward<F>(Function)]() mutable -> U { return std::invoke(Function); },
			Options, ETaskDependencyKind::Success);
	}

	template<typename T, typename F>
	requires Private::CTaskResultInvocable<F, FTaskOutcome<T>>
	auto ThenOutcome(const TTaskHandle<T>& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<std::decay_t<F>&, FTaskOutcome<T>>;
		return Private::LaunchContinuationResult<U>(Predecessor.GetTaskHandle(), Name,
			[Predecessor, Function = std::forward<F>(Function)]() mutable -> U {
				const FTaskDiagnostics Diagnostics = Predecessor.GetDiagnostics();
				return std::invoke(Function, FTaskOutcome<T>{Predecessor.GetTaskHandle(), Predecessor.GetResultShared(),
					Diagnostics.Diagnostic, Diagnostics.State, Diagnostics.TerminalReason});
			}, Options, ETaskDependencyKind::Completion);
	}

	template<typename F>
	requires Private::CTaskResultInvocable<F, FTaskOutcome<void>>
	auto ThenOutcome(const FTaskHandle& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<std::decay_t<F>&, FTaskOutcome<void>>;
		return Private::LaunchContinuationResult<U>(Predecessor, Name,
			[Predecessor, Function = std::forward<F>(Function)]() mutable -> U {
				const FTaskDiagnostics Diagnostics = Predecessor.GetDiagnostics();
				return std::invoke(Function, FTaskOutcome<void>{Predecessor, Diagnostics.Diagnostic,
					Diagnostics.State, Diagnostics.TerminalReason});
			}, Options, ETaskDependencyKind::Completion);
	}

	template<typename... Ts, typename F>
	requires (sizeof...(Ts) > 0
		&& (... && !std::is_void_v<Ts>)
		&& Private::CTaskResultInvocable<F, const Ts&...>)
	auto WhenAll(
		const std::tuple<TTaskHandle<Ts>...>& Predecessors,
		const char* Name,
		F&& Function,
		const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<std::decay_t<F>&, const Ts&...>;
		std::vector<FTaskHandle> PrerequisiteStorage;
		const FTaskContinuationOptions AdjustedOptions =
			Private::MakeFanInContinuationOptions(Predecessors, Options, PrerequisiteStorage);
		return Private::LaunchContinuationResult<U>(std::get<0>(Predecessors).GetTaskHandle(), Name,
			[Predecessors, Function = std::forward<F>(Function)]() mutable -> U {
				auto Results = std::apply([](const auto&... Handle) {
					return std::make_tuple(Handle.GetResultShared()...);
				}, Predecessors);
				check(std::apply([](const auto&... Result) { return (... && static_cast<bool>(Result)); }, Results));
				return std::apply([&Function](const auto&... Result) -> U {
					return std::invoke(Function, *Result...);
				}, Results);
			}, AdjustedOptions, ETaskDependencyKind::Success);
	}

	template<typename... Ts, typename F>
	requires (sizeof...(Ts) > 0
		&& (... && !std::is_void_v<Ts>)
		&& Private::CTaskResultInvocable<F, TTaskAggregateOutcome<Ts...>>)
	auto WhenAllOutcome(
		const std::tuple<TTaskHandle<Ts>...>& Predecessors,
		const char* Name,
		F&& Function,
		const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<std::decay_t<F>&, TTaskAggregateOutcome<Ts...>>;
		std::vector<FTaskHandle> PrerequisiteStorage;
		const FTaskContinuationOptions AdjustedOptions =
			Private::MakeFanInContinuationOptions(Predecessors, Options, PrerequisiteStorage);
		return Private::LaunchContinuationResult<U>(std::get<0>(Predecessors).GetTaskHandle(), Name,
			[Predecessors, Function = std::forward<F>(Function)]() mutable -> U {
				return std::invoke(Function, Private::MakeTaskAggregateOutcome(Predecessors));
			}, AdjustedOptions, ETaskDependencyKind::Completion);
	}
} // namespace Durin
