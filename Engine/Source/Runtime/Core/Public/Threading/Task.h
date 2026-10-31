#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

#include "Threading/TaskRuntime.h"

namespace Durin::Tasks
{
	namespace Detail
	{
		// Construction failures are programming/lifetime violations or allocation failure.
		// This is never used to turn ordinary scheduler saturation into an assertion.
		[[noreturn]] CORE_API auto FailConstruction(FTaskConstructionError Error) -> void;
	}

	// Executor choice is shared by roots and every composition edge.
	enum class ETaskExecutor : uint8 { Worker, BlockingIO, GameThreadDeferred };

	namespace Detail
	{
		// Result access requires both a supported wait and successful execution in every build.
		CORE_API auto WaitForSuccess(const FTask& Handle) -> void;
	}
	namespace Detail
	{
		template<typename T> using TResultState = std::conditional_t<std::is_void_v<T>, std::nullptr_t, std::shared_ptr<TTaskResultState<T>>>;
	}

	// Execution and owner identity; scheduler storage accounting is internal.
	struct FTaskExecutionOptions
	{
		const char* DebugName = "Task";
		ETaskPriority Priority = ETaskPriority::Normal;
		FTaskCancellationToken Cancellation;
		FTaskAttribution Attribution;
		// Additional ordering dependencies, never implicit success conditions.
		std::span<const FTask> Prerequisites;
	};

