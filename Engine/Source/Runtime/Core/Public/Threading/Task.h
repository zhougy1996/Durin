#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

#include "Templates/MoveOnlyFunction.h"

#include "CoreAPI.h"

#include "HAL/Platform.h"
#include "Threading/TaskError.h"

namespace Durin
{
	namespace Private
	{
		struct FTaskAttributionAccess;
		struct FTaskRuntimeAccess;
		struct FParallelForAccess;
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

	class FTaskCancellationToken;
	using FParallelForFunction = TMoveOnlyFunction<void(uint64)>;
	class FParallelForCancellationToken;
	using FCancelableParallelForFunction = TMoveOnlyFunction<void(uint64, const FParallelForCancellationToken&)>;

	class FTaskCancellationState;
	class FTaskGenerationState;
	namespace Tasks
	{
		class FPipe;
		namespace Private { class FTaskHandle; struct FTaskConcurrencyLimiterState; }
		// Public generic completion handle for any task or event.
		using FTask = Private::FTaskHandle;
	}
	class FTaskScheduler;
	class FTaskStateData;
	template<typename T>
	class TTaskResultState;
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
		TimedOut,
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

	enum class ETaskTerminalReason : uint8
	{
		None,
		DependencyFailed,
		CancellationRequested,
		DispatchRejected,
		Superseded,
		StaleGeneration,
		CallbackFailure,
		ShutdownCanceled,
	};

	namespace Tasks
	{
		// Framework failure is distinct from a successful value carrying a domain error.
		enum class ETaskFailureCode : uint8 { CallableException, DependencyBindingFailed, AbandonedSource, DependencyFailed };
		struct FTaskFailure
		{
			ETaskTerminalReason Reason = ETaskTerminalReason::CallbackFailure;
			std::optional<FTaskConstructionError> DependencyError;
			ETaskFailureCode Code = ETaskFailureCode::CallableException;
			uint64 RelatedTaskId = 0;
			size_t InputIndex = 0;
		};
	}

	namespace Private
	{
		CORE_API auto RetainForwardedTask(const Tasks::FTask& Owner, Tasks::FTask Inner) -> void;
		// Retire forwarding ownership iteratively, including reentrant node destruction.
		CORE_API auto ReleaseForwardedTask(Tasks::FTask Task) noexcept -> void;
		using FMoveOnlyTaskFunction = TMoveOnlyFunction<void(const FTaskCancellationToken&)>;

		using FTaskCompletionFunction = TMoveOnlyFunction<void(ETaskState)>;

		// Bound with the body and completion callback before node activation.
		struct FTaskResultStorage
		{
			std::shared_ptr<void> Storage;
			bool bHasValue = false;
		};

		// Valid construction returns a handle; contract and allocation failures are fatal.
		CORE_API auto LaunchCancelableTaskWithCompletion(const char* Name, FMoveOnlyTaskFunction&& Function, FTaskCompletionFunction&& CompletionFunction, const FTaskLaunchOptions& Options, uint64 AdditionalStorageBytes = 0, FTaskResultStorage Result = {}, const Tasks::FTask* PrimaryPrerequisite = nullptr) -> Tasks::FTask;
		CORE_API auto LaunchContinuationTask(const Tasks::FTask& Predecessor, const char* Name, FMoveOnlyTaskFunction&& Function, FTaskCompletionFunction&& CompletionFunction, const FTaskContinuationOptions& Options, uint64 AdditionalStorageBytes = 0, FTaskResultStorage Result = {}) -> Tasks::FTask;
		// Native-test seam for pausing after Close is claimed, before result publication.
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
		uint64 ParentTaskId = 0;
		std::vector<uint64> PrerequisiteTaskIds;
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
		uint64 EstimatedStorageBytes = 0;
		uint64 CoalescingOwnerDomain = 0;
		uint64 CoalescingWorkId = 0;
		uint64 CoalescingGeneration = 0;
		bool bHasResultStorage = false;
		uint16 AttributionOwnerId = 0;
		uint16 AttributionCategoryId = 0;
		std::string AttributionOwner;
		std::string AttributionCategory;
		uint64 ExecutionNanoseconds = 0;
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
		uint64 CancellationRequestedCount = 0;
		uint64 DispatchRejectedCount = 0;
		uint64 SupersededCount = 0;
		uint64 StaleGenerationCount = 0;
		uint64 CallbackFailureCount = 0;
		uint64 ShutdownCanceledCount = 0;
		uint64 CurrentWaitingCount = 0;
		uint64 CurrentQueuedCount = 0;
		uint64 CurrentRunningCount = 0;
		uint64 CurrentNonterminalCount = 0;
		uint64 ParallelForOperationCount = 0;
		uint64 CurrentPayloadBytes = 0;
		uint64 PeakPayloadBytes = 0;
		std::array<uint64, 32> QueueResidencyHistogram{};
		std::array<uint64, 32> ExecutionHistogram{};
		std::array<uint64, 32> PayloadBytesHistogram{};
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
		uint64 TaskWarningThreshold = 0;
		uint64 CurrentTrackedTaskCount = 0;
		uint64 PeakTrackedTaskCount = 0;
		uint32 QueueDepth = 0;
		uint32 BlockingIOWorkerCount = 0;
		uint32 BlockingIOQueueDepth = 0;
		uint32 BlockingIOTrackedTasks = 0;
		uint32 BlockingIOWarningThreshold = 0;
		uint32 ActiveWorkerCount = 0;
		uint64 CompletedTaskCount = 0;
		uint64 FailedTaskCount = 0;
		uint64 CanceledTaskCount = 0;
		uint64 RejectedTaskCount = 0;
		uint64 LongWaitCount = 0;
		uint64 NonterminalTaskCount = 0;
		uint64 RetainedTerminalHandleCount = 0;
		uint64 RetainedTerminalResultCount = 0;
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
		uint32 QueuedEntriesWarningThreshold = 1'024;
		uint64 QueuedBytesWarningThreshold = 8ull * 1'024ull * 1'024ull;
		uint64 EntryBytesWarningThreshold = 1ull * 1'024ull * 1'024ull;
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

