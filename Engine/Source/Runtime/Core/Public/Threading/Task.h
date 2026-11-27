#pragma once

#include "Misc/CoreStd.h"
#include "Misc/PreprocessorHelper.h"
#include "Misc/CoreTypes.h"
#include "Misc/CoreMiscDefines.h"

#include "Threading/TaskRuntime.h"

namespace Durin::Tasks
{

	namespace Detail
	{
		// Construction failures are programming/lifetime violations or allocation failure.
		// This is never used to turn ordinary scheduler saturation into an assertion.
		[[noreturn]] CORE_API auto FailConstruction(FTaskConstructionError Error) -> void;
	}

	namespace Detail
	{
		// Result access requires both a supported wait and successful execution in every build.
		CORE_API auto WaitForSuccess(const FTask& Handle) -> void;
	}
	namespace Detail
	{
		template<typename T> using TResultState = std::conditional_t<std::is_void_v<T>, std::nullptr_t, std::shared_ptr<TTaskResultState<T>>>;
	}

	enum class EParallelForFlags : uint8 { None, ForceSingleThread };

	namespace Detail
	{
		// Prefer the ordinary index signature for callables supporting both forms.
		template<typename F>
		using TParallelForBody = std::conditional_t<std::is_constructible_v<FParallelForFunction, F>,
			FParallelForFunction, FCancelableParallelForFunction>;

		CORE_API auto RunParallelFor(const char* Name, uint64 Num, std::optional<uint64> MinBatchSize,
			FParallelForFunction Body, EParallelForFlags Flags, const FCancellationToken* Cancellation,
			FTaskAttribution Attribution) -> FParallelForResult;
		CORE_API auto RunParallelFor(const char* Name, uint64 Num, std::optional<uint64> MinBatchSize,
			FCancelableParallelForFunction Body, EParallelForFlags Flags, const FCancellationToken* Cancellation,
			FTaskAttribution Attribution) -> FParallelForResult;
	}

	// Runs [0, Num) and joins all chunks. A body may also accept a cancellation token.
	template<typename F = FParallelForFunction>
	auto ParallelFor(const char* Name, uint64 Num, F&& Body, EParallelForFlags Flags = EParallelForFlags::None,
		const FCancellationToken* Cancellation = nullptr, FTaskAttribution Attribution = {}) -> FParallelForResult
	{
		return Detail::RunParallelFor(Name, Num, std::nullopt, Detail::TParallelForBody<F>(std::forward<F>(Body)),
			Flags, Cancellation, Attribution);
	}

	template<typename F = FParallelForFunction>
	auto ParallelFor(const char* Name, uint64 Num, uint64 MinBatchSize, F&& Body,
		EParallelForFlags Flags = EParallelForFlags::None, const FCancellationToken* Cancellation = nullptr,
		FTaskAttribution Attribution = {}) -> FParallelForResult
	{
		return Detail::RunParallelFor(Name, Num, MinBatchSize, Detail::TParallelForBody<F>(std::forward<F>(Body)),
			Flags, Cancellation, Attribution);
	}

	// Completion dependency only: does not forward cancellation or failure to either task.
	// Must be called from a running task body; its worker is free after the body returns.
	CORE_API auto AddNested(const FTask& Task) -> void;

	namespace Detail
	{
		template<typename T> concept CTaskDependency = std::convertible_to<const std::remove_cvref_t<T>&, FTask>;
		template<CTaskDependency T> auto DependencyHandle(const T& Task) -> FTask
		{ return Task; }

		template<typename R> concept CTaskDependencyCollection = std::ranges::input_range<const R>
			&& CTaskDependency<std::ranges::range_reference_t<const R>>;

		template<typename T> concept CTaskPrerequisites = CTaskDependency<T> || CTaskDependencyCollection<T>;

		// Dependency collection shares nodes without mutating their results.
		template<CTaskDependencyCollection R>
		auto CollectDependencies(const R& Inputs)
			-> std::vector<FTask>
		{
			try
			{
				std::vector<FTask> Handles;
				if constexpr (std::ranges::sized_range<const R>) Handles.reserve(std::ranges::size(Inputs));
				for (const auto& Input : Inputs) Handles.push_back(DependencyHandle(Input));
				return Handles;
			}
			catch (const std::bad_alloc&) { FailConstruction({ETaskConstructionErrorCode::AllocationFailure}); }
		}
	}

