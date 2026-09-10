#include "Threading/TaskComposition.h"

namespace Durin::Tasks
{
	namespace Detail
	{
		auto CreateGroupScope() -> FTaskScope
		{
			try { return CreateTaskScope(); }
			catch (const std::bad_alloc&) { requiref(false, "Task group allocation failed."); std::terminate(); }
		}

		[[noreturn]] auto FailConstruction(FTaskAdmissionError Error) -> void
		{
			requiref(false, "Task construction failed (code {}, task {}).", static_cast<uint32>(Error.Code), Error.RelatedTaskId);
			std::terminate();
		}

		auto WaitForSuccess(const FTaskHandle& Handle) -> void
		{
			const auto WaitResult = WaitTask(Handle);
			require(WaitResult.WaitStatus == ETaskWaitStatus::Completed && Handle.GetState() == ETaskState::Succeeded);
		}

		auto ReadFailure(const FTaskHandle& Handle, const FTaskFailure* Failure,
			std::optional<ETaskTerminalReason> FallbackReason) -> FTaskFailure
		{
			require(Handle.GetState() == ETaskState::Failed);
			if (Failure) return *Failure;
			return FTaskFailure{FallbackReason ? *FallbackReason : Handle.GetDiagnostics().TerminalReason};
		}
	}

	FTaskGroup::FTaskGroup() : Scope(Detail::CreateGroupScope())
	{
		requiref(Scope.IsValid(), "Task group requires a running scheduler.");
	}

	FTaskGroup::~FTaskGroup()
	{
		if (Scope.IsValid()) Durin::Private::FTaskRuntimeAccess::DiagnoseGroupDestruction(Scope.GetToken());
	}

	auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function,
		const FParallelForPolicyOptions& Options) -> FParallelForResult
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

	auto Wait(const FTaskCompletion& Completion) -> FTaskWaitResult { return Completion.GetTaskHandle().IsValid() ? WaitTask(Completion.GetTaskHandle()) : Durin::Private::FTaskRuntimeAccess::WaitGroup(Completion.GetGroupToken()); }
	auto Cancel(const FTaskCompletion& Completion) -> bool { return Completion.GetTaskHandle().IsValid() ? CancelTask(Completion.GetTaskHandle()) : Durin::Private::FTaskRuntimeAccess::CloseGroup(Completion.GetGroupToken(), ETaskScopeCloseMode::Cancel) == ETaskScopeCloseResult::Closed; }
}