	// Observes both ParallelFor group cancellation and caller-provided cancellation.
	class FParallelForCancellationToken
	{
	public:
		CORE_API auto IsCancellationRequested() const -> bool;

	private:
		FParallelForCancellationToken(FTaskCancellationToken InGroupToken, FTaskCancellationToken InExternalToken);

		friend struct Private::FParallelForAccess;

		FTaskCancellationToken GroupToken;
		FTaskCancellationToken ExternalToken;
	};

	CORE_API auto CancelTask(const Tasks::FTask& Task) -> bool;
	CORE_API auto WaitTask(const Tasks::FTask& Task) -> FTaskWaitResult;
	// Nonpositive seconds only poll. Positive seconds bound blocking/helping attempts,
	// never an executing callable. Empty handles complete with Invalid task state.
	CORE_API auto WaitTask(const Tasks::FTask& Task, double TimeoutSeconds) -> FTaskWaitResult;
	// Returns one wait result for each input handle, including invalid handles and rejected waits.
	CORE_API auto WaitAll(std::span<const Tasks::FTask> Tasks) -> std::vector<FTaskWaitResult>;
	CORE_API auto WaitAll(std::span<const Tasks::FTask> Tasks, double TimeoutSeconds) -> std::vector<FTaskWaitResult>;

	namespace Tasks::Detail { struct FTaskAccess; }
	namespace Tasks::Private
	{
		// Shares the execution node and its result lifetime without typed result access.
		class FTaskHandle
		{
		public:
			CORE_API FTaskHandle();
			FTaskHandle(const FTaskHandle&) = default;
			FTaskHandle(FTaskHandle&&) noexcept = default;
			auto operator=(const FTaskHandle&) -> FTaskHandle& = default;
			auto operator=(FTaskHandle&&) noexcept -> FTaskHandle& = default;

			CORE_API auto IsValid() const -> bool;
			CORE_API auto IsCompleted() const -> bool;
			// Cancellation is a cooperative request, independent of completion/outcome.
			CORE_API auto IsCancellationRequested() const -> bool;
			CORE_API auto GetState() const -> ETaskState;
			CORE_API auto GetDebugName() const -> const char*;
			CORE_API auto GetTaskId() const -> uint64;
			CORE_API auto GetDiagnostic() const -> std::string;
			CORE_API auto GetDiagnostics() const -> FTaskDiagnostics;
			auto Wait() const -> FTaskWaitResult { return Durin::WaitTask(*this); }
			auto Wait(double TimeoutSeconds) const -> FTaskWaitResult { return Durin::WaitTask(*this, TimeoutSeconds); }
			// Attempts this task and its dependencies without waiting for other threads.
			// An executed callable can run arbitrarily long; executor restrictions apply.
			CORE_API auto TryRetractAndExecute() const -> bool;

		protected:
			auto SetHandle(FTaskHandle InHandle) -> void { *this = std::move(InHandle); }

