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
		CORE_API auto WaitForSuccess(const FTaskHandle& Handle) -> void;
		// Requires failure; absent stored failure uses the override or the native terminal reason.
		CORE_API auto ReadFailure(const FTaskHandle& Handle, const FTaskFailure* Failure,
			std::optional<ETaskTerminalReason> FallbackReason = {}) -> FTaskFailure;
	}
	template<typename T> using TTaskValue = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

	// Execution and owner identity; scheduler storage accounting is internal.
	struct FTaskExecutionOptions
	{
		const char* DebugName = "Task";
		ETaskPriority Priority = ETaskPriority::Normal;
		FTaskCancellationToken Cancellation;
		FTaskAttribution Attribution;
		std::span<const FTaskHandle> Prerequisites;
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

	CORE_API auto Wait(const FTaskHandle& Task) -> FTaskWaitResult;
	CORE_API auto Cancel(const FTaskHandle& Task) -> bool;

	// Completion dependency only: does not forward cancellation or failure to either task.
	// Must be called from a running task body; its worker is free after the body returns.
	CORE_API auto AddNested(const FTaskHandle& Task) -> void;

	// Explicit one-shot signal, optionally joined with other completion dependencies.
	// Copies share one signal. The last handle must not be destroyed before Trigger().
	class FTaskEvent : public Private::FTaskHandle
	{
	public:
		// Set false only when signaling never needs GameThread or unknown external progress.
		CORE_API explicit FTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement = true);
		CORE_API auto AddPrerequisites(std::span<const Durin::FTaskHandle> Prerequisites) -> void;
		auto AddPrerequisites(const Durin::FTaskHandle& Prerequisite) -> void { AddPrerequisites(std::span(&Prerequisite, 1)); }
		CORE_API auto Trigger() -> void;
	private:
		struct FState;
		std::shared_ptr<FState> State;
	};

	namespace Detail
	{
		template<typename T> concept CTaskDependency = std::same_as<std::remove_cvref_t<T>, FTaskHandle>
			|| std::derived_from<std::remove_cvref_t<T>, Private::FTaskHandle>;
		template<CTaskDependency T> auto DependencyHandle(const T& Task) -> FTaskHandle
		{ return Task; }
	}

	// Owns only dependency handles, never a unique result claim. The returned
	// collection can be retained independently of the task/event facades.
	template<Detail::CTaskDependency... T>
	auto Prerequisites(const T&... Tasks) -> std::array<FTaskHandle, sizeof...(T)>
	{ return {Detail::DependencyHandle(Tasks)...}; }


	// Invocation-scoped authority cannot be copied into later asynchronous callbacks.
	class FTaskContext
	{
	public:
		FTaskContext(const FTaskCancellationToken& InCancellation)
			: Cancellation(InCancellation), ParentTaskId(Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId()) {}
		FTaskContext(const FTaskContext&) = delete;
		FTaskContext(FTaskContext&&) = delete;
		auto GetCancellationToken() const -> const FTaskCancellationToken& { return Cancellation; }
		template<typename F> auto LaunchChild(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function);
	private:
		FTaskCancellationToken Cancellation;
		uint64 ParentTaskId = 0;
	};

	namespace Detail { struct FTaskAccess; }
	template<typename T> class TSharedTask;

	// Move-only result ownership; status observation alone never prolongs a unique claim.
	template<typename T>
	class TTask : public Private::FTaskHandle
	{
	public:
		TTask() = default;
		~TTask() { if (Storage) Storage->Discard(); }
		TTask(TTask&&) noexcept = default;
		auto operator=(TTask&& Other) noexcept -> TTask&
		{
			if (this == &Other) return *this;
			if (Storage) Storage->Discard();
			Private::FTaskHandle::operator=(std::move(Other));
			Storage = std::move(Other.Storage);
			Failure = std::move(Other.Failure);
			return *this;
		}
		TTask(const TTask&) = delete;
		auto operator=(const TTask&) -> TTask& = delete;
		// Requires a failed terminal state; returns a copy independent of result ownership.
		auto GetFailure() const -> FTaskFailure
		{
			return Detail::ReadFailure(*this, Failure.get());
		}
		// Waits for success. The reference is invalidated by result consumption or task destruction.
		auto GetResult() const -> std::conditional_t<std::is_void_v<T>, void, const TTaskValue<T>&>
		{
			Detail::WaitForSuccess(*this);
			if constexpr (!std::is_void_v<T>) return *Storage->PeekPublished();
		}
		// Waits for success, then transfers the result exactly once and invalidates this task.
		// Rejected waits, failure and cancellation must be handled before requesting a result.
		auto TakeResult() && -> T
		{
			Detail::WaitForSuccess(*this);
			const Durin::FTaskHandle ProducerLifetime = *this;
			auto ResultState = Storage;
			const auto Claim = ResultState->ReserveClaim();
			require(Claim != 0);
			SetHandle({}); Storage.reset();
			auto Value = ResultState->TakePublished();
			require(Value);
			if constexpr (!std::is_void_v<T>) return std::move(*Value);
		}

	private:
		friend struct Detail::FTaskAccess;
		std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> Storage;
		std::shared_ptr<FTaskFailure> Failure;
	};

	// A void task retains the same move-only ownership and completion contract.
	using FTask = TTask<void>;

	// Explicit immutable fan-out shares the result storage without a consuming alias.
	template<typename T>
	class TSharedTask : public Private::FTaskHandle
	{
	public:
		auto GetResultShared() const -> std::shared_ptr<const TTaskValue<T>>
		{
			if (!IsCompleted() || GetState() != ETaskState::Succeeded) return {};
			return std::shared_ptr<const TTaskValue<T>>(LifetimePin, Storage->PeekPublished());
		}
		// Requires a failed terminal state; preserves the producer's failure identity.
		auto GetFailure() const -> FTaskFailure
		{
			return Detail::ReadFailure(*this, Failure.get(), ETaskTerminalReason::DependencyFailed);
		}
		// Waits for success. The immutable reference is valid while this shared task is retained.
		// Use GetResultShared when the result must outlive the task handle.
		auto GetResult() const -> std::conditional_t<std::is_void_v<T>, void, const TTaskValue<T>&>
		{
			Detail::WaitForSuccess(*this);
			if constexpr (!std::is_void_v<T>) return *Storage->PeekPublished();
		}

	private:
		friend struct Detail::FTaskAccess;
		std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> Storage;
		std::shared_ptr<void> LifetimePin;
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
			template<typename T> static auto GetResultState(TTask<T>& Task) -> auto& { return Task.Storage; }
			template<typename T> static auto GetResultState(const TTask<T>& Task) -> const auto& { return Task.Storage; }
			template<typename T> static auto InvalidateAfterClaim(TTask<T>& Task) -> void
			{ Task.SetHandle({}); Task.Storage.reset(); }
			template<typename To, typename From> static auto Rebind(TTask<From>&& Input) -> TTask<To>
			{
				static_assert(std::same_as<TTaskValue<To>, TTaskValue<From>>);
				TTask<To> Output;
				Output.SetHandle(Input.TakeHandle()); Output.Storage = std::move(Input.Storage); Output.Failure = std::move(Input.Failure);
				return Output;
			}
			template<typename T> static auto SharedStorage(const TSharedTask<T>& Input) -> const auto& { return Input.Storage; }
			template<typename T> static auto Failure(TTask<T>& Task) -> auto& { return Task.Failure; }
			template<typename T> static auto Make(FTaskHandle Handle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> State, std::shared_ptr<FTaskFailure> Failure = {}) -> TTask<T>
			{
				TTask<T> Task;
				Task.Failure = std::move(Failure);
				Task.SetHandle(std::move(Handle)); Task.Storage = std::move(State);
				return Task;
			}
			template<typename T> static auto MakeShared(FTaskHandle Handle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> State, std::shared_ptr<FTaskFailure> Failure = {}) -> TSharedTask<T>
			{
				TSharedTask<T> Task;
				Task.Failure = std::move(Failure);
				Task.LifetimePin = std::make_shared<std::pair<FTaskHandle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>>>>(Handle, State);
				Task.SetHandle(std::move(Handle));
				Task.Storage = std::move(State);
				return Task;
			}
		};
		template<typename T> struct TIsTask : std::false_type {};
		template<typename T> struct TIsTask<TTask<T>> : std::true_type {};
		template<typename T> struct TIsTask<TSharedTask<T>> : std::true_type {};
		template<typename T> struct TIsTask<TTaskAdmission<T>> : std::true_type {};
		template<typename F, bool = std::is_invocable_v<F&, FTaskContext&>> struct TSpawnResult;
		template<typename F> struct TSpawnResult<F, true> { using Type = std::invoke_result_t<F&, FTaskContext&>; };
		template<typename F, bool = std::is_invocable_v<F&, const FTaskCancellationToken&>> struct TTokenResult { using Type = std::invoke_result_t<F&>; };
		template<typename F> struct TTokenResult<F, true> { using Type = std::invoke_result_t<F&, const FTaskCancellationToken&>; };
		template<typename F> struct TSpawnResult<F, false> : TTokenResult<F> {};
		template<typename T, typename F, bool> struct TThenValueResult;
		template<typename T, typename F> struct TThenValueResult<T, F, false> { using Type = std::invoke_result_t<F&, T&&>; };
		template<typename T, typename F> struct TThenValueResult<T, F, true> { using Type = std::invoke_result_t<F&, FTaskContext&, T&&>; };
		template<typename T, typename F> struct TThenResult : TThenValueResult<T, F, std::is_invocable_v<F&, FTaskContext&, T&&>> {};
		template<typename F> struct TThenResult<void, F> : TSpawnResult<F> {};

		inline auto Target(ETaskExecutor Executor) -> ETaskTarget
		{
			if (Executor == ETaskExecutor::Worker) return ETaskTarget::AnyWorker;
			if (Executor == ETaskExecutor::BlockingIO) return ETaskTarget::BlockingIO;
			if (Executor == ETaskExecutor::GameThreadDeferred) return ETaskTarget::GameThreadDeferred;
			return static_cast<ETaskTarget>(255);
		}
		template<typename T>
		auto ResultBytes() -> uint64
		{
			if constexpr (std::is_void_v<T> || std::same_as<T, std::monostate>) return 0;
			else return sizeof(T);
		}

	}

	namespace Detail
	{
	template<typename F, typename T = typename Detail::TSpawnResult<std::decay_t<F>>::Type>
	requires (!std::is_reference_v<T> && !Detail::TIsTask<T>::value)
	auto LaunchTaskImpl(uint64 ParentTaskId, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function, bool bIndependentCPU = false)
		-> TTask<T>
	{
		const uint64 Bytes = Detail::ResultBytes<T>();

		try
		{
			auto State = std::make_shared<TUniqueTaskResultState<TTaskValue<T>>>(Bytes);
			FTaskLaunchOptions Launch;
			Launch.bIndependentCPU = bIndependentCPU;
			Launch.ExpectedParentTaskId = ParentTaskId;
			Launch.Attribution = Options.Attribution;
			Launch.CancellationToken = Options.Cancellation;
			Launch.Target = Detail::Target(Executor);
			Launch.Priority = Options.Priority;
			Launch.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
			Launch.bQueueOnSaturation = true;
			Launch.Prerequisites = Options.Prerequisites;
			Launch.GenerationToken = Options.GenerationToken;
			Launch.CoalescingKey = Options.CoalescingKey;
			auto Admission = Durin::Private::TryLaunchCancelableTaskWithCompletion(Options.DebugName,
				[State, Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
					FTaskContext Context(Token);
					auto Invoke = [&]() -> T {
						if constexpr (std::is_invocable_v<std::decay_t<F>&, FTaskContext&>) return std::invoke(Function, Context);
						else if constexpr (std::is_invocable_v<std::decay_t<F>&, const FTaskCancellationToken&>) return std::invoke(Function, Token);
						else return std::invoke(Function);
					};
					if constexpr (std::is_void_v<T>) { Invoke(); State->SetPending(std::monostate{}); }
					else State->SetPending(Invoke());
				}, [State](ETaskState Terminal) { State->Complete(Terminal); }, Launch, Bytes);
			if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			State->BindProducer(Handle);
			return Detail::FTaskAccess::Make<T>(std::move(Handle), std::move(State));
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	}

	// Preserve the executing parent identity for nested work, including blocking I/O.
	template<typename F>
	auto LaunchTask(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
	{
		return Detail::LaunchTaskImpl(
			Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId(), Executor, Options, std::forward<F>(Function));
	}

	// Worker root in the scheduler lifetime. Owners stop their own submission before shutdown.
	template<typename F>
	auto LaunchTask(const char* Name, F&& Function, FTaskExecutionOptions Options = {})
	{
		Options.DebugName = Name;
		return Detail::LaunchTaskImpl(0, ETaskExecutor::Worker, Options, std::forward<F>(Function));
	}

	namespace Detail
	{
		template<typename F>
		auto WithPrerequisites(FTaskExecutionOptions Options, std::span<const FTaskHandle> Inputs, F&& Launch)
		{
			try
			{
				std::vector<FTaskHandle> Handles(Options.Prerequisites.begin(), Options.Prerequisites.end());
				Handles.insert(Handles.end(), Inputs.begin(), Inputs.end());
				Options.Prerequisites = Handles;
				return Launch(Options);
			}
			catch (const std::bad_alloc&)
			{
				FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted});
			}
		}
	}

	template<typename F, size_t N>
	auto LaunchTask(ETaskExecutor Executor, const FTaskExecutionOptions& Options,
		F&& Function, const std::array<FTaskHandle, N>& Inputs)
	{
		return Detail::WithPrerequisites(Options, Inputs, [&](const auto& BoundOptions) {
			return LaunchTask(Executor, BoundOptions, std::forward<F>(Function));
		});
	}
	template<typename F, Detail::CTaskDependency T>
	auto LaunchTask(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function, const T& Input)
	{ return LaunchTask(Executor, Options, std::forward<F>(Function), Prerequisites(Input)); }

	template<typename F, size_t N>
	auto LaunchTask(const char* Name, F&& Function, const std::array<FTaskHandle, N>& Inputs,
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
		return Detail::LaunchTaskImpl(0, ETaskExecutor::Worker,
			Options, std::forward<F>(Function), true);
	}

	// A completion prerequisite contributes no result ownership to this success edge.
	template<typename F>
	auto Then(const FTaskHandle& Input, ETaskExecutor Executor, FTaskExecutionOptions Options, F&& Function)
	{
		try
		{
			const auto& Handle = Input;
			require(Handle.IsValid());
			if (Options.Attribution == FTaskAttribution{})
				Options.Attribution = Durin::Private::FTaskRuntimeAccess::GetAttribution(Handle);
			std::vector<FTaskHandle> Prerequisites(Options.Prerequisites.begin(), Options.Prerequisites.end());
			Prerequisites.push_back(Handle);
			Options.Prerequisites = Prerequisites;
			return Detail::LaunchTaskImpl(Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId(),
				Executor, Options, std::forward<F>(Function));
		}
		catch (const std::bad_alloc&)
		{
			Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted});
		}
	}


	template<typename F>
	auto FTaskContext::LaunchChild(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
	{
		if (!ParentTaskId || ParentTaskId != Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId())
			Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidExecutionContract});
		return Detail::LaunchTaskImpl(ParentTaskId, Executor, Options, std::forward<F>(Function));
	}

	// Unique claims remain transactional through native graph registration.
	template<typename T, typename F, typename U = typename Detail::TThenResult<T, std::decay_t<F>>::Type>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto Then(TTask<T>&& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		auto& Native = Input;
		if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
		auto Source = Detail::FTaskAccess::GetResultState(Native);
		uint64 Bytes = Detail::ResultBytes<U>();

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
		if (Executor == ETaskExecutor::GameThreadDeferred)
		{
			if (Edge.EstimatedPayloadBytes > std::numeric_limits<uint64>::max() - Source->GetEstimatedResultBytes())
				Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
			Edge.EstimatedPayloadBytes += Source->GetEstimatedResultBytes();
		}
		const auto Claim = Source->ReserveClaim();
		if (!Claim) Detail::FailConstruction({ETaskAdmissionErrorCode::UniqueConsumerClaimed});
		try
		{
			auto Failure = std::make_shared<FTaskFailure>();
			const auto Predecessor = FTaskHandle(Native);
			auto SourceFailure = Detail::FTaskAccess::Failure(Input);
			auto Output = std::make_shared<TUniqueTaskResultState<TTaskValue<U>>>(Bytes);
			auto Admission = Durin::Private::TryLaunchContinuationTask(FTaskHandle(Native), Options.DebugName,
				[Source, SourceFailure, Failure, Predecessor, Output, Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
					if (Predecessor.GetState() != ETaskState::Succeeded)
					{
						if (Predecessor.GetState() == ETaskState::Failed)
						{
							*Failure = SourceFailure ? *SourceFailure : FTaskFailure{ETaskTerminalReason::DependencyFailed};
							throw Detail::FPropagationFailure{};
						}
						Durin::Private::FTaskRuntimeAccess::CancelCurrent();
						return;
					}
					auto Value = Source->TakePublished();
					require(Value);
					FTaskContext Context(Token);
					auto Invoke = [&]() -> U {
						if constexpr (std::is_void_v<T>)
						{
							if constexpr (std::is_invocable_v<std::decay_t<F>&, FTaskContext&>) return std::invoke(Function, Context);
							else return std::invoke(Function);
						}
						else if constexpr (std::is_invocable_v<std::decay_t<F>&, FTaskContext&, T&&>) return std::invoke(Function, Context, std::move(*Value));
						else return std::invoke(Function, std::move(*Value));
					};
					if constexpr (std::is_void_v<U>) { Invoke(); Output->SetPending(std::monostate{}); }
					else Output->SetPending(Invoke());
				}, [Source, Output](ETaskState Terminal) { Source->Discard(); Output->Complete(Terminal); },
				Edge, ETaskDependencyKind::Completion, Bytes);
			if (!Admission.HasValue())
			{
				Source->RollbackClaim(Claim);
				Detail::FailConstruction(Admission.GetError());
			}
			auto Handle = std::move(Admission).TakeValue();
			require(Source->CommitClaim(Claim));
			Detail::FTaskAccess::InvalidateAfterClaim(Native);
			Output->BindProducer(Handle);
			return Detail::FTaskAccess::Make<U>(std::move(Handle), std::move(Output), std::move(Failure));
		}
		catch (const std::bad_alloc&)
		{
			Source->RollbackClaim(Claim);
			Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted});
		}
		catch (...)
		{
			Source->RollbackClaim(Claim);
			throw;
		}
	}

	template<typename T>
	auto Share(TTask<T>&& Input) -> TSharedTask<T>
	{
		if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
		auto& Native = Input;
		auto State = Detail::FTaskAccess::GetResultState(Native);
		try
		{
			auto Shared = Detail::FTaskAccess::MakeShared<T>(FTaskHandle(Native), State, Detail::FTaskAccess::Failure(Input));
			if (!State->ReserveClaim()) Detail::FailConstruction({ETaskAdmissionErrorCode::UniqueConsumerClaimed});
			Detail::FTaskAccess::InvalidateAfterClaim(Native);
			return Shared;
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	// Observes a completed task in any terminal state; admission/cancellation can suppress this edge.
	template<typename T, typename F, typename U = std::invoke_result_t<std::decay_t<F>&, const TSharedTask<T>&>>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto ThenCompleted(const TSharedTask<T>& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		const auto Predecessor = FTaskHandle(Input);
		if (!Predecessor.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
		const uint64 Bytes = Detail::ResultBytes<U>();

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
		if (Executor == ETaskExecutor::GameThreadDeferred)
		{
			const uint64 InputBytes = Detail::FTaskAccess::SharedStorage(Input)->GetEstimatedResultBytes();
			if (InputBytes > std::numeric_limits<uint64>::max() - Edge.EstimatedPayloadBytes)
				Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
			Edge.EstimatedPayloadBytes += InputBytes;
		}
		try
		{
			auto Failure = std::make_shared<FTaskFailure>();
			auto Output = std::make_shared<TUniqueTaskResultState<TTaskValue<U>>>(Bytes);
			auto Admission = Durin::Private::TryLaunchContinuationTask(Predecessor, Options.DebugName,
				[Input, Output, Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable {
					if constexpr (std::is_void_v<U>)
					{
						std::invoke(Function, Input);
						Output->SetPending(std::monostate{});
					}
					else Output->SetPending(std::invoke(Function, Input));
				}, [Output](ETaskState Terminal) { Output->Complete(Terminal); },
				Edge, ETaskDependencyKind::Completion, Bytes);
			if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			Output->BindProducer(Handle);
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
			FTaskHandle Handle;
			std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> Storage;
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
				const uint64 Bytes = Detail::ResultBytes<T>();

				auto Source = std::make_shared<FSource>();
				Source->Storage = std::make_shared<TUniqueTaskResultState<TTaskValue<T>>>(Bytes);
				FTaskLaunchOptions Launch;
				Launch.Attribution = Options.Attribution;
				Launch.CancellationToken = Options.Cancellation;
				Launch.ExpectedParentTaskId = Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId();
				Launch.bExternalCompletion = true;
				Launch.bQueueOnSaturation = true;
				Launch.bUnknownExecutionRequirement = bUnknownRequirements;
				auto Admission = Durin::Private::TryLaunchCancelableTaskWithCompletion(Options.DebugName,
					[](const FTaskCancellationToken&) {},
					[Storage = Source->Storage](ETaskState Terminal) { Storage->Complete(Terminal); }, Launch, Bytes);
				if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
				Source->Handle = std::move(Admission).TakeValue();
				Source->Storage->BindProducer(Source->Handle);
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
		auto GetTaskHandle() const -> FTaskHandle { return FTaskHandle(Source->Handle); }
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
		explicit TCompletionSource(std::shared_ptr<FSource> InSource) : Source(std::move(InSource)) {}
		std::shared_ptr<FSource> Source;
	};

	namespace Detail
	{
		struct FCompletionSourceAccess
		{
			template<typename T>
			static auto Create(const FTaskExecutionOptions& Options, bool bUnknownRequirements = true)
				-> TCompletionSource<T>
			{ return TCompletionSource<T>::CreateInternal(Options, bUnknownRequirements); }
		};
		template<typename T> struct TInnerTask;
		template<typename T> struct TInnerTask<TTask<T>> { using Type = T; };
	}

	// The outer node remains external/unknown until the callback binds its real inner dependency.
	template<typename T, typename F, typename R = typename Detail::TThenResult<T, std::decay_t<F>>::Type,
		typename U = typename Detail::TInnerTask<R>::Type>
	auto ThenAsync(TTask<T>&& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
		try
		{
			const auto Predecessor = FTaskHandle(Input);
			auto Source = Detail::FCompletionSourceAccess::Create<U>(Options);
			auto Output = Source.TakeTask();
			struct FBinding
			{
				TCompletionSource<U> Source;
				std::shared_ptr<TUniqueTaskResultState<TTaskValue<U>>> Storage;
				std::shared_ptr<FTaskFailure> Failure;
			};
			auto Binding = std::make_shared<FBinding>(Source);
			auto InnerHook = std::make_shared<Durin::Private::FTaskTerminalHook>();
			InnerHook->Function = [Binding](ETaskState Terminal) {
				try
				{
					if (Terminal == ETaskState::Succeeded)
					{
						auto Value = Binding->Storage->TakePublished();
						require(Value);
						Binding->Source.TrySetValue(std::move(*Value));
					}
					else if (Terminal == ETaskState::Failed)
						Binding->Source.TrySetFailure(Binding->Failure ? *Binding->Failure : FTaskFailure{});
					else Binding->Source.TrySetCanceled();
				}
				catch (...) { Binding->Source.TrySetFailure({}); }
			};
			auto RunnerHook = std::make_shared<Durin::Private::FTaskTerminalHook>();
			RunnerHook->Function = [Source, Predecessor](ETaskState Terminal) {
				if (Terminal == ETaskState::Failed || (Terminal == ETaskState::Canceled
					&& (Predecessor.GetState() == ETaskState::Failed || Predecessor.GetDiagnostics().TerminalReason == ETaskTerminalReason::DependencyFailed)))
					Source.TrySetFailure({ETaskTerminalReason::DependencyFailed});
				else if (Terminal == ETaskState::Canceled) Source.TrySetCanceled();
			};
			auto InvokeInner = [Source, Binding, InnerHook, Function = std::forward<F>(Function)](auto&&... Args) mutable
				requires std::is_invocable_v<std::decay_t<F>&, decltype(Args)...> {
				if (Durin::Private::FTaskRuntimeAccess::IsCancellationRequested(Source.GetTaskHandle()))
				{ Source.TrySetCanceled(); return; }
				auto Returned = std::invoke(Function, std::forward<decltype(Args)>(Args)...);
				TTask<U> Inner;
				Inner = std::move(Returned);
				if (auto Error = Durin::Private::FTaskRuntimeAccess::BindDynamicDependency(
					Source.GetTaskHandle(), FTaskHandle(Inner)))
				{
					requiref(Error->Code != ETaskAdmissionErrorCode::CapacityExhausted, "Dynamic task dependency allocation failed.");
					Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, *Error, ETaskFailureCode::DependencyBindingFailed});
					return;
				}
				auto& Native = Inner;
				Binding->Storage = Detail::FTaskAccess::GetResultState(Native);
				Binding->Failure = Detail::FTaskAccess::Failure(Inner);
				if (!Binding->Storage->ReserveClaim())
				{
					Source.TrySetFailure({ETaskTerminalReason::CallbackFailure,
						FTaskAdmissionError{ETaskAdmissionErrorCode::UniqueConsumerClaimed}, ETaskFailureCode::DependencyBindingFailed});
					return;
				}
				const auto Handle = FTaskHandle(Native);
				Detail::FTaskAccess::InvalidateAfterClaim(Native);
				Durin::Private::FTaskRuntimeAccess::BindTerminal(Handle, std::move(InnerHook));
			};
			auto Runner = Then(std::move(Input), Executor, Options, std::move(InvokeInner));
			Durin::Private::FTaskRuntimeAccess::BindTerminal(FTaskHandle(Runner), std::move(RunnerHook));
			return Output;
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	namespace Detail
	{
		// A reserved claim rolls back even when a later input or allocation fails.
		template<typename T>
		struct TClaimLease
		{
			std::shared_ptr<TUniqueTaskResultState<T>> Storage;
			uint64 Token;
			explicit TClaimLease(std::shared_ptr<TUniqueTaskResultState<T>> InStorage)
				: Storage(std::move(InStorage)), Token(Storage->ReserveClaim()) {}
			TClaimLease(TClaimLease&&) noexcept = default;
			TClaimLease(const TClaimLease&) = delete;
			~TClaimLease() { if (Storage && Token) Storage->RollbackClaim(Token); }
			auto Commit() -> void { const bool bCommitted = Storage->CommitClaim(Token); require(bCommitted); Token = 0; }
		};

		template<typename Out, typename F, typename D>
		auto BuildFanIn(const std::vector<FTaskHandle>& Inputs, ETaskExecutor Executor, FTaskExecutionOptions Options,
			uint64 InputBytes, F&& Gather, D&& Discard) -> TTask<Out>
		{
			if (InputBytes > std::numeric_limits<uint64>::max() - sizeof(Out))
				Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
			if (Inputs.empty())
			{
				if (!Options.Prerequisites.empty())
					return LaunchTaskImpl(0, Executor, Options, std::forward<F>(Gather));
				auto Source = FCompletionSourceAccess::Create<Out>(Options, false);
				auto Result = Source.TakeTask();
				Source.TrySetValue(Out{});
				return Result;
			}
			std::vector<FTaskHandle> Prerequisites(Options.Prerequisites.begin(), Options.Prerequisites.end());
			Prerequisites.insert(Prerequisites.end(), Inputs.begin(), Inputs.end());
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
			if (Executor == ETaskExecutor::GameThreadDeferred)
			{
				if (InputBytes > std::numeric_limits<uint64>::max() - Edge.EstimatedPayloadBytes)
					Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
				Edge.EstimatedPayloadBytes += InputBytes;
			}
			auto Failure = std::make_shared<FTaskFailure>();
			auto Output = std::make_shared<TUniqueTaskResultState<Out>>(ResultBytes<Out>());
			auto Admission = Durin::Private::TryLaunchContinuationTask(Inputs.front(), Options.DebugName,
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
					if (bCanceled) { Durin::Private::FTaskRuntimeAccess::CancelCurrent(); return; }
					Output->SetPending(Gather());
				}, [Output, Discard = std::forward<D>(Discard)](ETaskState Terminal) mutable {
					Discard();
					Output->Complete(Terminal);
				}, Edge, ETaskDependencyKind::Completion, ResultBytes<Out>());
			if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			Output->BindProducer(Handle);
			return FTaskAccess::Make<Out>(std::move(Handle), std::move(Output), std::move(Failure));
		}
	}

	template<typename T>
	auto WhenAll(std::vector<TTask<T>>&& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::vector<TTaskValue<T>>>
	{
		using FValue = TTaskValue<T>;
		using FOut = std::vector<FValue>;
		try
		{
			std::vector<FTaskHandle> Handles;
			std::vector<std::shared_ptr<TUniqueTaskResultState<FValue>>> States;
			std::vector<Detail::TClaimLease<FValue>> Claims;
			Handles.reserve(Inputs.size()); States.reserve(Inputs.size()); Claims.reserve(Inputs.size());
			uint64 Bytes = 0;
			for (auto& Input : Inputs)
			{
				if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
				auto& Native = Input;
				auto Handle = FTaskHandle(Native);
				if (std::ranges::any_of(Handles, [&](const auto& Existing) { return Existing.GetTaskId() == Handle.GetTaskId(); }))
					Detail::FailConstruction({ETaskAdmissionErrorCode::UniqueConsumerClaimed, Handle.GetTaskId()});
				auto State = Detail::FTaskAccess::GetResultState(Native);
				if (State->GetEstimatedResultBytes() > std::numeric_limits<uint64>::max() - Bytes)
					Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
				Bytes += State->GetEstimatedResultBytes();
				Handles.emplace_back(std::move(Handle)); States.emplace_back(State); Claims.emplace_back(State);
				if (!Claims.back().Token) Detail::FailConstruction({ETaskAdmissionErrorCode::UniqueConsumerClaimed});
			}
			auto Result = Detail::BuildFanIn<FOut>(Handles, Executor, Options, Bytes,
				[States] {
					FOut Values; Values.reserve(States.size());
					for (const auto& State : States) { auto Value = State->TakePublished(); require(Value); Values.emplace_back(std::move(*Value)); }
					return Values;
				}, [States] { for (const auto& State : States) State->Discard(); });
			for (size_t Index = 0; Index < Inputs.size(); ++Index)
			{
				Claims[Index].Commit();
				Detail::FTaskAccess::InvalidateAfterClaim(Inputs[Index]);
			}
			return Result;
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	template<typename... Ts>
	auto WhenAll(std::tuple<TTask<Ts>...>&& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::tuple<TTaskValue<Ts>...>>
	{
		using FOut = std::tuple<TTaskValue<Ts>...>;
		try
		{
			std::vector<FTaskHandle> Handles;
			Handles.reserve(sizeof...(Ts));
			std::tuple<std::optional<Detail::TClaimLease<TTaskValue<Ts>>>...> Claims;
			std::tuple<std::shared_ptr<TUniqueTaskResultState<TTaskValue<Ts>>>...> States;
			std::optional<FTaskAdmissionError> Error;
			uint64 Bytes = 0;
			[&]<size_t... Indices>(std::index_sequence<Indices...>) {
				auto Reserve = [&]<size_t Index>() {
					if (Error) return;
					auto& Input = std::get<Index>(Inputs);
					if (!Input.IsValid()) { Error = {ETaskAdmissionErrorCode::InvalidPrerequisite}; return; }
					auto& Native = Input;
					const auto Handle = FTaskHandle(Native);
					if (std::ranges::any_of(Handles, [&](const auto& Other) { return Other.GetTaskId() == Handle.GetTaskId(); }))
					{ Error = {ETaskAdmissionErrorCode::UniqueConsumerClaimed, Handle.GetTaskId()}; return; }
					auto State = Detail::FTaskAccess::GetResultState(Native);
					if (State->GetEstimatedResultBytes() > std::numeric_limits<uint64>::max() - Bytes)
					{ Error = {ETaskAdmissionErrorCode::InvalidPayloadDeclaration}; return; }
					Bytes += State->GetEstimatedResultBytes();
					Handles.emplace_back(Handle);
					std::get<Index>(States) = State;
					std::get<Index>(Claims).emplace(State);
					if (!std::get<Index>(Claims)->Token) Error = {ETaskAdmissionErrorCode::UniqueConsumerClaimed};
				};
				(Reserve.template operator()<Indices>(), ...);
			}(std::index_sequence_for<Ts...>{});
			if (Error) Detail::FailConstruction(*Error);
			auto Result = Detail::BuildFanIn<FOut>(Handles, Executor, Options, Bytes,
				[States] { return std::apply([](const auto&... State) { return FOut(std::move(*State->TakePublished())...); }, States); },
				[States] { std::apply([](const auto&... State) { (State->Discard(), ...); }, States); });
			std::apply([](auto&... Claim) { (Claim->Commit(), ...); }, Claims);
			std::apply([](auto&... Input) { (Detail::FTaskAccess::InvalidateAfterClaim(Input), ...); }, Inputs);
			return Result;
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	namespace Detail
	{
		template<typename T, typename F> struct TSharedThenResult { using Type = std::invoke_result_t<F&, const T&>; };
		template<typename F> struct TSharedThenResult<void, F> { using Type = std::invoke_result_t<F&>; };
		template<typename T> struct TSharedInnerTask;
		template<typename T> struct TSharedInnerTask<TSharedTask<T>> { using Type = T; };
	}

	template<typename T, typename F, typename U = typename Detail::TSharedThenResult<T, std::decay_t<F>>::Type>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto Then(const TSharedTask<T>& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});

		try
		{
			std::vector<FTaskHandle> Handles{FTaskHandle(Input)};
			auto Result = Detail::BuildFanIn<TTaskValue<U>>(Handles, Executor, Options,
				Detail::FTaskAccess::SharedStorage(Input)->GetEstimatedResultBytes(),
				[Input, Function = std::forward<F>(Function)]() mutable -> TTaskValue<U> {
					auto Value = Input.GetResultShared();
					require(Value);
					auto Invoke = [&]() -> U {
						if constexpr (std::is_void_v<T>) return std::invoke(Function);
						else return std::invoke(Function, *Value);
					};
					if constexpr (std::is_void_v<U>) { Invoke(); return {}; }
					else return Invoke();
				}, [] {});
			return Detail::FTaskAccess::Rebind<U>(std::move(Result));
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	// Each duplicate shared position retains its own immutable owner in input order.
	template<typename T>
	auto WhenAll(const std::vector<TSharedTask<T>>& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::vector<std::shared_ptr<const TTaskValue<T>>>>
	{
		using FOut = std::vector<std::shared_ptr<const TTaskValue<T>>>;
		try
		{
			std::vector<FTaskHandle> Handles;
			Handles.reserve(Inputs.size());
			uint64 Bytes = 0;
			for (const auto& Input : Inputs)
			{
				if (!Input.IsValid()) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPrerequisite});
				const uint64 InputBytes = Detail::FTaskAccess::SharedStorage(Input)->GetEstimatedResultBytes();
				if (InputBytes > std::numeric_limits<uint64>::max() - Bytes) Detail::FailConstruction({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
				Bytes += InputBytes;
				Handles.emplace_back(FTaskHandle(Input));
			}
			return Detail::BuildFanIn<FOut>(Handles, Executor, Options, Bytes, [Inputs] {
				FOut Values; Values.reserve(Inputs.size());
				for (const auto& Input : Inputs) Values.emplace_back(Input.GetResultShared());
				return Values;
			}, [] {});
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	// A shared inner result yields an immutable owning view; cancellation is local to the observer.
	template<typename T, typename F, typename R = typename Detail::TThenResult<T, std::decay_t<F>>::Type,
		typename U = typename Detail::TSharedInnerTask<R>::Type>
	requires std::same_as<R, TSharedTask<U>>
	auto ThenAsync(TTask<T>&& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<std::shared_ptr<const TTaskValue<U>>>
	{
		FTaskExecutionOptions ObserverOptions = Options;
		return ThenAsync(std::move(Input), Executor, ObserverOptions,
			[Function = std::forward<F>(Function), ObserverOptions](auto&&... Args) mutable
				requires std::is_invocable_v<std::decay_t<F>&, decltype(Args)...> {
				auto Shared = std::invoke(Function, std::forward<decltype(Args)>(Args)...);
				if constexpr (std::is_void_v<U>)
					return Then(Shared, ETaskExecutor::Worker, ObserverOptions, [Shared] { return Shared.GetResultShared(); });
				else return Then(Shared, ETaskExecutor::Worker, ObserverOptions, [Shared](const U&) { return Shared.GetResultShared(); });
			});
	}

}
