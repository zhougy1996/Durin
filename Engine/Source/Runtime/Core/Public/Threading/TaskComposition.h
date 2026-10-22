#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

#include "Threading/Task.h"

namespace Durin::Tasks
{
	namespace Detail
	{
		struct FCompletionSourceAccess;
		// Construction failures are programming/lifetime violations or allocation failure.
		// This is never used to turn ordinary scheduler saturation into an assertion.
		[[noreturn]] CORE_API auto FailConstruction(FTaskAdmissionError Error) -> void;
	}

	// Executor choice is shared by roots and every composition edge.
	enum class ETaskExecutor : uint8 { Worker, BlockingIO, GameThreadDeferred };

	// Framework failure is distinct from a successful value carrying a domain error.
	enum class ETaskFailureCode : uint8 { CallableException, DependencyBindingFailed, AbandonedSource, DependencyFailed };
	struct FTaskFailure
	{
		ETaskTerminalReason Reason = ETaskTerminalReason::CallbackFailure;
		std::optional<FTaskAdmissionError> DependencyError;
		ETaskFailureCode Code = ETaskFailureCode::CallableException;
		uint64 RelatedTaskId = 0;
		size_t InputIndex = 0;
	};

	namespace Detail
	{
		// Result access requires both a supported wait and successful execution in every build.
		CORE_API auto WaitForSuccess(const FTask& Handle) -> void;
		// Requires failure; absent stored failure uses the native terminal reason.
		CORE_API auto ReadFailure(const FTask& Handle, const FTaskFailure* Failure) -> FTaskFailure;
	}
	template<typename T> using TTaskValue = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

	// Execution and owner identity; scheduler storage accounting is internal.
	struct FTaskExecutionOptions
	{
		const char* DebugName = "Task";
		ETaskPriority Priority = ETaskPriority::Normal;
		FTaskCancellationToken Cancellation;
		FTaskAttribution Attribution;
		// Additional ordering dependencies, never implicit success conditions.
		std::span<const FTask> Prerequisites;
		FTaskGenerationToken GenerationToken;
		std::optional<FTaskCoalescingKey> CoalescingKey;
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
				std::vector<FTask> Handles(Existing.begin(), Existing.end());
				for (const auto& Input : Inputs) Handles.push_back(DependencyHandle(Input));
				return Handles;
			}
			catch (const std::bad_alloc&) { FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
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
	// Core-owned data and notification code can safely outlive a retired host observer.
	class FTaskReadiness
	{
	public:
		CORE_API FTaskReadiness();
		auto IsReady() const -> bool { return State->load(std::memory_order_acquire); }
	private:
		std::shared_ptr<std::atomic<bool>> State;
		friend CORE_API auto ObserveTaskReady(const FTask&, const FTaskReadiness&) -> FTaskSubscription;
	};
	CORE_API auto ObserveTaskReady(const FTask& Task, const FTaskReadiness& Readiness) -> FTaskSubscription;

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
		auto GetFailure() const -> FTaskFailure { return Detail::ReadFailure(*this, Failure.get()); }
		// Moving from this reference is an explicit mutation visible to all aliases.
		// Callers synchronize mutations of a shared result.
		auto GetResult() const -> std::conditional_t<std::is_void_v<T>, void, TTaskValue<T>&>
		{
			Detail::WaitForSuccess(*this);
			if constexpr (!std::is_void_v<T>) return *Storage->PeekPublished();
		}
		auto GetResultShared() const -> std::shared_ptr<const TTaskValue<T>>
		{
			if (!IsCompleted() || GetState() != ETaskState::Succeeded) return {};
			return std::shared_ptr<const TTaskValue<T>>(Durin::Private::FTaskRuntimeAccess::PinLifetime(*this), Storage->PeekPublished());
		}
		auto Subscribe(std::function<void(ETaskState)> Callback) const -> FTaskSubscription
		{ return SubscribeTask(*this, std::move(Callback)); }
		auto ObserveReady(const FTaskReadiness& Readiness) const -> FTaskSubscription
		{ return ObserveTaskReady(*this, Readiness); }
		auto WithLifetime(std::shared_ptr<void> Lifetime) const -> TTask
		{
			Durin::Private::FTaskRuntimeAccess::RetainLifetime(*this, std::move(Lifetime));
			return *this;
		}
	private:
		friend struct Detail::FTaskAccess;
		TTaskResultState<TTaskValue<T>>* Storage = nullptr;
		std::shared_ptr<FTaskFailure> Failure;
	};