		private:
			explicit FTaskHandle(std::shared_ptr<FTaskStateData> InState);
			friend class Durin::FTaskScheduler;
			friend struct Durin::Private::FTaskRuntimeAccess;
			friend struct Durin::Tasks::Detail::FTaskAccess;
			friend CORE_API auto Durin::Private::LaunchCancelableTaskWithCompletion(const char* Name, Durin::Private::FMoveOnlyTaskFunction&& Function, Durin::Private::FTaskCompletionFunction&& CompletionFunction, const FTaskLaunchOptions& Options, uint64 AdditionalStorageBytes, Durin::Private::FTaskResultStorage Result, const Tasks::FTask* PrimaryPrerequisite) -> FTaskHandle;
			friend CORE_API auto Durin::Private::LaunchContinuationTask(const FTaskHandle& Predecessor, const char* Name, Durin::Private::FMoveOnlyTaskFunction&& Function, Durin::Private::FTaskCompletionFunction&& CompletionFunction, const FTaskContinuationOptions& Options, uint64 AdditionalStorageBytes, Durin::Private::FTaskResultStorage Result) -> FTaskHandle;
			friend CORE_API auto Durin::CancelTask(const FTaskHandle& Task) -> bool;
			friend CORE_API auto Durin::WaitTask(const FTaskHandle& Task) -> FTaskWaitResult;
			friend CORE_API auto Durin::WaitTask(const FTaskHandle& Task, double TimeoutSeconds) -> FTaskWaitResult;
			friend CORE_API auto Durin::WaitAll(std::span<const FTaskHandle> Tasks, double TimeoutSeconds) -> std::vector<FTaskWaitResult>;

			std::shared_ptr<FTaskStateData> State;
		};
	}

	namespace Private
	{
		// Binding this preallocated internal hook never admits another scheduled node.
		struct FTaskTerminalHook
		{
			FTaskCompletionFunction Function;
			std::shared_ptr<FTaskTerminalHook> Next;
		};
		struct FTaskRuntimeAccess
		{
			// Internal reads are also valid from terminal hooks before public completion.
			CORE_API static auto GetFailure(const Tasks::FTask& Task) -> Tasks::FTaskFailure;
			CORE_API static auto SetFailure(const Tasks::FTask& Task, Tasks::FTaskFailure Failure) -> void;
			CORE_API static auto SetCurrentFailure(Tasks::FTaskFailure Failure) -> void;
			// Detached completion reuses task storage without admitting a scheduled body.
			CORE_API static auto CreateDetachedCompletion(FTaskCompletionFunction&& Completion, FTaskResultStorage Result) -> Tasks::FTask;
			CORE_API static auto CreateTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement) -> Tasks::FTask;
			CORE_API static auto AddTaskEventPrerequisites(const Tasks::FTask& Event, std::span<const Tasks::FTask> Prerequisites) -> std::optional<Tasks::FTaskConstructionError>;
			CORE_API static auto TriggerTaskEvent(const Tasks::FTask& Event) -> void;
			CORE_API static auto PinLifetime(const Tasks::FTask& Task) -> std::shared_ptr<void>;
			CORE_API static auto RetainLifetime(const Tasks::FTask& Task, std::shared_ptr<void> Lifetime) -> void;
			CORE_API static auto GetCurrentTaskId() -> uint64;
			CORE_API static auto IsExecutingIndependentCPU() -> bool;
			CORE_API static auto IsCancellationRequested(const Tasks::FTask& Task) -> bool;
			CORE_API static auto SetCurrentCanceledOutcome() -> void;
			CORE_API static auto GetAttribution(const Tasks::FTask& Task) -> FTaskAttribution;
			CORE_API static auto BindTerminal(const Tasks::FTask& Task, std::shared_ptr<FTaskTerminalHook> Hook) -> void;
			CORE_API static auto CompleteExternal(const Tasks::FTask& Task, ETaskState State) -> void;
			CORE_API static auto AddCompletionDependency(const Tasks::FTask& Parent, const Tasks::FTask& Child) -> std::optional<Tasks::FTaskConstructionError>;
			CORE_API static auto AddNested(const Tasks::FTask& Child) -> std::optional<Tasks::FTaskConstructionError>;
			CORE_API static auto BindDynamicDependency(const Tasks::FTask& Task, const Tasks::FTask& Inner) -> std::optional<Tasks::FTaskConstructionError>;
		};
	}