	enum class EParallelForPolicy : uint8 { Auto, Serial, ExplicitBatch };
	struct FParallelForPolicyOptions
	{
		EParallelForPolicy Policy = EParallelForPolicy::Auto;
		uint64 BatchSize = 0;
		FTaskCancellationToken Cancellation;
		FTaskAttribution Attribution;
	};
	CORE_API auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function,
		const FParallelForPolicyOptions& Options = {}) -> FParallelForResult;

	CORE_API auto Wait(const FTask& Task) -> FTaskWaitResult;
	CORE_API auto Wait(const FTask& Task, double TimeoutSeconds) -> FTaskWaitResult;
	CORE_API auto Cancel(const FTask& Task) -> bool;

	// Completion dependency only: does not forward cancellation or failure to either task.
	// Must be called from a running task body; its worker is free after the body returns.
	CORE_API auto AddNested(const FTask& Task) -> void;

	namespace Detail
	{
		template<typename T> concept CTaskDependency = std::derived_from<std::remove_cvref_t<T>, FTask>;
		template<CTaskDependency T> auto DependencyHandle(const T& Task) -> FTask
		{ return Task; }

		template<typename R> concept CTaskDependencyCollection = std::ranges::input_range<const R>
			&& CTaskDependency<std::ranges::range_reference_t<const R>>;

		// Dependency collection shares nodes without mutating their results.
		template<CTaskDependencyCollection R>
		auto CollectDependencies(const R& Inputs, std::span<const FTask> Existing = {})
			-> std::vector<FTask>
		{
			try
			{
				std::vector<FTask> Handles;
				if constexpr (std::ranges::sized_range<const R>) Handles.reserve(Existing.size() + std::ranges::size(Inputs));
				Handles.insert(Handles.end(), Existing.begin(), Existing.end());
				for (const auto& Input : Inputs) Handles.push_back(DependencyHandle(Input));
				return Handles;
			}
			catch (const std::bad_alloc&) { FailConstruction({ETaskConstructionErrorCode::AllocationFailure}); }
		}
	}

	// One outcome per input, in input order; does not consume task results.
	template<Detail::CTaskDependencyCollection R>
	auto Wait(const R& Inputs) -> std::vector<FTaskWaitResult>
	{ return WaitAll(Detail::CollectDependencies(Inputs)); }
	template<Detail::CTaskDependencyCollection R>
	auto Wait(const R& Inputs, double TimeoutSeconds) -> std::vector<FTaskWaitResult>
	{ return WaitAll(Detail::CollectDependencies(Inputs), TimeoutSeconds); }

	// Explicit one-shot signal, optionally joined with other completion dependencies.
	// Scheduler-independent: creation and signaling require no running scheduler.
	// Copies share one node, which must complete before its final reference is released.
	class FTaskEvent : public Private::FTaskHandle
	{
	public:
		// Set false only when signaling never needs GameThread or unknown external progress.
		CORE_API explicit FTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement = true);
		CORE_API auto AddPrerequisites(std::span<const FTask> Prerequisites) -> void;
		auto AddPrerequisites(const FTask& Prerequisite) -> void { AddPrerequisites(std::span(&Prerequisite, 1)); }
		template<Detail::CTaskDependencyCollection R>
		auto AddPrerequisites(const R& Inputs) -> void
		{
			const auto Handles = Detail::CollectDependencies(Inputs);
			AddPrerequisites(std::span<const FTask>(Handles));
		}
		CORE_API auto Trigger() -> void;
	};

	// Owns dependency handles, retaining their nodes and results. The returned
	// collection can be retained independently of the task/event facades.
	template<Detail::CTaskDependency... T>
	auto Prerequisites(const T&... Tasks) -> std::array<FTask, sizeof...(T)>
	{ return {Detail::DependencyHandle(Tasks)...}; }


	namespace Detail { struct FTaskAccess; }
	// Dropping observation never cancels the producer. An in-flight callback may finish.
	class FTaskSubscription
	{
	public:
		FTaskSubscription() = default;
		FTaskSubscription(FTaskSubscription&&) noexcept = default;
		auto operator=(FTaskSubscription&&) noexcept -> FTaskSubscription& = default;
		FTaskSubscription(const FTaskSubscription&) = delete;
		auto operator=(const FTaskSubscription&) -> FTaskSubscription& = delete;
		auto Reset() -> void { Callback.reset(); }
	private:
		friend CORE_API auto SubscribeTask(const FTask&, std::function<void(ETaskState)>) -> FTaskSubscription;
		std::shared_ptr<std::function<void(ETaskState)>> Callback;
	};

	CORE_API auto SubscribeTask(const FTask& Task, std::function<void(ETaskState)> Callback) -> FTaskSubscription;

	// Copyable handles share the task node and its result. Erased FTask handles
	// retain the same result lifetime without exposing a typed accessor.
	template<typename T>
	class TTask : public Private::FTaskHandle
	{
	public:
		TTask() = default;
		TTask(const TTask&) = default;
		TTask(TTask&&) noexcept = default;
		auto operator=(const TTask&) -> TTask& = default;
		auto operator=(TTask&&) noexcept -> TTask& = default;
		// Moving from this reference is an explicit mutation visible to all aliases.
		// Callers synchronize mutations of a shared result.
		auto GetResult() const -> std::add_lvalue_reference_t<T> requires (!std::is_void_v<T>)
		{
			Detail::WaitForSuccess(*this);
			return *Storage->PeekPublished();
		}
		auto GetResultShared() const -> std::shared_ptr<const T> requires (!std::is_void_v<T>)
		{
			if (!IsCompleted() || GetState() != ETaskState::Succeeded) return {};
			return std::shared_ptr<const T>(Durin::Private::FTaskRuntimeAccess::PinLifetime(*this), Storage->PeekPublished());
		}
		auto Subscribe(std::function<void(ETaskState)> Callback) const -> FTaskSubscription
		{ return SubscribeTask(*this, std::move(Callback)); }
	private:
		friend struct Detail::FTaskAccess;
		std::conditional_t<std::is_void_v<T>, std::nullptr_t, TTaskResultState<T>*> Storage = nullptr;
	};

	namespace Detail
	{
		struct FTaskAccess
		{
			template<typename T> static auto Make(FTask Handle, Detail::TResultState<T> State) -> TTask<T>
			{
				TTask<T> Task;
				Task.SetHandle(std::move(Handle));
				if constexpr (!std::is_void_v<T>) Task.Storage = State.get();
				return Task;
			}

		};
		template<typename T> struct TIsTask : std::false_type {};
		template<typename T> struct TIsTask<TTask<T>> : std::true_type {};
		template<typename F, bool = std::is_invocable_v<F&, const FTaskCancellationToken&>> struct TTokenResult { using Type = std::invoke_result_t<F&>; };
		template<typename F> struct TTokenResult<F, true> { using Type = std::invoke_result_t<F&, const FTaskCancellationToken&>; };

		inline auto Target(ETaskExecutor Executor) -> ETaskTarget
		{
			if (Executor == ETaskExecutor::Worker) return ETaskTarget::AnyWorker;
			if (Executor == ETaskExecutor::BlockingIO) return ETaskTarget::BlockingIO;
			if (Executor == ETaskExecutor::GameThreadDeferred) return ETaskTarget::GameThreadDeferred;
			return static_cast<ETaskTarget>(255);
		}
		template<typename T>
		auto MakeResultState() -> Detail::TResultState<T>
		{
			if constexpr (std::is_void_v<T>) return nullptr;
			else return std::make_shared<TTaskResultState<T>>();
		}

		template<typename T>
		auto MakeResultCompletion(const TResultState<T>& Result) -> Durin::Private::FTaskCompletionFunction
		{
			if constexpr (std::is_void_v<T>) return {};
			else return [Result](ETaskState Terminal) { Result->Complete(Terminal); };
		}

		// Void tasks have neither a result allocation nor a result publication callback.
		template<typename T, typename Body>
		constexpr auto ResultStorageBytes() -> uint64
		{
			if constexpr (std::is_void_v<T>) return sizeof(Body);
			else return sizeof(Body) + sizeof(TResultState<T>) + sizeof(TTaskResultState<T>);
		}
		template<typename T, typename Body>
		auto LaunchWithStorage(const char* Name, const TResultState<T>& Result,
			Body&& Function, const FTaskLaunchOptions& Options)
		{
			return Durin::Private::LaunchCancelableTaskWithCompletion(Name, std::forward<Body>(Function),
				MakeResultCompletion<T>(Result), Options, ResultStorageBytes<T, std::decay_t<Body>>(), {Result, !std::is_void_v<T>});
		}

	}

	namespace Detail
	{
	template<typename F, typename T = typename Detail::TTokenResult<std::decay_t<F>>::Type>
	requires (!std::is_reference_v<T> && !Detail::TIsTask<T>::value)
	auto LaunchTaskImpl(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function, bool bIndependentCPU = false, FPipe* Pipe = nullptr)
		-> TTask<T>
	{
		try
		{
			auto State = Detail::MakeResultState<T>();
			FTaskLaunchOptions Launch;
			Launch.Pipe = Pipe;
			Launch.bIndependentCPU = bIndependentCPU;
			Launch.Attribution = Options.Attribution;
			Launch.CancellationToken = Options.Cancellation;
			Launch.Target = Detail::Target(Executor);
			Launch.Priority = Options.Priority;
			Launch.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
			Launch.Prerequisites = Options.Prerequisites;
			auto Handle = Detail::LaunchWithStorage<T>(Options.DebugName, State,
				[State, Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
					auto Invoke = [&]() -> T {
						if constexpr (std::is_invocable_v<std::decay_t<F>&, const FTaskCancellationToken&>) return std::invoke(Function, Token);
						else return std::invoke(Function);
					};
					if constexpr (std::is_void_v<T>) Invoke();
					else State->SetPending(Invoke());
				}, Launch);
			return Detail::FTaskAccess::Make<T>(std::move(Handle), std::move(State));
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskConstructionErrorCode::AllocationFailure}); }
	}

	}

	// Preserve the executing parent identity for nested work, including blocking I/O.
	template<typename F>
	auto LaunchTask(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
	{
		return Detail::LaunchTaskImpl(Executor, Options, std::forward<F>(Function));
	}

	// Worker root in the scheduler lifetime. Owners stop their own submission before shutdown.
	template<typename F>
	auto LaunchTask(const char* Name, F&& Function, FTaskExecutionOptions Options = {})
	{
		Options.DebugName = Name;
		return Detail::LaunchTaskImpl(ETaskExecutor::Worker, Options, std::forward<F>(Function));
	}

	namespace Detail
	{
		template<CTaskDependencyCollection R, typename F>
		auto WithPrerequisites(FTaskExecutionOptions Options, const R& Inputs, F&& Launch)
		{
			const auto Handles = CollectDependencies(Inputs, Options.Prerequisites);
			Options.Prerequisites = Handles;
			return Launch(Options);
		}
	}

	template<typename F, Detail::CTaskDependencyCollection R>
	auto LaunchTask(ETaskExecutor Executor, const FTaskExecutionOptions& Options,
		F&& Function, const R& Inputs)
	{
		return Detail::WithPrerequisites(Options, Inputs, [&](const auto& BoundOptions) {
			return LaunchTask(Executor, BoundOptions, std::forward<F>(Function));
		});
	}
	template<typename F, Detail::CTaskDependency T>
	auto LaunchTask(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function, const T& Input)
	{ return LaunchTask(Executor, Options, std::forward<F>(Function), Prerequisites(Input)); }

	template<typename F, Detail::CTaskDependencyCollection R>
	auto LaunchTask(const char* Name, F&& Function, const R& Inputs,
		FTaskExecutionOptions Options = {})
	{
		return Detail::WithPrerequisites(Options, Inputs, [&](const auto& BoundOptions) {
			return LaunchTask(Name, std::forward<F>(Function), BoundOptions);
		});
	}
	template<typename F, Detail::CTaskDependency T>
	auto LaunchTask(const char* Name, F&& Function, const T& Input, FTaskExecutionOptions Options = {})
	{ return LaunchTask(Name, std::forward<F>(Function), Prerequisites(Input), Options); }

	// Declares a CPU-only leaf: the callable must not depend on owner-thread,
	// RHI, I/O, or external progress. The scheduler enforces task-level leaf
	// restrictions; the caller owns the callable's ordinary C++ side effects.
	template<typename F>
	auto LaunchIndependentTask(const char* Name, F&& Function, FTaskExecutionOptions Options = {})
	{
		Options.DebugName = Name;
		return Detail::LaunchTaskImpl(ETaskExecutor::Worker,
			Options, std::forward<F>(Function), true);
	}

}
