#pragma once

#include "Threading/Task.h"

namespace Durin::Tasks
{
	// Executor choice is shared by roots and every composition edge.
	enum class ETaskExecutor : uint8 { Worker, BlockingIO, GameThreadDeferred };

	// Framework failure is distinct from a successful value carrying a domain error.
	struct FTaskFailure
	{
		ETaskTerminalReason Reason = ETaskTerminalReason::CallbackFailure;
		std::optional<FTaskAdmissionError> AdmissionError;
	};
	struct FTaskCanceled {};
	template<typename T> using TTaskValue = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
	template<typename T> using TTaskOutcome = std::variant<TTaskValue<T>, FTaskFailure, FTaskCanceled>;

	// Estimates bound retained payload independently of the callable's inline storage.
	struct FTaskExecutionOptions
	{
		const char* DebugName = "Task";
		ETaskPriority Priority = ETaskPriority::Normal;
		FTaskAttribution Attribution;
		FTaskCancellationToken Cancellation;
		uint64 EstimatedCaptureBytes = 0;
		uint64 EstimatedResultBytes = 0;
	};

	// Non-consuming observation carries no access to the unique result.
	class FTaskCompletion
	{
	public:
		FTaskCompletion() = default;
		explicit FTaskCompletion(FTaskHandle InHandle) : Handle(std::move(InHandle)) {}
		auto IsValid() const -> bool { return Handle.IsValid(); }
		auto IsReady() const -> bool { return Handle.IsComplete(); }
		auto GetState() const -> ETaskState { return Handle.GetState(); }
		auto GetTaskHandle() const -> const FTaskHandle& { return Handle; }
	private:
		FTaskHandle Handle;
	};
	inline auto Wait(const FTaskCompletion& Completion) -> FTaskWaitResult { return WaitTask(Completion.GetTaskHandle()); }
	inline auto Cancel(const FTaskCompletion& Completion) -> bool { return CancelTask(Completion.GetTaskHandle()); }

	// Owns admission lifetime; owners explicitly close and join before destroying captures.
	class FTaskGroup
	{
	public:
		FTaskGroup() : Scope(CreateTaskScope()) {}
		FTaskGroup(const FTaskGroup&) = delete;
		auto operator=(const FTaskGroup&) -> FTaskGroup& = delete;
		auto IsValid() const -> bool { return Scope.IsValid(); }
		auto GetToken() const -> FTaskScopeToken { return Scope.GetToken(); }
		auto Close(ETaskScopeCloseMode Mode = ETaskScopeCloseMode::Drain) -> ETaskScopeCloseResult { return Scope.Close(Mode); }
		auto GetDiagnostics() const -> FTaskScopeDiagnostics { return Scope.GetDiagnostics(); }
		auto WaitFor(double Seconds) -> ETaskScopeWaitResult { return Scope.WaitFor(Seconds); }
	private:
		FTaskScope Scope;
	};

