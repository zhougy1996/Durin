#pragma once

#include "Threading/Task.h"

namespace Durin::Tasks
{
	namespace Detail
	{
		struct FCompletionSourceAccess;
		inline auto CreateGroupScope() -> FTaskScope
		{
			try { return CreateTaskScope(); }
			catch (const std::bad_alloc&) { requiref(false, "Task group allocation failed."); std::terminate(); }
		}
		// Construction failures are programming/lifetime violations or allocation failure.
		// This is never used to turn ordinary scheduler saturation into an assertion.
		template<typename T> struct TConstruction
		{
			[[noreturn]] static auto Failure(FTaskAdmissionError Error) -> T
			{
				requiref(false, "Task construction failed (code {}, task {}).", static_cast<uint32>(Error.Code), Error.RelatedTaskId);
				std::terminate();
			}
			static auto Success(T Value) -> T { return Value; }
		};
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
	template<typename T> using TTaskValue = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

	// Execution and owner identity; scheduler storage accounting is internal.
	struct FTaskExecutionOptions
	{
		const char* DebugName = "Task";
		ETaskPriority Priority = ETaskPriority::Normal;
		FTaskCancellationToken Cancellation;
		FTaskAttribution Attribution;
		FTaskScopeToken Scope;
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
		FTaskScopeToken Scope;
	};
	inline auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function,
		const FParallelForPolicyOptions& Options = {}) -> FParallelForResult
	{
		FParallelForOptions Native;
		Native.CancellationToken = Options.Cancellation;
		Native.Attribution = Options.Attribution;
		Native.Scope = Options.Scope;
		switch (Options.Policy)
		{
		case EParallelForPolicy::Auto:
			if (Num >= 16'384) Native.MinBatchSize = 2048;
			break;
		case EParallelForPolicy::Serial: break;
		case EParallelForPolicy::ExplicitBatch:
			if (Options.BatchSize == 0) return {ETaskState::Invalid, "ExplicitBatch requires a positive batch size.", 0};
			Native.MinBatchSize = Options.BatchSize;
			break;
		default: return {ETaskState::Invalid, "Unknown ParallelFor policy.", 0};
		}
		return Durin::ParallelFor(Name, Num, std::move(Function), Native);
	}

	// Non-consuming observation carries no access to the unique result.
	class FTaskCompletion
	{
	public:
		FTaskCompletion() = default;
		explicit FTaskCompletion(FTaskHandle InHandle) : Handle(std::move(InHandle)) {}
		explicit FTaskCompletion(FTaskScopeToken InGroup) : Group(std::move(InGroup)) {}
		auto IsValid() const -> bool { return Handle.IsValid() || !(Group == FTaskScopeToken{}); }
		auto IsReady() const -> bool { const auto State = GetState(); return State == ETaskState::Succeeded || State == ETaskState::Failed || State == ETaskState::Canceled; }
		auto GetState() const -> ETaskState { return Handle.IsValid() ? Handle.GetState() : Durin::Private::FTaskRuntimeAccess::GroupState(Group); }
		auto GetGroupToken() const -> const FTaskScopeToken& { return Group; }
		auto GetTaskHandle() const -> const FTaskHandle& { return Handle; }
	private:
		FTaskHandle Handle;
		FTaskScopeToken Group;
	};
	inline auto Wait(const FTaskCompletion& Completion) -> FTaskWaitResult { return Completion.GetTaskHandle().IsValid() ? WaitTask(Completion.GetTaskHandle()) : Durin::Private::FTaskRuntimeAccess::WaitGroup(Completion.GetGroupToken()); }
	inline auto Cancel(const FTaskCompletion& Completion) -> bool { return Completion.GetTaskHandle().IsValid() ? CancelTask(Completion.GetTaskHandle()) : Durin::Private::FTaskRuntimeAccess::CloseGroup(Completion.GetGroupToken(), ETaskScopeCloseMode::Cancel) == ETaskScopeCloseResult::Closed; }

	// Owns admission lifetime; owners explicitly close and join before destroying captures.
	class FTaskGroup
	{
	public:
		FTaskGroup() : Scope(Detail::CreateGroupScope()) { requiref(Scope.IsValid(), "Task group requires a running scheduler."); }
		// Borrowed module scope retains the module owner's stronger drain authority.
		explicit FTaskGroup(FTaskScopeToken InScope) : BorrowedScope(std::move(InScope)) {}
		~FTaskGroup() { if (Scope.IsValid()) Durin::Private::FTaskRuntimeAccess::DiagnoseGroupDestruction(Scope.GetToken()); }
		FTaskGroup(FTaskGroup&&) noexcept = default;
		FTaskGroup(const FTaskGroup&) = delete;
		auto operator=(const FTaskGroup&) -> FTaskGroup& = delete;
		auto IsValid() const -> bool { return !(GetToken() == FTaskScopeToken{}); }
		auto GetToken() const -> FTaskScopeToken { return Scope.IsValid() ? Scope.GetToken() : BorrowedScope; }
		auto Close(ETaskScopeCloseMode Mode = ETaskScopeCloseMode::Drain) -> ETaskScopeCloseResult { return Durin::Private::FTaskRuntimeAccess::CloseGroup(GetToken(), Mode); }
		auto GetDiagnostics() const -> FTaskScopeDiagnostics { return Durin::Private::FTaskRuntimeAccess::GroupDiagnostics(GetToken()); }
		auto WaitFor(double Seconds) -> ETaskScopeWaitResult { return Durin::Private::FTaskRuntimeAccess::WaitGroupFor(GetToken(), Seconds); }
		auto JoinAsync() const -> FTaskCompletion { return FTaskCompletion(GetToken()); }
	private:
		FTaskScope Scope;
		FTaskScopeToken BorrowedScope;
	};

	// Invocation-scoped authority cannot be copied into later asynchronous callbacks.
	class FTaskContext
	{
	public:
		FTaskContext(const FTaskCancellationToken& InCancellation, FTaskScopeToken InScope)
			: Cancellation(InCancellation), Scope(std::move(InScope)), ParentTaskId(Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId()) {}
		FTaskContext(const FTaskContext&) = delete;
		FTaskContext(FTaskContext&&) = delete;
		auto GetCancellationToken() const -> const FTaskCancellationToken& { return Cancellation; }
		template<typename F> auto LaunchChild(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function);
	private:
		FTaskCancellationToken Cancellation;
		FTaskScopeToken Scope;
		uint64 ParentTaskId = 0;
	};

	namespace Detail { struct FTaskAccess; }
	template<typename T> class TSharedTask;

	// Move-only result ownership; status observation alone never prolongs a unique claim.
	template<typename T>
	class TTask
	{
	public:
		TTask() = default;
		TTask(TTask&&) noexcept = default;
		auto operator=(TTask&&) noexcept -> TTask& = default;
		TTask(const TTask&) = delete;
		auto operator=(const TTask&) -> TTask& = delete;
		auto IsValid() const -> bool { return Handle.IsValid(); }
		auto GetCompletion() const -> FTaskCompletion { return FTaskCompletion(Handle.GetTaskHandle()); }
		auto IsCompleted() const -> bool { return GetCompletion().IsReady(); }
		auto GetDiagnostics() const -> FTaskDiagnostics { return GetCompletion().GetTaskHandle().GetDiagnostics(); }
		auto GetDiagnostic() const -> std::string { return GetCompletion().GetTaskHandle().GetDiagnostic(); }
		auto GetState() const -> ETaskState { return GetCompletion().GetState(); }
		auto Wait() const -> FTaskWaitResult { return Tasks::Wait(GetCompletion()); }
		// Requires a failed terminal state; returns a copy independent of result ownership.
		auto GetFailure() const -> FTaskFailure
		{
			require(GetState() == ETaskState::Failed);
			return Failure ? *Failure : FTaskFailure{Handle.GetDiagnostics().TerminalReason};
		}
		// Waits for success. The reference is invalidated by result consumption or task destruction.
		auto GetResult() const -> std::conditional_t<std::is_void_v<T>, void, const TTaskValue<T>&>
		{
			const auto WaitResult = Wait();
			require(WaitResult.WaitStatus == ETaskWaitStatus::Completed && GetState() == ETaskState::Succeeded);
			if constexpr (!std::is_void_v<T>) return *Durin::Private::FUniqueTaskAccess::GetResultState(Handle)->PeekPublished();
		}
		// Waits for success, then transfers the result exactly once and invalidates this task.
		// Rejected waits, failure and cancellation must be handled before requesting a result.
		auto TakeResult() && -> T
		{
			const auto WaitResult = Wait();
			require(WaitResult.WaitStatus == ETaskWaitStatus::Completed && GetState() == ETaskState::Succeeded);
			const auto ProducerLifetime = Handle.GetTaskHandle();
			auto Storage = Durin::Private::FUniqueTaskAccess::GetResultState(Handle);
			const auto Claim = Storage->ReserveClaim();
			require(Claim != 0);
			Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Handle);
			auto Value = Storage->TakePublished();
			require(Value);
			if constexpr (!std::is_void_v<T>) return std::move(*Value);
		}

	private:
		friend struct Detail::FTaskAccess;
		TUniqueTaskHandle<TTaskValue<T>> Handle;
		std::shared_ptr<FTaskFailure> Failure;
	};