	namespace Detail
	{
		struct FPropagationFailure : std::exception
		{
			auto what() const noexcept -> const char* override { return "Task prerequisite failed."; }
		};
		struct FTaskAccess
		{
			template<typename T> static auto GetResultState(const TTask<T>& Task)
			{ require(Task.IsValid() && Task.Storage); return Task.Storage->shared_from_this(); }
			template<typename T> static auto Failure(const TTask<T>& Task) -> const auto& { return Task.Failure; }
			template<typename T> static auto Make(FTask Handle, std::shared_ptr<TTaskResultState<TTaskValue<T>>> State, std::shared_ptr<FTaskFailure> Failure = {}) -> TTask<T>
			{
				TTask<T> Task;
				Task.Failure = std::move(Failure);
				Task.SetHandle(std::move(Handle)); Task.Storage = State.get();
				return Task;
			}

		};
		template<typename T> struct TIsTask : std::false_type {};
		template<typename T> struct TIsTask<TTask<T>> : std::true_type {};
		template<typename T> struct TIsTask<TTaskAdmission<T>> : std::true_type {};
		template<typename F, bool = std::is_invocable_v<F&, const FTaskCancellationToken&>> struct TTokenResult { using Type = std::invoke_result_t<F&>; };
		template<typename F> struct TTokenResult<F, true> { using Type = std::invoke_result_t<F&, const FTaskCancellationToken&>; };
		template<typename T, typename F, bool> struct TThenValueResult;
		template<typename T, typename F> struct TThenValueResult<T, F, false> { using Type = std::invoke_result_t<F&, T&>; };
		template<typename T, typename F> struct TThenValueResult<T, F, true> { using Type = std::invoke_result_t<F&, const FTaskCancellationToken&, T&>; };
		template<typename T, typename F> struct TThenResult : TThenValueResult<T, F, std::is_invocable_v<F&, const FTaskCancellationToken&, T&>> {};
		template<typename F> struct TThenResult<void, F> : TTokenResult<F> {};

		inline auto Target(ETaskExecutor Executor) -> ETaskTarget
		{
			if (Executor == ETaskExecutor::Worker) return ETaskTarget::AnyWorker;
			if (Executor == ETaskExecutor::BlockingIO) return ETaskTarget::BlockingIO;
			if (Executor == ETaskExecutor::GameThreadDeferred) return ETaskTarget::GameThreadDeferred;
			return static_cast<ETaskTarget>(255);
		}
		template<typename Value>
		auto MakeResultState() -> std::shared_ptr<TTaskResultState<Value>>
		{
			if constexpr (std::same_as<Value, std::monostate>) return Durin::Private::MakeVoidTaskResultState();
			else return std::make_shared<TTaskResultState<Value>>();
		}