	// Invocation-scoped authority cannot be copied into later asynchronous callbacks.
	class FTaskContext
	{
	public:
		FTaskContext(const FTaskCancellationToken& InCancellation, FTaskScopeToken InScope)
			: Cancellation(InCancellation), Scope(std::move(InScope)) {}
		FTaskContext(const FTaskContext&) = delete;
		FTaskContext(FTaskContext&&) = delete;
		auto GetCancellationToken() const -> const FTaskCancellationToken& { return Cancellation; }
	private:
		FTaskCancellationToken Cancellation;
		FTaskScopeToken Scope;
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
		// The caller must first observe terminal completion; consumes exactly once.
		auto TakeOutcome() && -> TTaskOutcome<T>
		{
			require(Handle.IsValid() && Handle.IsComplete());
			auto Diagnostics = Handle.GetDiagnostics();
			auto Storage = Durin::Private::FUniqueTaskAccess::GetResultState(Handle);
			const auto Claim = Storage->ReserveClaim();
			require(Claim != 0);
			Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Handle);
			if (Diagnostics.State == ETaskState::Succeeded)
			{
				auto Value = Storage->TakePublished();
				require(Value);
				return TTaskOutcome<T>(std::in_place_index<0>, std::move(*Value));
			}
			if (Diagnostics.State == ETaskState::Failed || Diagnostics.TerminalReason == ETaskTerminalReason::DependencyFailed)
				return FTaskFailure{Diagnostics.TerminalReason};
			return FTaskCanceled{};
		}
	private:
		friend struct Detail::FTaskAccess;
		TUniqueTaskHandle<TTaskValue<T>> Handle;
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
			return std::shared_ptr<const TTaskValue<T>>(Storage, Storage->PeekPublished());
		}
	private:
		friend struct Detail::FTaskAccess;
		FTaskHandle Handle;
		std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> Storage;
	};

	namespace Detail
	{
		struct FTaskAccess
		{
			template<typename T> static auto Native(TTask<T>& Task) -> auto& { return Task.Handle; }
			template<typename T> static auto Make(FTaskHandle Handle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> State) -> TTask<T>
			{
				TTask<T> Task;
				Task.Handle = Durin::Private::FTaskHandleFactory::MakeUnique(std::move(Handle), std::move(State));
				return Task;
			}
			template<typename T> static auto MakeShared(FTaskHandle Handle, std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> State) -> TSharedTask<T>
			{
				TSharedTask<T> Task;
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
		template<typename F> struct TSpawnResult<F, false> { using Type = std::invoke_result_t<F&>; };
		template<typename T, typename F> struct TThenResult { using Type = std::invoke_result_t<F&, T&&>; };
		template<typename F> struct TThenResult<void, F> { using Type = std::invoke_result_t<F&>; };

		inline auto Target(ETaskExecutor Executor) -> ETaskTarget
		{
			if (Executor == ETaskExecutor::Worker) return ETaskTarget::AnyWorker;
			if (Executor == ETaskExecutor::GameThreadDeferred) return ETaskTarget::GameThreadDeferred;
			return static_cast<ETaskTarget>(255);
		}
		template<typename T>
		auto ResultBytes(const FTaskExecutionOptions& Options) -> uint64
		{
			if constexpr (std::is_void_v<T>) return 0;
			else if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>)
				return std::max<uint64>(sizeof(T), Options.EstimatedResultBytes);
			else return Options.EstimatedResultBytes;
		}
	}

	template<typename F, typename T = typename Detail::TSpawnResult<std::decay_t<F>>::Type>
	requires (!std::is_reference_v<T> && !Detail::TIsTask<T>::value)
	auto TrySpawn(FTaskGroup& Group, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTaskAdmission<TTask<T>>
	{
		using FAdmission = TTaskAdmission<TTask<T>>;
		if (!Group.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::GroupClosed});
		const uint64 Bytes = Detail::ResultBytes<T>(Options);
		if (!std::is_void_v<T> && Bytes == 0)
			return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
		try
		{
			auto State = std::make_shared<TUniqueTaskResultState<TTaskValue<T>>>(Bytes);
			FTaskLaunchOptions Launch;
			Launch.Scope = Group.GetToken();
			Launch.Attribution = Options.Attribution;
			Launch.CancellationToken = Options.Cancellation;
			Launch.Target = Detail::Target(Executor);
			Launch.Priority = Options.Priority;
			Launch.EstimatedPayloadBytes = Options.EstimatedCaptureBytes;
			Launch.bValidateConstruction = true;
			auto Admission = Durin::Private::TryLaunchCancelableTaskWithCompletion(Options.DebugName,
				[State, Scope = Group.GetToken(), Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
					FTaskContext Context(Token, Scope);
					auto Invoke = [&]() -> T {
						if constexpr (std::is_invocable_v<std::decay_t<F>&, FTaskContext&>) return std::invoke(Function, Context);
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

	// Claim rollback happens before returning a construction failure; the input stays usable.
	template<typename T, typename F, typename U = typename Detail::TThenResult<T, std::decay_t<F>>::Type>
	requires (!std::is_reference_v<U> && !Detail::TIsTask<U>::value)
	auto Then(TTask<T>&& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTaskAdmission<TTask<U>>
	{
		using FAdmission = TTaskAdmission<TTask<U>>;
		auto& Native = Detail::FTaskAccess::Native(Input);
		if (!Input.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
		auto Source = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
		uint64 Bytes = Detail::ResultBytes<U>(Options);
		if (!std::is_void_v<U> && Bytes == 0) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
		FTaskContinuationOptions Edge;
		Edge.Target = Detail::Target(Executor);
		Edge.Priority = Options.Priority;
		Edge.CancellationToken = Options.Cancellation;
		Edge.Attribution = Options.Attribution;
		Edge.EstimatedPayloadBytes = Options.EstimatedCaptureBytes;
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
			auto Output = std::make_shared<TUniqueTaskResultState<TTaskValue<U>>>(Bytes);
			auto Admission = Durin::Private::TryLaunchContinuationTask(Native.GetTaskHandle(), Options.DebugName,
				[Source, Output, Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable {
					auto Value = Source->TakePublished();
					require(Value);
					auto Invoke = [&]() -> U {
						if constexpr (std::is_void_v<T>) return std::invoke(Function);
						else return std::invoke(Function, std::move(*Value));
					};
					if constexpr (std::is_void_v<U>) { Invoke(); Output->SetPending(std::monostate{}); }
					else Output->SetPending(Invoke());
				}, [Source, Output](ETaskState Terminal) { Source->Discard(); Output->Complete(Terminal); },
				Edge, ETaskDependencyKind::Success, Bytes);
			if (!Admission.HasValue())
			{
				Source->RollbackClaim(Claim);
				return FAdmission::Failure(Admission.GetError());
			}
			auto Handle = std::move(Admission).TakeValue();
			Source->CommitClaim(Claim, Handle);
			Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Native);
			Output->BindProducer(Handle);
			return FAdmission::Success(Detail::FTaskAccess::Make<U>(std::move(Handle), std::move(Output)));
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
	auto Share(TTask<T>&& Input) -> TTaskAdmission<TSharedTask<T>>
	{
		using FAdmission = TTaskAdmission<TSharedTask<T>>;
		if (!Input.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
		auto& Native = Detail::FTaskAccess::Native(Input);
		auto State = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
		if (!State->ReserveClaim()) return FAdmission::Failure({ETaskAdmissionErrorCode::UniqueConsumerClaimed});
		auto Shared = Detail::FTaskAccess::MakeShared<T>(Native.GetTaskHandle(), State);
		Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Native);
		return FAdmission::Success(std::move(Shared));
	}
}
