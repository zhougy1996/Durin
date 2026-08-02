#pragma once

#include "CoreAPI.h"

#include "HAL/Platform.h"

namespace Durin
{
	using FTaskFunction = std::function<void()>;
	class FTaskCancellationToken;
	using FCancelableTaskFunction = std::function<void(const FTaskCancellationToken&)>;
	using FParallelForFunction = std::function<void(uint64)>;
	class FParallelForCancellationToken;
	using FCancelableParallelForFunction = std::function<void(uint64, const FParallelForCancellationToken&)>;

	class FTaskCancellationState;
	class FTaskScheduler;
	class FTaskStateData;
	struct FTaskLaunchOptions;
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

	// A copied, thread-safe view of one task's identity, relationships, timing, and outcome.
	struct FTaskDiagnostics
	{
		uint64 TaskId = 0;
		uint64 ParentTaskId = 0;
		std::vector<uint64> PrerequisiteTaskIds;
		uint64 EnqueueTimeNanoseconds = 0;
		uint64 StartTimeNanoseconds = 0;
		uint64 FinishTimeNanoseconds = 0;
		uint32 ExecutingThreadId = 0;
		std::string DebugName;
		std::string ExecutingThreadName;
		std::string Diagnostic;
		ETaskState State = ETaskState::Invalid;
	};

	// Aggregate counters and currently nonterminal nodes for one scheduler lifetime.
	struct FTaskSchedulerDiagnostics
	{
		uint32 WorkerCount = 0;
		uint32 QueueDepth = 0;
		uint32 ActiveWorkerCount = 0;
		uint64 CompletedTaskCount = 0;
		uint64 FailedTaskCount = 0;
		uint64 CanceledTaskCount = 0;
		uint64 RejectedTaskCount = 0;
		uint64 LongWaitCount = 0;
		uint64 NonterminalTaskCount = 0;
		uint64 RetainedTerminalHandleCount = 0;
		uint64 LastLongWaitTargetTaskId = 0;
		uint64 LastLongWaitElapsedNanoseconds = 0;
		std::string LastLongWaiterName;
		std::string LastLongWaitTargetName;
		ETaskState LastLongWaitTargetState = ETaskState::Invalid;
		bool bRunning = false;
		std::vector<FTaskDiagnostics> NonterminalTasks;
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
		friend CORE_API auto CancelTask(const FTaskHandle& Task) -> bool;
		friend CORE_API auto WaitTask(const FTaskHandle& Task) -> ETaskState;

		std::shared_ptr<FTaskStateData> State;
	};

	// Immutable launch-time relationships and optional shared cancellation.
	struct FTaskLaunchOptions
	{
		std::span<const FTaskHandle> Prerequisites;
		FTaskCancellationToken CancellationToken;
	};

	struct FParallelForOptions
	{
		// Conservative default remains serial until framework qualification selects a crossover.
		uint64 MinBatchSize = std::numeric_limits<uint64>::max();
		FTaskCancellationToken CancellationToken;
	};

	struct FParallelForResult
	{
		ETaskState State = ETaskState::Invalid;
		std::string Diagnostic;
		uint32 ChunkCount = 0;
	};

	// Starts the process-owned CPU scheduler. Engine lifecycle starts it once during PreInit.
	CORE_API auto InitializeTaskScheduler(uint32 InNumThreads = 0) -> bool;
	// Closes admission and either drains or discards all accepted work before returning.
	CORE_API auto ShutdownTaskScheduler(bool bWaitForQueuedWork = true) -> void;
	CORE_API auto IsTaskSchedulerRunning() -> bool;
	// Returns the live lifetime snapshot, or the final snapshot after shutdown.
	CORE_API auto GetTaskSchedulerDiagnostics() -> FTaskSchedulerDiagnostics;

	CORE_API auto LaunchTask(const char* Name, FTaskFunction&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle;
	CORE_API auto LaunchCancelableTask(const char* Name, FCancelableTaskFunction&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle;
	// Returns false only for an invalid or already-terminal task.
	CORE_API auto CancelTask(const FTaskHandle& Task) -> bool;
	CORE_API auto WaitTask(const FTaskHandle& Task) -> ETaskState;
	// Returns one observed outcome for each input handle, including Invalid.
	CORE_API auto WaitAll(std::span<const FTaskHandle> Tasks) -> std::vector<ETaskState>;

	// Executes [0, Num) in bounded contiguous chunks and includes the calling thread.
	CORE_API auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;
	CORE_API auto ParallelForCancelable(const char* Name, uint64 Num, FCancelableParallelForFunction&& Function, const FParallelForOptions& Options = {}) -> FParallelForResult;
} // namespace Durin