	// Explicit immutable fan-out shares the result storage without a consuming alias.
	template<typename T>
	class TSharedTask
	{
	public:
		auto GetCompletion() const -> FTaskCompletion { return FTaskCompletion(Handle); }
		auto GetResultShared() const -> std::shared_ptr<const TTaskValue<T>>
		{
			if (!Handle.IsComplete() || Handle.GetState() != ETaskState::Succeeded) return {};
			return std::shared_ptr<const TTaskValue<T>>(LifetimePin, Storage->PeekPublished());
		}
		auto IsValid() const -> bool { return Handle.IsValid(); }
		auto IsCompleted() const -> bool { return GetCompletion().IsReady(); }
		auto GetDiagnostics() const -> FTaskDiagnostics { return GetCompletion().GetTaskHandle().GetDiagnostics(); }
		auto GetDiagnostic() const -> std::string { return GetCompletion().GetTaskHandle().GetDiagnostic(); }
		auto GetState() const -> ETaskState { return Handle.GetState(); }
		auto Wait() const -> FTaskWaitResult { return Tasks::Wait(GetCompletion()); }
		// Requires a failed terminal state; preserves the producer's failure identity.
		auto GetFailure() const -> FTaskFailure
		{
			require(GetState() == ETaskState::Failed);
			return Failure ? *Failure : FTaskFailure{ETaskTerminalReason::DependencyFailed};
		}
		// Waits for success. The immutable reference is valid while this shared task is retained.
		// Use GetResultShared when the result must outlive the task handle.
		auto GetResult() const -> std::conditional_t<std::is_void_v<T>, void, const TTaskValue<T>&>
		{
			const auto WaitResult = Wait();
			require(WaitResult.WaitStatus == ETaskWaitStatus::Completed && GetState() == ETaskState::Succeeded);
			if constexpr (!std::is_void_v<T>) return *Storage->PeekPublished();
		}