	// Waits for all inputs with one deadline, without allocating per-input outcomes.
	template<Detail::CTaskDependencyCollection R>
	auto Wait(const R& Inputs, double TimeoutSeconds = std::numeric_limits<double>::infinity()) -> bool
	{
		if constexpr (std::ranges::forward_range<const R>)
			if (std::ranges::all_of(Inputs, [](const auto& Input) { return Detail::DependencyHandle(Input).IsCompleted(); })) return true;
		// Helping can reenter the caller and mutate its collection. Pin a snapshot.
		return Detail::WaitCollection(Detail::CollectDependencies(Inputs), TimeoutSeconds);
	}

	// First already-completed input in collection order, or any subsequently observed completion.
	// Empty collections and unsuccessful waits return INDEX_NONE. Does not help execute inputs.
	template<Detail::CTaskDependencyCollection R>
	auto WaitAny(const R& Inputs, double TimeoutSeconds = std::numeric_limits<double>::infinity()) -> int32
	{
		if constexpr (std::ranges::forward_range<const R>)
		{
			size_t Index = 0;
			for (const auto& Input : Inputs)
			{
				require(Index < static_cast<size_t>(std::numeric_limits<int32>::max()));
				if (Detail::DependencyHandle(Input).IsCompleted()) return static_cast<int32>(Index);
				++Index;
			}
			if (Index == 0) return INDEX_NONE;
		}
		return Detail::WaitAnyCollection(Detail::CollectDependencies(Inputs), TimeoutSeconds);
	}

	// Extension: one outcome per input, including invalid handles and rejected waits.
	template<Detail::CTaskDependencyCollection R>
	auto WaitDetailed(const R& Inputs, double TimeoutSeconds = std::numeric_limits<double>::infinity()) -> std::vector<FTaskWaitResult>
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
	// Copyable handles share the task node and its result. Erased FTask handles
	// retain the same result lifetime without exposing a typed accessor.
	template<typename T>
	class TTask
	{
	public:
		TTask() = default;
		TTask(const TTask&) = default;
		TTask(TTask&&) noexcept = default;
		auto operator=(const TTask&) -> TTask& = default;
		auto operator=(TTask&&) noexcept -> TTask& = default;
		// Erasure copies or transfers observation, never exposes a writable base subobject.
		operator FTask() const & { return Handle; }
		operator FTask() && { return std::move(Handle); }
		auto IsValid() const -> bool { return Handle.IsValid(); }
		auto IsCompleted() const -> bool { return Handle.IsCompleted(); }
		auto GetState() const -> ETaskState { return Handle.GetState(); }
		auto GetDebugName() const -> const char* { return Handle.GetDebugName(); }
		auto GetTaskId() const -> uint64 { return Handle.GetTaskId(); }
		auto GetDiagnostic() const -> std::string { return Handle.GetDiagnostic(); }
		auto GetDiagnostics() const -> FTaskDiagnostics { return Handle.GetDiagnostics(); }
		auto Wait() const -> bool { return Handle.Wait(); }
		auto Wait(double TimeoutSeconds) const -> bool { return Handle.Wait(TimeoutSeconds); }
		auto WaitDetailed() const -> FTaskWaitResult { return Handle.WaitDetailed(); }
		auto WaitDetailed(double TimeoutSeconds) const -> FTaskWaitResult { return Handle.WaitDetailed(TimeoutSeconds); }
		auto TryRetractAndExecute() const -> bool { return Handle.TryRetractAndExecute(); }
		// Moving from this reference is an explicit mutation visible to all aliases.
		// Callers synchronize mutations of a shared result.
		auto GetResult() const -> std::add_lvalue_reference_t<T> requires (!std::is_void_v<T>)
		{
			Detail::WaitForSuccess(Handle);
			return *Storage->PeekPublished();
		}
	private:
		friend struct Detail::FTaskAccess;
		FTask Handle;
		std::conditional_t<std::is_void_v<T>, std::nullptr_t, TTaskResultState<T>*> Storage = nullptr;
	};

