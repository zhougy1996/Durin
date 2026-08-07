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
	class FTaskHandle;
	class FTaskScheduler;
	class FTaskStateData;
	template<typename T>
	class TTaskResultState;
	struct FTaskLaunchOptions;
	struct FTaskContinuationOptions;
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

	enum class ETaskTarget : uint8
	{
		AnyWorker,
		GameThreadDeferred,
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
		Superseded,
		StaleGeneration,
		CallbackFailure,
		ShutdownCanceled,
	};

	namespace Private
	{
		struct FTaskHandleFactory;
		CORE_API auto LaunchCancelableTaskWithCompletion(const char* Name, FCancelableTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskLaunchOptions& Options) -> FTaskHandle;
		CORE_API auto LaunchContinuationTask(const FTaskHandle& Predecessor, const char* Name, FCancelableTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskContinuationOptions& Options, ETaskDependencyKind DependencyKind) -> FTaskHandle;
	}

	// A copied, thread-safe view of one task's identity, relationships, timing, and outcome.
	struct FTaskDiagnostics
	{
		uint64 TaskId = 0;
		uint64 ParentTaskId = 0;
		std::vector<uint64> PrerequisiteTaskIds;
		std::vector<ETaskDependencyKind> PrerequisiteDependencyKinds;
		uint64 DirectBlockingTaskId = 0;
		uint64 EnqueueTimeNanoseconds = 0;
		uint64 StartTimeNanoseconds = 0;
		uint64 FinishTimeNanoseconds = 0;
		uint32 ExecutingThreadId = 0;
		std::string DebugName;
		std::string ExecutingThreadName;
		std::string Diagnostic;
		ETaskState State = ETaskState::Invalid;
		ETaskTarget Target = ETaskTarget::AnyWorker;
		ETaskTerminalReason TerminalReason = ETaskTerminalReason::None;
		bool bHasResultStorage = false;
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
		uint64 RetainedTerminalResultCount = 0;
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
		friend CORE_API auto Private::LaunchCancelableTaskWithCompletion(const char* Name, FCancelableTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskLaunchOptions& Options) -> FTaskHandle;
		friend CORE_API auto Private::LaunchContinuationTask(const FTaskHandle& Predecessor, const char* Name, FCancelableTaskFunction&& Function, std::function<void(ETaskState)>&& CompletionFunction, const FTaskContinuationOptions& Options, ETaskDependencyKind DependencyKind) -> FTaskHandle;
		friend CORE_API auto CancelTask(const FTaskHandle& Task) -> bool;
		friend CORE_API auto WaitTask(const FTaskHandle& Task) -> ETaskState;

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

	// Immutable launch-time relationships and optional shared cancellation.
	struct FTaskLaunchOptions
	{
		std::span<const FTaskHandle> Prerequisites;
		FTaskCancellationToken CancellationToken;
	};

	struct FTaskContinuationOptions
	{
		std::span<const FTaskHandle> Prerequisites;
		FTaskCancellationToken CancellationToken;
		ETaskTarget Target = ETaskTarget::AnyWorker;
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
			std::lock_guard Lock(Mutex);
			if (State == ETaskState::Succeeded)
			{
				Published = std::move(Pending);
			}
			else
			{
				Pending.reset();
			}
		}

		auto GetPublished() const -> std::shared_ptr<const T>
		{
			std::lock_guard Lock(Mutex);
			return Published;
		}

	private:
		mutable std::mutex Mutex;
		std::shared_ptr<T> Pending;
		std::shared_ptr<const T> Published;
	};

	namespace Private
	{
		struct FTaskHandleFactory
		{
			template<typename T>
			static auto Make(FTaskHandle Task, std::shared_ptr<TTaskResultState<T>> ResultState) -> TTaskHandle<T>
			{
				return TTaskHandle<T>(std::move(Task), std::move(ResultState));
			}
		};

		template<typename T>
		auto MakeTypedTaskHandle(
			const char* Name,
			std::function<T(const FTaskCancellationToken&)>&& Function,
			const FTaskLaunchOptions& Options) -> TTaskHandle<T>
		{
			auto ResultState = std::make_shared<TTaskResultState<T>>();
			FTaskHandle Task = Private::LaunchCancelableTaskWithCompletion(
				Name,
				[Function = std::move(Function), ResultState](const FTaskCancellationToken& Token) mutable {
					ResultState->SetPending(Function(Token));
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
	auto Then(const TTaskHandle<T>& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<F, const T&>;
		static_assert(std::is_copy_constructible_v<std::decay_t<F>>,
			"Task continuation callables must be copy constructible in V1.");
		return Private::LaunchContinuationResult<U>(Predecessor.GetTaskHandle(), Name,
			[Predecessor, Function = std::forward<F>(Function)]() mutable -> U {
				auto Result = Predecessor.GetResultShared();
				check(Result);
				return Function(*Result);
			}, Options, ETaskDependencyKind::Success);
	}

	template<typename F>
	auto Then(const FTaskHandle& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<F>;
		static_assert(std::is_copy_constructible_v<std::decay_t<F>>,
			"Task continuation callables must be copy constructible in V1.");
		return Private::LaunchContinuationResult<U>(Predecessor, Name,
			[Function = std::forward<F>(Function)]() mutable -> U { return Function(); },
			Options, ETaskDependencyKind::Success);
	}

	template<typename T, typename F>
	auto ThenOutcome(const TTaskHandle<T>& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<F, FTaskOutcome<T>>;
		static_assert(std::is_copy_constructible_v<std::decay_t<F>>,
			"Task continuation callables must be copy constructible in V1.");
		return Private::LaunchContinuationResult<U>(Predecessor.GetTaskHandle(), Name,
			[Predecessor, Function = std::forward<F>(Function)]() mutable -> U {
				const FTaskDiagnostics Diagnostics = Predecessor.GetDiagnostics();
				return Function(FTaskOutcome<T>{Predecessor.GetTaskHandle(), Predecessor.GetResultShared(),
					Diagnostics.Diagnostic, Diagnostics.State, Diagnostics.TerminalReason});
			}, Options, ETaskDependencyKind::Completion);
	}

	template<typename F>
	auto ThenOutcome(const FTaskHandle& Predecessor, const char* Name, F&& Function, const FTaskContinuationOptions& Options = {})
	{
		using U = std::invoke_result_t<F, FTaskOutcome<void>>;
		static_assert(std::is_copy_constructible_v<std::decay_t<F>>,
			"Task continuation callables must be copy constructible in V1.");
		return Private::LaunchContinuationResult<U>(Predecessor, Name,
			[Predecessor, Function = std::forward<F>(Function)]() mutable -> U {
				const FTaskDiagnostics Diagnostics = Predecessor.GetDiagnostics();
				return Function(FTaskOutcome<void>{Predecessor, Diagnostics.Diagnostic,
					Diagnostics.State, Diagnostics.TerminalReason});
			}, Options, ETaskDependencyKind::Completion);
	}
} // namespace Durin
