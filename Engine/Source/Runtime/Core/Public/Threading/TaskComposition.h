#pragma once

#include "Threading/Task.h"

namespace Durin::Tasks
{
	// Executor choice is shared by roots and every composition edge.
	enum class ETaskExecutor : uint8 { Worker, BlockingIO, GameThreadDeferred };

	// Framework failure is distinct from a successful value carrying a domain error.
	enum class ETaskFailureCode : uint8 { CallableException, AdmissionRejected, AbandonedSource, DependencyFailed };
	struct FTaskFailure
	{
		ETaskTerminalReason Reason = ETaskTerminalReason::CallbackFailure;
		std::optional<FTaskAdmissionError> AdmissionError;
		ETaskFailureCode Code = ETaskFailureCode::CallableException;
		uint64 RelatedTaskId = 0;
		size_t InputIndex = 0;
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
				return Failure ? *Failure : FTaskFailure{Diagnostics.TerminalReason};
			return FTaskCanceled{};
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
			return std::shared_ptr<const TTaskValue<T>>(Storage, Storage->PeekPublished());
		}
	private:
		friend struct Detail::FTaskAccess;
		FTaskHandle Handle;
		std::shared_ptr<TUniqueTaskResultState<TTaskValue<T>>> Storage;
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
			auto Failure = std::make_shared<FTaskFailure>();
			const auto Predecessor = Native.GetTaskHandle();
			auto SourceFailure = Detail::FTaskAccess::Failure(Input);
			auto Output = std::make_shared<TUniqueTaskResultState<TTaskValue<U>>>(Bytes);
			auto Admission = Durin::Private::TryLaunchContinuationTask(Native.GetTaskHandle(), Options.DebugName,
				[Source, SourceFailure, Failure, Predecessor, Output, Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable {
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
					auto Invoke = [&]() -> U {
						if constexpr (std::is_void_v<T>) return std::invoke(Function);
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
			Source->CommitClaim(Claim, Handle);
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
		static auto TryCreate(FTaskGroup& Group, const FTaskExecutionOptions& Options, bool bUnknownRequirements = true)
			-> TTaskAdmission<TCompletionSource>
		{
			if (!Group.IsValid()) return TTaskAdmission<TCompletionSource>::Failure({ETaskAdmissionErrorCode::GroupClosed});
			return TryCreate(Group.GetToken(), Options, bUnknownRequirements);
		}
		static auto TryCreate(FTaskScopeToken Scope, const FTaskExecutionOptions& Options, bool bUnknownRequirements = true)
			-> TTaskAdmission<TCompletionSource>
		{
			using FAdmission = TTaskAdmission<TCompletionSource>;
			try
			{
				const uint64 Bytes = Detail::ResultBytes<T>(Options);
				if (!std::is_void_v<T> && Bytes == 0) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
				auto Source = std::make_shared<FSource>();
				Source->Storage = std::make_shared<TUniqueTaskResultState<TTaskValue<T>>>(Bytes);
				FTaskLaunchOptions Launch;
				Launch.Scope = std::move(Scope);
				Launch.Attribution = Options.Attribution;
				Launch.CancellationToken = Options.Cancellation;
				Launch.bExternalCompletion = true;
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
		template<typename T> struct TInnerTask;
		template<typename T> struct TInnerTask<TTask<T>> { using Type = T; static constexpr bool bAdmission = false; };
		template<typename T> struct TInnerTask<TTaskAdmission<TTask<T>>> { using Type = T; static constexpr bool bAdmission = true; };
	}

	// The outer node remains external/unknown until the callback binds its real inner dependency.
	template<typename T, typename F, typename R = typename Detail::TThenResult<T, std::decay_t<F>>::Type,
		typename U = typename Detail::TInnerTask<R>::Type>
	auto ThenAsync(TTask<T>&& Input, ETaskExecutor Executor, const FTaskExecutionOptions& Options, F&& Function)
		-> TTaskAdmission<TTask<U>>
	{
		using FAdmission = TTaskAdmission<TTask<U>>;
		if (!Input.IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
		try
		{
			const auto Predecessor = Input.GetCompletion().GetTaskHandle();
			auto ExternalAdmission = TCompletionSource<U>::TryCreate(Durin::Private::FTaskRuntimeAccess::GetScope(Predecessor), Options);
			if (!ExternalAdmission.HasValue()) return FAdmission::Failure(ExternalAdmission.GetError());
			auto Source = std::move(ExternalAdmission).TakeValue();
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
			auto InvokeInner = [Source, Binding, InnerHook, Function = std::forward<F>(Function)](auto&&... Args) mutable {
				if (Durin::Private::FTaskRuntimeAccess::IsCancellationRequested(Source.GetCompletion().GetTaskHandle()))
				{ Source.TrySetCanceled(); return; }
				auto Returned = std::invoke(Function, std::forward<decltype(Args)>(Args)...);
				TTask<U> Inner;
				if constexpr (Detail::TInnerTask<R>::bAdmission)
				{
					if (!Returned.HasValue())
					{
						Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, Returned.GetError(), ETaskFailureCode::AdmissionRejected});
						return;
					}
					Inner = std::move(Returned).TakeValue();
				}
				else Inner = std::move(Returned);
				if (auto Error = Durin::Private::FTaskRuntimeAccess::BindDynamicDependency(
					Source.GetCompletion().GetTaskHandle(), Inner.GetCompletion().GetTaskHandle()))
				{
					Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, *Error, ETaskFailureCode::AdmissionRejected});
					return;
				}
				auto& Native = Detail::FTaskAccess::Native(Inner);
				Binding->Storage = Durin::Private::FUniqueTaskAccess::GetResultState(Native);
				Binding->Failure = Detail::FTaskAccess::Failure(Inner);
				if (!Binding->Storage->ReserveClaim())
				{
					Source.TrySetFailure({ETaskTerminalReason::CallbackFailure,
						FTaskAdmissionError{ETaskAdmissionErrorCode::UniqueConsumerClaimed}, ETaskFailureCode::AdmissionRejected});
					return;
				}
				const auto Handle = Native.GetTaskHandle();
				Durin::Private::FUniqueTaskAccess::InvalidateAfterClaim(Native);
				Durin::Private::FTaskRuntimeAccess::BindTerminal(Handle, std::move(InnerHook));
			};
			auto RunnerAdmission = Then(std::move(Input), Executor, Options, std::move(InvokeInner));
			if (!RunnerAdmission.HasValue()) return FAdmission::Failure(RunnerAdmission.GetError());
			auto Runner = std::move(RunnerAdmission).TakeValue();
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
			auto Commit(const FTaskHandle& Handle) -> void { const bool bCommitted = Storage->CommitClaim(Token, Handle); require(bCommitted); Token = 0; }
		};

		template<typename Out, typename F, typename D>
		auto TryFanIn(const std::vector<FTaskHandle>& Inputs, ETaskExecutor Executor, FTaskExecutionOptions Options,
			uint64 InputBytes, F&& Gather, D&& Discard) -> TTaskAdmission<TTask<Out>>
		{
			using FAdmission = TTaskAdmission<TTask<Out>>;
			if (InputBytes > std::numeric_limits<uint64>::max() - sizeof(Out))
				return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
			Options.EstimatedResultBytes = std::max(Options.EstimatedResultBytes, InputBytes + sizeof(Out));
			if (Inputs.empty())
			{
				auto Admission = TCompletionSource<Out>::TryCreate(FTaskScopeToken{}, Options, false);
				if (!Admission.HasValue()) return FAdmission::Failure(Admission.GetError());
				auto Source = std::move(Admission).TakeValue();
				auto Result = Source.TakeTask();
				Source.TrySetValue(Out{});
				return FAdmission::Success(std::move(Result));
			}
			FTaskContinuationOptions Edge;
			Edge.Prerequisites = Inputs;
			Edge.Target = Target(Executor);
			Edge.Priority = Options.Priority;
			Edge.CancellationToken = Options.Cancellation;
			Edge.Attribution = Options.Attribution;
			Edge.EstimatedPayloadBytes = Options.EstimatedCaptureBytes;
			if (Executor == ETaskExecutor::GameThreadDeferred)
			{
				if (InputBytes > std::numeric_limits<uint64>::max() - Edge.EstimatedPayloadBytes)
					return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
				Edge.EstimatedPayloadBytes += InputBytes;
			}
			auto Failure = std::make_shared<FTaskFailure>();
			auto Output = std::make_shared<TUniqueTaskResultState<Out>>(Options.EstimatedResultBytes);
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
				}, Edge, ETaskDependencyKind::Completion, Options.EstimatedResultBytes);
			if (!Admission.HasValue()) return FAdmission::Failure(Admission.GetError());
			auto Handle = std::move(Admission).TakeValue();
			Output->BindProducer(Handle);
			return FAdmission::Success(FTaskAccess::Make<Out>(std::move(Handle), std::move(Output), std::move(Failure)));
		}
	}