		// Estimate the concrete storage before erasing the callables. Dynamic payloads
		// behind captures and results remain the responsibility of their owning domain.
		template<typename Value, typename Body, typename Completion>
		auto LaunchWithStorage(const char* Name, const std::shared_ptr<TTaskResultState<Value>>& Result,
			Body&& Function, Completion&& OnComplete, const FTaskLaunchOptions& Options)
		{
			constexpr uint64 StorageBytes = sizeof(std::decay_t<Body>) + sizeof(std::decay_t<Completion>)
				+ sizeof(TTaskResultState<Value>) + (std::same_as<Value, std::monostate> ? 0 : sizeof(Value));
			return Durin::Private::TryLaunchCancelableTaskWithCompletion(Name, std::forward<Body>(Function),
				std::forward<Completion>(OnComplete), Options, StorageBytes, {Result, !std::same_as<Value, std::monostate>});
		}
		template<typename Value, typename Body, typename Completion>
		auto ContinueWithStorage(const FTask& Predecessor, const char* Name,
			const std::shared_ptr<TTaskResultState<Value>>& Result, Body&& Function,
			Completion&& OnComplete, const FTaskContinuationOptions& Options)
		{
			constexpr uint64 StorageBytes = sizeof(std::decay_t<Body>) + sizeof(std::decay_t<Completion>)
				+ sizeof(TTaskResultState<Value>) + (std::same_as<Value, std::monostate> ? 0 : sizeof(Value));
			return Durin::Private::TryLaunchContinuationTask(Predecessor, Name, std::forward<Body>(Function),
				std::forward<Completion>(OnComplete), Options, StorageBytes, {Result, !std::same_as<Value, std::monostate>});
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
			auto State = Detail::MakeResultState<TTaskValue<T>>();
			FTaskLaunchOptions Launch;
			Launch.Pipe = Pipe;
			Launch.bIndependentCPU = bIndependentCPU;
			Launch.Attribution = Options.Attribution;
			Launch.CancellationToken = Options.Cancellation;
			Launch.Target = Detail::Target(Executor);
			Launch.Priority = Options.Priority;
			Launch.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
			Launch.bQueueOnSaturation = true;
			Launch.Prerequisites = Options.Prerequisites;
			Launch.GenerationToken = Options.GenerationToken;
			Launch.CoalescingKey = Options.CoalescingKey;
			auto Admission = Detail::LaunchWithStorage<TTaskValue<T>>(Options.DebugName, State,
				[State, Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
					auto Invoke = [&]() -> T {
						if constexpr (std::is_invocable_v<std::decay_t<F>&, const FTaskCancellationToken&>) return std::invoke(Function, Token);
						else return std::invoke(Function);
					};
					if constexpr (std::is_void_v<T>) { Invoke(); State->SetPending(std::monostate{}); }
					else State->SetPending(Invoke());
				}, [State](ETaskState Terminal) { State->Complete(Terminal); }, Launch);
			if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			return Detail::FTaskAccess::Make<T>(std::move(Handle), std::move(State));
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
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

	namespace Detail
	{
	// Success continuations check only their primary input; extra prerequisites
	// constrain ordering independently of their terminal outcomes.
	template<typename T, typename F, typename U = typename Detail::TThenResult<T, std::decay_t<F>>::Type>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto ThenImpl(const FTask& Input, std::shared_ptr<TTaskResultState<TTaskValue<T>>> Source,
		std::shared_ptr<FTaskFailure> SourceFailure, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		auto& Native = Input;
		if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});

		FTaskContinuationOptions Edge;
		Edge.bQueueOnSaturation = true;
		Edge.GenerationToken = Options.GenerationToken;
		Edge.CoalescingKey = Options.CoalescingKey;
		Edge.Prerequisites = Options.Prerequisites;
		Edge.Target = Detail::Target(Executor);
		Edge.Priority = Options.Priority;
		Edge.CancellationToken = Options.Cancellation;
		Edge.Attribution = Options.Attribution;
		Edge.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
		try
		{
			auto Failure = std::make_shared<FTaskFailure>();
			const auto Predecessor = FTask(Native);
			auto Output = Detail::MakeResultState<TTaskValue<U>>();
			auto Admission = Detail::ContinueWithStorage<TTaskValue<U>>(FTask(Native), Options.DebugName, Output,
				[Source, SourceFailure, Failure, Predecessor, Output, Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
					if (Predecessor.GetState() != ETaskState::Succeeded)
					{
						if (Predecessor.GetState() == ETaskState::Failed)
						{
							*Failure = Detail::ReadFailure(Predecessor, SourceFailure.get());
							throw Detail::FPropagationFailure{};
						}
						Durin::Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome();
						return;
					}
					auto Value = Source ? Source->PeekPublished() : nullptr;
					if constexpr (!std::is_void_v<T>) require(Value);
					auto Invoke = [&]() -> U {
						if constexpr (std::is_void_v<T>)
						{
							if constexpr (std::is_invocable_v<std::decay_t<F>&, const FTaskCancellationToken&>) return std::invoke(Function, Token);
							else return std::invoke(Function);
						}
						else if constexpr (std::is_invocable_v<std::decay_t<F>&, const FTaskCancellationToken&, T&>) return std::invoke(Function, Token, *Value);
						else return std::invoke(Function, *Value);
					};
					if constexpr (std::is_void_v<U>) { Invoke(); Output->SetPending(std::monostate{}); }
					else Output->SetPending(Invoke());
				}, [Output](ETaskState Terminal) { Output->Complete(Terminal); },
				Edge);
			if (!Admission.HasValue())
			{
				Detail::FailConstruction(Admission.GetError());
			}
			auto Handle = std::move(Admission).TakeValue();
			return Detail::FTaskAccess::Make<U>(std::move(Handle), std::move(Output), std::move(Failure));
		}
		catch (const std::bad_alloc&)
		{
			Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted});
		}
	}

	} // namespace Detail

	// Generic handles provide success ordering without typed result access.
	template<typename F>
	auto Then(const FTask& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
	{
		return Detail::ThenImpl<void>(Input, {}, {}, Executor, Options, std::forward<F>(Function));
	}

	// Continuations observe shared input results and never consume their handles.
	template<typename T, typename F, typename U = typename Detail::TThenResult<T, std::decay_t<F>>::Type>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto Then(const TTask<T>& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		return Detail::ThenImpl<T>(Input, Detail::FTaskAccess::GetResultState(Input),
			Detail::FTaskAccess::Failure(Input), Executor, Options, std::forward<F>(Function));
	}

	// Observes a completed task in any terminal state; admission/cancellation can suppress this edge.
	template<typename T, typename F, typename U = std::invoke_result_t<std::decay_t<F>&, const TTask<T>&>>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto ThenCompleted(const TTask<T>& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		const auto Predecessor = FTask(Input);
		if (!Predecessor.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});

		FTaskContinuationOptions Edge;
		Edge.bQueueOnSaturation = true;
		Edge.GenerationToken = Options.GenerationToken;
		Edge.CoalescingKey = Options.CoalescingKey;
		Edge.Prerequisites = Options.Prerequisites;
		Edge.Target = Detail::Target(Executor);
		Edge.Priority = Options.Priority;
		Edge.CancellationToken = Options.Cancellation;
		Edge.Attribution = Options.Attribution;
		Edge.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
		try
		{
			auto Failure = std::make_shared<FTaskFailure>();
			auto Output = Detail::MakeResultState<TTaskValue<U>>();
			auto Admission = Detail::ContinueWithStorage<TTaskValue<U>>(Predecessor, Options.DebugName, Output,
				[Input, Output, Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable {
					if constexpr (std::is_void_v<U>)
					{
						std::invoke(Function, Input);
						Output->SetPending(std::monostate{});
					}
					else Output->SetPending(std::invoke(Function, Input));
				}, [Output](ETaskState Terminal) { Output->Complete(Terminal); },
				Edge);
			if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			return Detail::FTaskAccess::Make<U>(std::move(Handle), std::move(Output), std::move(Failure));
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	// One producer lifetime; cancellation retains external work until explicit acknowledgement.
	template<typename T>
	class TCompletionSource
	{
		struct FSource
		{
			FTask Handle;
			std::shared_ptr<TTaskResultState<TTaskValue<T>>> Storage;
			std::shared_ptr<FTaskFailure> Failure = std::make_shared<FTaskFailure>();
			std::atomic<bool> bResolved = false;
			std::atomic<bool> bTaskTaken = false;
			~FSource()
			{
				if (Handle.IsValid() && !bResolved.exchange(true))
				{
					Failure->Code = ETaskFailureCode::AbandonedSource;
					Durin::Private::FTaskRuntimeAccess::CompleteExternal(Handle, ETaskState::Failed);
				}
			}
		};
	public:
		// Data-only completion independent of scheduler admission and shutdown.
		static auto CreateDetached() -> TCompletionSource
		{
			auto Source = std::make_shared<FSource>();
			Source->Storage = Detail::MakeResultState<TTaskValue<T>>();
			Source->Handle = Durin::Private::FTaskRuntimeAccess::CreateDetachedCompletion(
				[Storage = Source->Storage](ETaskState Terminal) { Storage->Complete(Terminal); },
				{Source->Storage, !std::same_as<TTaskValue<T>, std::monostate>});
			return TCompletionSource(std::move(Source));
		}
		// External producer in the scheduler lifetime.
		static auto Create(const FTaskExecutionOptions& Options = {}) -> TCompletionSource
		{
			return CreateInternal(Options, true);
		}
		private:
		friend struct Detail::FCompletionSourceAccess;
		static auto CreateInternal(const FTaskExecutionOptions& Options, bool bUnknownRequirements)
			-> TCompletionSource
		{
			try
			{
				auto Source = std::make_shared<FSource>();
				Source->Storage = Detail::MakeResultState<TTaskValue<T>>();
				FTaskLaunchOptions Launch;
				Launch.Attribution = Options.Attribution;
				Launch.CancellationToken = Options.Cancellation;
				Launch.bExternalCompletion = true;
				Launch.bQueueOnSaturation = true;
				Launch.bUnknownExecutionRequirement = bUnknownRequirements;
				auto Admission = Detail::LaunchWithStorage<TTaskValue<T>>(Options.DebugName, Source->Storage,
					[](const FTaskCancellationToken&) {},
					[Storage = Source->Storage](ETaskState Terminal) { Storage->Complete(Terminal); }, Launch);
				if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
				Source->Handle = std::move(Admission).TakeValue();
				return TCompletionSource(std::move(Source));
			}
			catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
		}
	public:
		auto TakeTask() -> TTask<T>
		{
			require(Source && !Source->bTaskTaken.exchange(true));
			return Detail::FTaskAccess::Make<T>(Source->Handle, Source->Storage, Source->Failure);
		}
		auto GetTaskHandle() const -> FTask { return FTask(Source->Handle); }
		auto TrySetValue(TTaskValue<T> Value = {}) const -> bool
		{
			if (Source->bResolved.exchange(true)) return false;
			try { Source->Storage->SetPending(std::move(Value)); }
			catch (...) { Durin::Private::FTaskRuntimeAccess::CompleteExternal(Source->Handle, ETaskState::Failed); return true; }
			Durin::Private::FTaskRuntimeAccess::CompleteExternal(Source->Handle, ETaskState::Succeeded);
			return true;
		}
		auto TrySetFailure(FTaskFailure Failure) const -> bool
		{
			if (Source->bResolved.exchange(true)) return false;
			*Source->Failure = std::move(Failure);
			Durin::Private::FTaskRuntimeAccess::CompleteExternal(Source->Handle, ETaskState::Failed);
			return true;
		}
		auto TrySetCanceled() const -> bool
		{
			if (Source->bResolved.exchange(true)) return false;
			Durin::Private::FTaskRuntimeAccess::CompleteExternal(Source->Handle, ETaskState::Canceled);
			return true;
		}
	private:
		auto TrySetTaskResult(const TTask<T>& Task) const -> bool
		{
			if (Source->bResolved.exchange(true)) return false;
			Source->Storage->ForwardResult(Detail::FTaskAccess::GetResultState(Task), FTask(Task));
			Durin::Private::FTaskRuntimeAccess::CompleteExternal(Source->Handle, ETaskState::Succeeded);
			return true;
		}
		explicit TCompletionSource(std::shared_ptr<FSource> InSource) : Source(std::move(InSource)) {}
		std::shared_ptr<FSource> Source;
	};

	template<typename T>
	auto MakeReadyTask(T Value) -> TTask<T>
	{
		auto Source = TCompletionSource<T>::CreateDetached();
		auto Task = Source.TakeTask();
		Source.TrySetValue(std::move(Value));
		return Task;
	}

	namespace Detail
	{
		struct FCompletionSourceAccess
		{
			template<typename T>
			static auto Create(const FTaskExecutionOptions& Options, bool bUnknownRequirements = true)
				-> TCompletionSource<T>
			{ return TCompletionSource<T>::CreateInternal(Options, bUnknownRequirements); }
			template<typename T> static auto SetTaskResult(const TCompletionSource<T>& Source, const TTask<T>& Task) -> bool
			{ return Source.TrySetTaskResult(Task); }
		};
		template<typename T> struct TInnerTask;
		template<typename T> struct TInnerTask<TTask<T>> { using Type = T; };
	}

	// Flatten an inner task by retaining its result, without consuming any alias.
	template<typename T, typename F, typename R = typename Detail::TThenResult<T, std::decay_t<F>>::Type,
		typename U = typename Detail::TInnerTask<R>::Type>
	auto ThenAsync(const TTask<T>& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
		try
		{
			const FTask Predecessor = Input;
			const auto PredecessorFailure = Detail::FTaskAccess::Failure(Input);
			auto Source = Detail::FCompletionSourceAccess::Create<U>(Options);
			auto Output = Source.TakeTask();
			auto InvokeInner = [Source, Function = std::forward<F>(Function)](auto&&... Args) mutable
				requires std::is_invocable_v<std::decay_t<F>&, decltype(Args)...> {
				auto Inner = std::invoke(Function, std::forward<decltype(Args)>(Args)...);
				if (auto Error = Durin::Private::FTaskRuntimeAccess::BindDynamicDependency(Source.GetTaskHandle(), Inner))
				{
					requiref(Error->Code != ETaskAdmissionErrorCode::CapacityExhausted, "Dynamic task dependency allocation failed.");
					Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, *Error, ETaskFailureCode::DependencyBindingFailed});
					return;
				}
				auto Hook = std::make_shared<Durin::Private::FTaskTerminalHook>();
				Hook->Function = [Source, Inner, Failure = Detail::FTaskAccess::Failure(Inner)](ETaskState Terminal) {
					if (Terminal == ETaskState::Succeeded) Detail::FCompletionSourceAccess::SetTaskResult(Source, Inner);
					// Hooks precede public terminal visibility; GetFailure() is not valid here.
					else if (Terminal == ETaskState::Failed) Source.TrySetFailure(Failure ? *Failure : FTaskFailure{});
					else Source.TrySetCanceled();
				};
				Durin::Private::FTaskRuntimeAccess::BindTerminal(Inner, std::move(Hook));
			};
			auto Runner = Then(Input, Executor, Options, std::move(InvokeInner));
			const auto RunnerFailure = Detail::FTaskAccess::Failure(Runner);
			auto Hook = std::make_shared<Durin::Private::FTaskTerminalHook>();
			Hook->Function = [Source, Predecessor, PredecessorFailure, RunnerFailure](ETaskState Terminal) {
				if (Terminal == ETaskState::Failed) Source.TrySetFailure(*RunnerFailure);
				else if (Terminal == ETaskState::Canceled)
				{
					if (Predecessor.GetState() == ETaskState::Failed)
						Source.TrySetFailure(Detail::ReadFailure(Predecessor, PredecessorFailure.get()));
					else Source.TrySetCanceled();
				}
			};
			Durin::Private::FTaskRuntimeAccess::BindTerminal(Runner, std::move(Hook));
			return Output;
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	namespace Detail
	{
		template<typename Out, typename F>
		auto BuildFanIn(const std::vector<FTask>& Inputs, ETaskExecutor Executor, FTaskExecutionOptions Options,
			F&& Gather) -> TTask<Out>
		{
			if (Inputs.empty())
			{
				if (!Options.Prerequisites.empty()) return LaunchTaskImpl(Executor, Options, std::forward<F>(Gather));
				auto Source = FCompletionSourceAccess::Create<Out>(Options, false);
				auto Result = Source.TakeTask();
				Source.TrySetValue(Out{});
				return Result;
			}
			const auto Prerequisites = CollectDependencies(Inputs, Options.Prerequisites);
			FTaskContinuationOptions Edge;
			Edge.bQueueOnSaturation = true;
			Edge.GenerationToken = Options.GenerationToken;
			Edge.CoalescingKey = Options.CoalescingKey;
			Edge.Prerequisites = Prerequisites;
			Edge.Target = Target(Executor);
			Edge.Priority = Options.Priority;
			Edge.CancellationToken = Options.Cancellation;
			Edge.Attribution = Options.Attribution;
			Edge.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
			auto Failure = std::make_shared<FTaskFailure>();
			auto Output = MakeResultState<Out>();
			auto Admission = ContinueWithStorage<Out>(Inputs.front(), Options.DebugName, Output,
				[Inputs, Output, Failure, Gather = std::forward<F>(Gather)](const FTaskCancellationToken&) mutable {
					bool bCanceled = false;
					for (size_t Index = 0; Index < Inputs.size(); ++Index)
					{
						const auto& Input = Inputs[Index];
						if (Input.GetState() == ETaskState::Failed)
						{
							*Failure = {ETaskTerminalReason::DependencyFailed, {}, ETaskFailureCode::DependencyFailed, Input.GetTaskId(), Index};
							throw FPropagationFailure{};
						}
						bCanceled |= Input.GetState() == ETaskState::Canceled;
					}
					if (bCanceled) { Durin::Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome(); return; }
					Output->SetPending(Gather());
				}, [Output](ETaskState Terminal) { Output->Complete(Terminal); }, Edge);
			if (!Admission.HasValue()) FailConstruction(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			return FTaskAccess::Make<Out>(std::move(Handle), std::move(Output), std::move(Failure));
		}
	}

	// Fan-in returns owning immutable views, preserving move-only results and duplicate inputs.
	template<typename T>
	auto WhenAll(const std::vector<TTask<T>>& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::vector<std::shared_ptr<const TTaskValue<T>>>>
	{
		using FOut = std::vector<std::shared_ptr<const TTaskValue<T>>>;
		try
		{
			std::vector<FTask> Handles;
			for (const auto& Input : Inputs)
			{
				if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
				Handles.emplace_back(Input);
			}
			return Detail::BuildFanIn<FOut>(Handles, Executor, Options, [Inputs] {
				FOut Values; Values.reserve(Inputs.size());
				for (const auto& Input : Inputs) Values.emplace_back(Input.GetResultShared());
				return Values;
			});
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	template<typename... Ts>
	auto WhenAll(const std::tuple<TTask<Ts>...>& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::tuple<std::shared_ptr<const TTaskValue<Ts>>...>>
	{
		using FOut = std::tuple<std::shared_ptr<const TTaskValue<Ts>>...>;
		const auto Handles = std::apply([](const auto&... Task) { return std::vector<FTask>{Task...}; }, Inputs);
		for (const auto& Handle : Handles) if (!Handle.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
		return Detail::BuildFanIn<FOut>(Handles, Executor, Options,
			[Inputs] { return std::apply([](const auto&... Task) { return FOut{Task.GetResultShared()...}; }, Inputs); });
	}
}