	namespace Detail
	{
		struct FTaskAccess
		{
			template<typename T> static auto Make(FTask Handle, Detail::TResultState<T> State) -> TTask<T>
			{
				TTask<T> Task;
				Task.Handle = std::move(Handle);
				if constexpr (!std::is_void_v<T>) Task.Storage = State.get();
				return Task;
			}

		};
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
			return Durin::Private::LaunchTaskWithCompletion(Name, std::forward<Body>(Function),
				MakeResultCompletion<T>(Result), Options, ResultStorageBytes<T, std::decay_t<Body>>(), {Result, !std::is_void_v<T>});
		}

	}

	// Publishes an existing value without submitting work or requiring a scheduler.
	template<typename T>
	auto MakeCompletedTask(T Value, const char* Name = "CompletedTask") -> TTask<T>
	{
		try
		{
			auto State = Detail::MakeResultState<T>();
			State->SetPending(std::move(Value));
			State->Complete(ETaskState::Succeeded);
			auto Handle = Durin::Private::FTaskRuntimeAccess::CreateCompletedTask(Name, {State, true});
			return Detail::FTaskAccess::Make<T>(std::move(Handle), std::move(State));
		}
		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskConstructionErrorCode::AllocationFailure}); }
	}

	namespace Detail
	{
		template<typename F, typename T = std::invoke_result_t<std::decay_t<F>&>>
		requires (!std::is_reference_v<T>)
		auto LaunchTaskImpl(const char* Name, F&& Function, FTaskLaunchOptions Launch)
			-> TTask<T>
		{
			try
			{
				auto State = Detail::MakeResultState<T>();
				Launch.EstimatedPayloadBytes = sizeof(std::decay_t<F>);
				auto Handle = Detail::LaunchWithStorage<T>(Name, State,
					[State, Function = std::forward<F>(Function)]() mutable {
						auto Invoke = [&]() -> T {
							return std::invoke(Function);
						};
						if constexpr (std::is_void_v<T>) Invoke();
						else State->SetPending(Invoke());
					}, Launch);
				return Detail::FTaskAccess::Make<T>(std::move(Handle), std::move(State));
			}
			catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskConstructionErrorCode::AllocationFailure}); }
		}

		// Snapshot inputs before moving the user callable, which may reenter the caller.
		template<typename F, CTaskPrerequisites R>
		auto LaunchAfter(const char* Name, F&& Function, const R& Inputs, FTaskLaunchOptions Launch)
		{
			if constexpr (CTaskDependency<R>)
			{
				const FTask Handle = Inputs;
				Launch.Prerequisites = std::span(&Handle, 1);
				return LaunchTaskImpl(Name, std::forward<F>(Function), Launch);
			}
			else
			{
				const auto Handles = CollectDependencies(Inputs);
				Launch.Prerequisites = Handles;
				return LaunchTaskImpl(Name, std::forward<F>(Function), Launch);
			}
		}
	}

	template<typename F>
	auto LaunchTask(const char* Name, F&& Function,
		ETaskPriority Priority = ETaskPriority::Normal, FTaskAttribution Attribution = {})
	{
		return Detail::LaunchTaskImpl(Name, std::forward<F>(Function),
			{.Attribution = Attribution, .Priority = Priority});
	}

	template<typename F>
	auto LaunchTask(const char* Name, F&& Function, FTaskAttribution Attribution)
	{
		return LaunchTask(Name, std::forward<F>(Function), ETaskPriority::Normal, Attribution);
	}

	template<typename F>
	auto LaunchTask(const char* Name, F&& Function, ETaskThread Thread,
		ETaskPriority Priority = ETaskPriority::Normal, FTaskAttribution Attribution = {})
	{
		return Detail::LaunchTaskImpl(Name, std::forward<F>(Function),
			{.Attribution = Attribution, .Target = Thread, .Priority = Priority});
	}

	template<typename F>
	auto LaunchTask(const char* Name, F&& Function, ETaskThread Thread, FTaskAttribution Attribution)
	{
		return LaunchTask(Name, std::forward<F>(Function), Thread, ETaskPriority::Normal, Attribution);
	}

	template<typename F, Detail::CTaskPrerequisites R>
	auto LaunchTask(const char* Name, F&& Function, const R& Inputs,
		ETaskPriority Priority = ETaskPriority::Normal, FTaskAttribution Attribution = {})
	{
		return Detail::LaunchAfter(Name, std::forward<F>(Function), Inputs,
			{.Attribution = Attribution, .Priority = Priority});
	}

	template<typename F, Detail::CTaskPrerequisites R>
	auto LaunchTask(const char* Name, F&& Function, const R& Inputs, FTaskAttribution Attribution)
	{
		return LaunchTask(Name, std::forward<F>(Function), Inputs, ETaskPriority::Normal, Attribution);
	}

	template<typename F, Detail::CTaskPrerequisites R>
	auto LaunchTask(const char* Name, F&& Function, const R& Inputs, ETaskThread Thread,
		ETaskPriority Priority = ETaskPriority::Normal, FTaskAttribution Attribution = {})
	{
		return Detail::LaunchAfter(Name, std::forward<F>(Function), Inputs,
			{.Attribution = Attribution, .Target = Thread, .Priority = Priority});
	}

	template<typename F, Detail::CTaskPrerequisites R>
	auto LaunchTask(const char* Name, F&& Function, const R& Inputs, ETaskThread Thread, FTaskAttribution Attribution)
	{
		return LaunchTask(Name, std::forward<F>(Function), Inputs, Thread, ETaskPriority::Normal, Attribution);
	}

}