	template<typename T>
	auto WhenAll(std::vector<TTask<T>>&& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTaskAdmission<TTask<std::vector<TTaskValue<T>>>>
	{
		using FValue = TTaskValue<T>;
		using FOut = std::vector<FValue>;
		using FAdmission = TTaskAdmission<TTask<FOut>>;
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
			auto Admission = Detail::TryFanIn<FOut>(Handles, Executor, Options, Bytes,
				[States] {
					FOut Values; Values.reserve(States.size());
					for (const auto& State : States) { auto Value = State->TakePublished(); require(Value); Values.emplace_back(std::move(*Value)); }
					return Values;
				}, [States] { for (const auto& State : States) State->Discard(); });
			if (!Admission.HasValue()) return Admission;
			auto Result = std::move(Admission).TakeValue();
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
		const FTaskExecutionOptions& Options = {}) -> TTaskAdmission<TTask<std::tuple<TTaskValue<Ts>...>>>
	{
		using FOut = std::tuple<TTaskValue<Ts>...>;
		using FAdmission = TTaskAdmission<TTask<FOut>>;
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
			auto Admission = Detail::TryFanIn<FOut>(Handles, Executor, Options, Bytes,
				[States] { return std::apply([](const auto&... State) { return FOut(std::move(*State->TakePublished())...); }, States); },
				[States] { std::apply([](const auto&... State) { (State->Discard(), ...); }, States); });
			if (!Admission.HasValue()) return Admission;
			auto Result = std::move(Admission).TakeValue();
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
		-> TTaskAdmission<TTask<U>>
	{
		using FAdmission = TTaskAdmission<TTask<U>>;
		if (!Input.GetCompletion().IsValid()) return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPrerequisite});
		if (!std::is_void_v<U> && Detail::ResultBytes<U>(Options) == 0)
			return FAdmission::Failure({ETaskAdmissionErrorCode::InvalidPayloadDeclaration});
		try
		{
			std::vector<FTaskHandle> Handles{Input.GetCompletion().GetTaskHandle()};
			auto Admission = Detail::TryFanIn<TTaskValue<U>>(Handles, Executor, Options,
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
			if (!Admission.HasValue()) return FAdmission::Failure(Admission.GetError());
			return FAdmission::Success(Detail::FTaskAccess::Rebind<U>(std::move(Admission).TakeValue()));
		}
		catch (const std::bad_alloc&) { return FAdmission::Failure({ETaskAdmissionErrorCode::CapacityExhausted}); }
	}

	// Each duplicate shared position retains its own immutable owner in input order.
	template<typename T>
	auto WhenAll(const std::vector<TSharedTask<T>>& Inputs, ETaskExecutor Executor = ETaskExecutor::Worker,
		const FTaskExecutionOptions& Options = {}) -> TTaskAdmission<TTask<std::vector<std::shared_ptr<const TTaskValue<T>>>>>
	{
		using FOut = std::vector<std::shared_ptr<const TTaskValue<T>>>;
		using FAdmission = TTaskAdmission<TTask<FOut>>;
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
			return Detail::TryFanIn<FOut>(Handles, Executor, Options, Bytes, [Inputs] {
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
		-> TTaskAdmission<TTask<std::shared_ptr<const TTaskValue<U>>>>
	{
		FTaskExecutionOptions ObserverOptions = Options;
		ObserverOptions.EstimatedResultBytes = std::max<uint64>(Options.EstimatedResultBytes, sizeof(std::shared_ptr<const TTaskValue<U>>));
		return ThenAsync(std::move(Input), Executor, ObserverOptions,
			[Function = std::forward<F>(Function), ObserverOptions](auto&&... Args) mutable {
				auto Shared = std::invoke(Function, std::forward<decltype(Args)>(Args)...);
				if constexpr (std::is_void_v<U>)
					return Then(Shared, ETaskExecutor::Worker, ObserverOptions, [Shared] { return Shared.GetResultShared(); });
				else return Then(Shared, ETaskExecutor::Worker, ObserverOptions, [Shared](const U&) { return Shared.GetResultShared(); });
			});
	}

}