	// Immutable launch-time relationships and optional shared cancellation.
	struct FTaskLaunchOptions
	{
		// Internal ready-time serialization; owned by the caller until completion.
		Tasks::FPipe* Pipe = nullptr;
		std::shared_ptr<Tasks::Private::FTaskConcurrencyLimiterState> ConcurrencyLimiter;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		// Order execution after every nonempty prerequisite completes, regardless of outcome.
		std::span<const Tasks::FTask> Prerequisites;
		FTaskCancellationToken CancellationToken;
		FTaskAttribution Attribution;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		uint64 EstimatedPayloadBytes = 0;
		// Internal completion sources stay counted until producer acknowledgement.
		bool bExternalCompletion = false;
		bool bUnknownExecutionRequirement = true;
		// Independent CPU leaves may be joined by RenderingThread. No task
		// dependencies, external completion, child submission or blocking waits.
		bool bIndependentCPU = false;
	};

	struct FTaskContinuationOptions
	{
		std::span<const Tasks::FTask> Prerequisites;
		FTaskCancellationToken CancellationToken;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
		uint64 EstimatedPayloadBytes = 0;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskPriority Priority = ETaskPriority::Normal;
		FTaskAttribution Attribution;
	};

	struct FParallelForOptions
	{
		// Conservative default remains serial until framework qualification selects a crossover.
		uint64 MinBatchSize = std::numeric_limits<uint64>::max();
		FTaskCancellationToken CancellationToken;
		FTaskAttribution Attribution;
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
		uint64 TaskWarningThreshold = 16'384;
		uint32 NumBlockingIOThreads = 2;
		uint32 BlockingIOWarningThreshold = 128;
	};

	// Starts the process-owned CPU scheduler. Engine lifecycle starts it once during PreInit.
	CORE_API auto InitializeTaskScheduler(uint32 InNumThreads = 0) -> bool;
	CORE_API auto InitializeTaskScheduler(const FTaskSchedulerConfig& Config) -> bool;
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


	// Executes [0, Num) in bounded contiguous chunks and includes the calling thread.
	// Chunks invoke the same owned callable concurrently; captures must support concurrent access.
	CORE_API auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;
	CORE_API auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;

	template<typename T>
	class TTaskResultState : public std::enable_shared_from_this<TTaskResultState<T>>
	{
	public:
		~TTaskResultState()
		{
			Discard();
		}

		auto SetPending(T&& InValue) -> void
		{
			// A producer owns construction until completion publication. Construct
			// outside the mutex: a user move constructor can reenter task APIs.
			try
			{
				std::construct_at(reinterpret_cast<T*>(ValueStorage), std::move(InValue));
			}
			catch (const std::bad_alloc&) { requiref(false, "Task result allocation failed."); std::terminate(); }
			std::lock_guard Lock(Mutex);
			check(!bHasValue && !bCompleted);
			bHasValue = true;
		}

		auto Complete(ETaskState State) -> void
		{
			T* DetachedValue = nullptr;
			{
				std::lock_guard Lock(Mutex);
				bCompleted = true;
				if (State == ETaskState::Succeeded && (bHasValue || Forwarded))
				{
					bPublished = true;
				}
				else
				{
					if (bHasValue) DetachedValue = Value();
					bHasValue = false;
				}
			}
			if (DetachedValue) std::destroy_at(DetachedValue);
		}

		// Shared access never consumes or invalidates another task handle.
		auto PeekPublished() const -> T*
		{
			std::lock_guard Lock(Mutex);
			if (!bPublished) return nullptr;
			return Forwarded ? Forwarded : bHasValue ? Value() : nullptr;
		}

		auto ForwardResult(std::shared_ptr<TTaskResultState> Other, Tasks::FTask Lifetime) -> void
		{
			// The source has published before its terminal hooks run. Cache the final
			// address, but retain the immediate node to preserve all owner lifetimes.
			T* Published = Other->PeekPublished();
			require(Published && Lifetime.IsValid());
			std::lock_guard Lock(Mutex);
			require(!bCompleted && !bHasValue && !Forwarded);
			Forwarded = Published;
			ForwardedTask = std::move(Lifetime);
		}

		auto Discard() -> void
		{
			T* DetachedValue = nullptr;
			Tasks::FTask DetachedTask;
			{
				std::lock_guard Lock(Mutex);
				bPublished = false;
				if (bHasValue) DetachedValue = Value();
				bHasValue = false;
				Forwarded = nullptr;
				DetachedTask = std::move(ForwardedTask);
			}
			Private::ReleaseForwardedTask(std::move(DetachedTask));
			if (DetachedValue) std::destroy_at(DetachedValue);
		}

	private:

		mutable std::mutex Mutex;
		auto Value() const -> T* { return std::launder(reinterpret_cast<T*>(ValueStorage)); }
		alignas(T) mutable std::byte ValueStorage[sizeof(T)];
		bool bHasValue = false;
		bool bCompleted = false;
		bool bPublished = false;
		T* Forwarded = nullptr;
		Tasks::FTask ForwardedTask;
	};

} // namespace Durin