	private:
		friend struct Detail::FTaskAccess;
		FTaskHandle Handle;
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
			template<typename To, typename From> static auto Rebind(TTask<From>&& Input) -> TTask<To>
			{
				static_assert(std::same_as<TTaskValue<To>, TTaskValue<From>>);
				TTask<To> Output;
				Output.Handle = std::move(Input.Handle); Output.Failure = std::move(Input.Failure);
				return Output;
			}
			template<typename T> static auto SharedStorage(const TSharedTask<T>& Input) -> const auto& { return Input.Storage; }
			template<typename T> static auto Failure(TTask<T>& Task) -> auto& { return Task.Failure; }
			template<typename T> static auto Native(TTask<T>& Task) -> auto& { return Task.Handle; }
			template<typename T> static auto Make(FTaskHandle Handle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> State, std::shared_ptr<FTaskFailure> Failure = {}) -> TTask<T>
			{
				TTask<T> Task;
				Task.Failure = std::move(Failure);
				Task.Handle = Durin::Private::FTaskHandleFactory::MakeUnique(std::move(Handle), std::move(State));
				return Task;
			}
			template<typename T> static auto MakeShared(FTaskHandle Handle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> State, std::shared_ptr<FTaskFailure> Failure = {}) -> TSharedTask<T>
			{
				TSharedTask<T> Task;
				Task.Failure = std::move(Failure);
				Task.LifetimePin = std::make_shared<std::pair<FTaskHandle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>>>>(Handle, State);
				Task.Handle = std::move(Handle);
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
	auto LaunchTaskImpl(FTaskScopeToken Scope, uint64 ParentTaskId, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<T>
	{
		using FAdmission = Detail::TConstruction<TTask<T>>;
		const uint64 Bytes = Detail::ResultBytes<T>();

		try
		{
			auto State = std::make_shared<TUniqueTaskResultState<TTaskValue<T>>>(Bytes);
			FTaskLaunchOptions Launch;
			Launch.Scope = Scope;
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
				[State, Scope = Scope, Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
					FTaskContext Context(Token, Scope);
					auto Invoke = [&]() -> T {
						if constexpr (std::is_invocable_v<std::decay_t<F>&, FTaskContext&>) return std::invoke(Function, Context);
						else if constexpr (std::is_invocable_v<std::decay_t<F>&, const FTaskCancellationToken&>) return std::invoke(Function, Token);
						else return std::invoke(Function);
					};
					if constexpr (std::is_void_v<T>) { Invoke(); State->SetPending(std::monostate{}); }
					else State->SetPending(Invoke());
				}, [State](ETaskState Terminal) { State->Complete(Terminal); }, Launch, Bytes);
			if (!Admission.HasValue()) return FAdmission::Failure(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			State->BindProducer(Handle);
			return FAdmission::Success(Detail::FTaskAccess::Make<T>(std::move(Handle), std::move(State)));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	}

	template<typename F>
	auto LaunchTask(FTaskGroup& Group, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
	{
		require(Group.IsValid());
		require(Options.Scope == FTaskScopeToken{} || Options.Scope == Group.GetToken());
		return Detail::LaunchTaskImpl(Group.GetToken(), 0, Executor, Options, std::forward<F>(Function));
	}

	// Worker root in the scheduler lifetime or an explicitly borrowed owner scope.
	// Owners must stop submission before closing their scope or the scheduler.
	template<typename F>
	auto LaunchTask(const char* Name, F&& Function, FTaskExecutionOptions Options = {})
	{
		Options.DebugName = Name;
		return Detail::LaunchTaskImpl(Options.Scope, 0, ETaskExecutor::Worker, Options, std::forward<F>(Function));
	}

	// A completion prerequisite contributes no result ownership to this success edge.
	template<typename F>
	auto Then(const FTaskCompletion& Input, ETaskExecutor Executor, FTaskExecutionOptions Options, F&& Function)
	{
		try
		{
			const auto& Handle = Input.GetTaskHandle();
			require(Handle.IsValid());
			const auto Scope = Durin::Private::FTaskRuntimeAccess::GetScope(Handle);
			if (Options.Attribution == FTaskAttribution{})
				Options.Attribution = Durin::Private::FTaskRuntimeAccess::GetAttribution(Handle);
			require(Options.Scope == FTaskScopeToken{} || Options.Scope == Scope);
			std::vector<FTaskHandle> Prerequisites(Options.Prerequisites.begin(), Options.Prerequisites.end());
			Prerequisites.push_back(Handle);
			Options.Prerequisites = Prerequisites;
			return Detail::LaunchTaskImpl(Scope, Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId(),
				Executor, Options, std::forward<F>(Function));
		}
		catch (const std::bad_alloc&)
		{
			using FResult = decltype(Detail::LaunchTaskImpl(FTaskScopeToken{}, 0, Executor, Options, std::forward<F>(Function)));
			return Detail::TConstruction<FResult>::Failure({ETaskAdmissionErrorCode::CapacityExhausted});
		}
	}

	template<typename F>
	auto FTaskContext::LaunchChild(ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
	{
		using FAdmission = Detail::TConstruction<decltype(Detail::LaunchTaskImpl(Scope, ParentTaskId, Executor, Options, std::forward<F>(Function)))>;
		if (!ParentTaskId || ParentTaskId != Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId())
			return FAdmission::Failure({ETaskAdmissionErrorCode::GroupClosed});
		return Detail::LaunchTaskImpl(Scope, ParentTaskId, Executor, Options, std::forward<F>(Function));
	}

	// Unique claims remain transactional through native graph registration.
	template<typename T, typename F, typename U = typename Detail::TThenResult<T, std::decay_t<F>>::Type>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto Then(TTask<T>&& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		using FAdmission = Detail::TConstruction<TTask<U>>;
		auto& Native = Detail::FTaskAccess::Native(Input);
		if (!Input.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
		auto Source = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
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
		Edge.Scope = Options.Scope;
		Edge.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
		if (Executor == ETaskExecutor::GameThreadDeferred)
		{
			if (Edge.EstimatedPayloadBytes > std::numeric_limits<uint64>::max() - Source->GetEstimatedResultBytes())
				return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
			Edge.EstimatedPayloadBytes += Source->GetEstimatedResultBytes();
		}
		const auto Claim = Source->ReserveClaim();
		if (!Claim) return FAdmission::Failure({ETaskAdmissionErrorCode::UniqueConsumerClaimed});
		try
		{
			auto Failure = std::make_shared<FTaskFailure>();
			const auto Predecessor = Native.GetTaskHandle();
			auto SourceFailure = Detail::FTaskAccess::Failure(Input);
			auto Output = std::make_shared<TUniqueTaskResultState<TTaskValue<U>>>(Bytes);
			auto Admission = Durin::Private::TryLaunchContinuationTask(Native.GetTaskHandle(), Options.DebugName,
				[Source, SourceFailure, Failure, Predecessor, Output, Scope = Durin::Private::FTaskRuntimeAccess::GetScope(Predecessor), Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
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
					FTaskContext Context(Token, Scope);
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
				return FAdmission::Failure(Admission.GetError());
			}
			auto Handle = std::move(Admission).TakeValue();
			Source->CommitClaim(Claim, Handle, false);
			Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Native);
			Output->BindProducer(Handle);
			return FAdmission::Success(Detail::FTaskAccess::Make<U>(std::move(Handle), std::move(Output), std::move(Failure)));
		}
		catch (const std::bad_alloc&)
		{
			Source->RollbackClaim(Claim);
			return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted});
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
		using FAdmission = Detail::TConstruction<TSharedTask<T>>;
		if (!Input.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
		auto& Native = Detail::FTaskAccess::Native(Input);
		auto State = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
		try
		{
			auto Shared = Detail::FTaskAccess::MakeShared<T>(Native.GetTaskHandle(), State, Detail::FTaskAccess::Failure(Input));
			if (!State->ReserveClaim()) return FAdmission::Failure({ETaskAdmissionErrorCode::UniqueConsumerClaimed});
			Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Native);
			return FAdmission::Success(std::move(Shared));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	// Observes a completed task in any terminal state; admission/cancellation can suppress this edge.
	template<typename T, typename F, typename U = std::invoke_result_t<std::decay_t<F>&, const TSharedTask<T>&>>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto ThenCompleted(const TSharedTask<T>& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTask<U>
	{
		using FAdmission = Detail::TConstruction<TTask<U>>;
		const auto Predecessor = Input.GetCompletion().GetTaskHandle();
		if (!Predecessor.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
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
		Edge.Scope = Options.Scope;
		Edge.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
		if (Executor == ETaskExecutor::GameThreadDeferred)
		{
			const uint64 InputBytes = Detail::FTaskAccess::SharedStorage(Input)->GetEstimatedResultBytes();
			if (InputBytes > std::numeric_limits<uint64>::max() - Edge.EstimatedPayloadBytes)
				return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
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
			if (!Admission.HasValue()) return FAdmission::Failure(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			Output->BindProducer(Handle);
			return FAdmission::Success(Detail::FTaskAccess::Make<U>(std::move(Handle), std::move(Output), std::move(Failure)));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
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
		// Unknown requirements conservatively reject GameThread waiting until a dynamic edge is bound.
		static auto Create(FTaskGroup& Group, const FTaskExecutionOptions& Options)
			-> TCompletionSource
		{
			if (!Group.IsValid()) return Detail::TConstruction<TCompletionSource>::Failure({ETaskAdmissionErrorCode::GroupClosed});
			require(Options.Scope == FTaskScopeToken{} || Options.Scope == Group.GetToken());
			return CreateInternal(Group.GetToken(), Options, true);
		}
		private:
		friend struct Detail::FCompletionSourceAccess;
		static auto CreateInternal(FTaskScopeToken Scope, const FTaskExecutionOptions& Options, bool bUnknownRequirements)
			-> TCompletionSource
		{
			using FAdmission = Detail::TConstruction<TCompletionSource>;
			try
			{
				const uint64 Bytes = Detail::ResultBytes<T>();

				auto Source = std::make_shared<FSource>();
				Source->Storage = std::make_shared<TUniqueTaskResultState<TTaskValue<T>>>(Bytes);
				FTaskLaunchOptions Launch;
				Launch.Scope = std::move(Scope);
				Launch.Attribution = Options.Attribution;
				Launch.CancellationToken = Options.Cancellation;
				Launch.ExpectedParentTaskId = Durin::Private::FTaskRuntimeAccess::GetCurrentTaskId();
				Launch.bExternalCompletion = true;
				Launch.bQueueOnSaturation = true;
				Launch.bUnknownExecutionRequirement = bUnknownRequirements;
				auto Admission = Durin::Private::TryLaunchCancelableTaskWithCompletion(Options.DebugName,
					[](const FTaskCancellationToken&) {},
					[Storage = Source->Storage](ETaskState Terminal) { Storage->Complete(Terminal); }, Launch, Bytes);
				if (!Admission.HasValue()) return FAdmission::Failure(Admission.GetError());
				Source->Handle = std::move(Admission).TakeValue();
				Source->Storage->BindProducer(Source->Handle);
				return FAdmission::Success(TCompletionSource(std::move(Source)));
			}
			catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
		}
	public:
		auto TakeTask() -> TTask<T>
		{
			require(Source && !Source->bTaskTaken.exchange(true));
			return Detail::FTaskAccess::Make<T>(Source->Handle, Source->Storage, Source->Failure);
		}
		auto GetCompletion() const -> FTaskCompletion { return FTaskCompletion(Source->Handle); }
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
			static auto Create(FTaskScopeToken Scope, const FTaskExecutionOptions& Options, bool bUnknownRequirements = true)
				-> TCompletionSource<T>
			{ return TCompletionSource<T>::CreateInternal(Scope, Options, bUnknownRequirements); }
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
		using FAdmission = Detail::TConstruction<TTask<U>>;
		if (!Input.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
		try
		{
			const auto Predecessor = Input.GetCompletion().GetTaskHandle();
			auto ExternalAdmission = Detail::FCompletionSourceAccess::Create<U>(Durin::Private::FTaskRuntimeAccess::GetScope(Predecessor), Options);
			auto Source = std::move(ExternalAdmission);
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
				if (Durin::Private::FTaskRuntimeAccess::IsCancellationRequested(Source.GetCompletion().GetTaskHandle()))
				{ Source.TrySetCanceled(); return; }
				auto Returned = std::invoke(Function, std::forward<decltype(Args)>(Args)...);
				TTask<U> Inner;
				Inner = std::move(Returned);
				if (auto Error = Durin::Private::FTaskRuntimeAccess::BindDynamicDependency(
					Source.GetCompletion().GetTaskHandle(), Inner.GetCompletion().GetTaskHandle()))
				{
					requiref(Error->Code != ETaskAdmissionErrorCode::CapacityExhausted, "Dynamic task dependency allocation failed.");
					Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, *Error, ETaskFailureCode::DependencyBindingFailed});
					return;
				}
				auto& Native = Detail::FTaskAccess::Native(Inner);
				Binding->Storage = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
				Binding->Failure = Detail::FTaskAccess::Failure(Inner);
				if (!Binding->Storage->ReserveClaim())
				{
					Source.TrySetFailure({ETaskTerminalReason::CallbackFailure,
						FTaskAdmissionError{ETaskAdmissionErrorCode::UniqueConsumerClaimed}, ETaskFailureCode::DependencyBindingFailed});
					return;
				}
				const auto Handle = Native.GetTaskHandle();
				Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Native);
				Durin::Private::FTaskRuntimeAccess::BindTerminal(Handle, std::move(InnerHook));
			};
			auto RunnerAdmission = Then(std::move(Input), Executor, Options, std::move(InvokeInner));
			auto Runner = std::move(RunnerAdmission);
			Durin::Private::FTaskRuntimeAccess::BindTerminal(Runner.GetCompletion().GetTaskHandle(), std::move(RunnerHook));
			return FAdmission::Success(std::move(Output));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
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
			auto Commit(const FTaskHandle& Handle) -> void { const bool bCommitted = Storage->CommitClaim(Token, Handle, false); require(bCommitted); Token = 0; }
		};

		template<typename Out, typename F, typename D>
		auto BuildFanIn(const std::vector<FTaskHandle>& Inputs, ETaskExecutor Executor, FTaskExecutionOptions Options,
			uint64 InputBytes, F&& Gather, D&& Discard) -> TTask<Out>
		{
			using FAdmission = Detail::TConstruction<TTask<Out>>;
			if (InputBytes > std::numeric_limits<uint64>::max() - sizeof(Out))
				return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
			if (Inputs.empty())
			{
				if (!Options.Prerequisites.empty())
					return LaunchTaskImpl(Options.Scope, 0, Executor, Options, std::forward<F>(Gather));
				auto Admission = FCompletionSourceAccess::Create<Out>(Options.Scope, Options, false);
				auto Source = std::move(Admission);
				auto Result = Source.TakeTask();
				Source.TrySetValue(Out{});
				return FAdmission::Success(std::move(Result));
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
			Edge.Scope = Options.Scope;
			Edge.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
			if (Executor == ETaskExecutor::GameThreadDeferred)
			{
				if (InputBytes > std::numeric_limits<uint64>::max() - Edge.EstimatedPayloadBytes)
					return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
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
			if (!Admission.HasValue()) return FAdmission::Failure(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			Output->BindProducer(Handle);
			return FAdmission::Success(FTaskAccess::Make<Out>(std::move(Handle), std::move(Output), std::move(Failure)));
		}
	}

	template<typename T>
	auto WhenAll(std::vector<TTask<T>>&& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::vector<TTaskValue<T>>>
	{
		using FValue = TTaskValue<T>;
		using FOut = std::vector<FValue>;
		using FAdmission = Detail::TConstruction<TTask<FOut>>;
		try
		{
			std::vector<FTaskHandle> Handles;
			std::vector<std::shared_ptr<TUniqueTaskResultState<FValue>>> States;
			std::vector<Detail::TClaimLease<FValue>> Claims;
			Handles.reserve(Inputs.size()); States.reserve(Inputs.size()); Claims.reserve(Inputs.size());
			uint64 Bytes = 0;
			for (auto& Input : Inputs)
			{
				if (!Input.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
				auto& Native = Detail::FTaskAccess::Native(Input);
				auto Handle = Native.GetTaskHandle();
				if (std::ranges::any_of(Handles, [&](const auto& Existing) { return Existing.GetTaskId() == Handle.GetTaskId(); }))
					return FAdmission::Failure({ETaskAdmissionErrorCode::UniqueConsumerClaimed, Handle.GetTaskId()});
				auto State = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
				if (State->GetEstimatedResultBytes() > std::numeric_limits<uint64>::max() - Bytes)
					return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
				Bytes += State->GetEstimatedResultBytes();
				Handles.emplace_back(std::move(Handle)); States.emplace_back(State); Claims.emplace_back(State);
				if (!Claims.back().Token) return FAdmission::Failure({ETaskAdmissionErrorCode::UniqueConsumerClaimed});
			}
			auto Admission = Detail::BuildFanIn<FOut>(Handles, Executor, Options, Bytes,
				[States] {
					FOut Values; Values.reserve(States.size());
					for (const auto& State : States) { auto Value = State->TakePublished(); require(Value); Values.emplace_back(std::move(*Value)); }
					return Values;
				}, [States] { for (const auto& State : States) State->Discard(); });
			auto Result = std::move(Admission);
			for (size_t Index = 0; Index < Inputs.size(); ++Index)
			{
				Claims[Index].Commit(Result.GetCompletion().GetTaskHandle());
				Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Detail::FTaskAccess::Native(Inputs[Index]));
			}
			return FAdmission::Success(std::move(Result));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	template<typename... Ts>
	auto WhenAll(std::tuple<TTask<Ts>...>&& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::tuple<TTaskValue<Ts>...>>
	{
		using FOut = std::tuple<TTaskValue<Ts>...>;
		using FAdmission = Detail::TConstruction<TTask<FOut>>;
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
					auto& Native = Detail::FTaskAccess::Native(Input);
					const auto Handle = Native.GetTaskHandle();
					if (std::ranges::any_of(Handles, [&](const auto& Other) { return Other.GetTaskId() == Handle.GetTaskId(); }))
					{ Error = {ETaskAdmissionErrorCode::UniqueConsumerClaimed, Handle.GetTaskId()}; return; }
					auto State = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
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
			if (Error) return FAdmission::Failure(*Error);
			auto Admission = Detail::BuildFanIn<FOut>(Handles, Executor, Options, Bytes,
				[States] { return std::apply([](const auto&... State) { return FOut(std::move(*State->TakePublished())...); }, States); },
				[States] { std::apply([](const auto&... State) { (State->Discard(), ...); }, States); });
			auto Result = std::move(Admission);
			std::apply([&](auto&... Claim) { (Claim->Commit(Result.GetCompletion().GetTaskHandle()), ...); }, Claims);
			std::apply([](auto&... Input) { (Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Detail::FTaskAccess::Native(Input)), ...); }, Inputs);
			return FAdmission::Success(std::move(Result));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
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
		using FAdmission = Detail::TConstruction<TTask<U>>;
		if (!Input.GetCompletion().IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});

		try
		{
			std::vector<FTaskHandle> Handles{Input.GetCompletion().GetTaskHandle()};
			auto Admission = Detail::BuildFanIn<TTaskValue<U>>(Handles, Executor, Options,
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
			return FAdmission::Success(Detail::FTaskAccess::Rebind<U>(std::move(Admission)));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	// Each duplicate shared position retains its own immutable owner in input order.
	template<typename T>
	auto WhenAll(const std::vector<TSharedTask<T>>& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTask<std::vector<std::shared_ptr<const TTaskValue<T>>>>
	{
		using FOut = std::vector<std::shared_ptr<const TTaskValue<T>>>;
		using FAdmission = Detail::TConstruction<TTask<FOut>>;
		try
		{
			std::vector<FTaskHandle> Handles;
			Handles.reserve(Inputs.size());
			uint64 Bytes = 0;
			for (const auto& Input : Inputs)
			{
				if (!Input.GetCompletion().IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
				const uint64 InputBytes = Detail::FTaskAccess::SharedStorage(Input)->GetEstimatedResultBytes();
				if (InputBytes > std::numeric_limits<uint64>::max() - Bytes) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
				Bytes += InputBytes;
				Handles.emplace_back(Input.GetCompletion().GetTaskHandle());
			}
			return Detail::BuildFanIn<FOut>(Handles, Executor, Options, Bytes, [Inputs] {
				FOut Values; Values.reserve(Inputs.size());
				for (const auto& Input : Inputs) Values.emplace_back(Input.GetResultShared());
				return Values;
			}, [] {});
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
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
