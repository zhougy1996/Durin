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

	auto AddNested(const FTaskHandle& Completion) -> void
	{
		if (auto Error = Durin::Private::FTaskRuntimeAccess::AddNested(Completion))
			Detail::FailConstruction(*Error);
	}

	struct FTaskEvent::FState
	{
		FTaskHandle Handle;
		std::mutex Mutex;
		bool Triggered = false;
		~FState() { requiref(Triggered, "Task events must be triggered before their last handle is released."); }
	};

	FTaskEvent::FTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement)
	{
		FTaskLaunchOptions Options;
		Options.bExternalCompletion = true;
		Options.bUnknownExecutionRequirement = bUnknownExecutionRequirement;
		Options.bUseExplicitScope = true;
		Options.bQueueOnSaturation = true;
		auto Admission = Durin::Private::TryLaunchCancelableTaskWithCompletion(DebugName, [](const FTaskCancellationToken&) {}, {}, Options);
		if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
		try { State = std::make_shared<FState>(); }
		catch (...)
		{
			Durin::Private::FTaskRuntimeAccess::CompleteExternal(std::move(Admission).TakeValue(), ETaskState::Failed);
			throw;
		}
		State->Handle = std::move(Admission).TakeValue();
	}

	auto FTaskEvent::GetTaskHandle() const -> FTaskHandle
	{
		require(State);
		return FTaskHandle(State->Handle);
	}

	auto FTaskEvent::AddPrerequisites(std::span<const FTaskHandle> Prerequisites) -> void
	{
		require(State);
		std::lock_guard Lock(State->Mutex);
		requiref(!State->Triggered, "Task event prerequisites must be added before Trigger.");
		for (const auto& Prerequisite : Prerequisites)
			if (auto Error = Durin::Private::FTaskRuntimeAccess::AddCompletionDependency(State->Handle, Prerequisite))
				Detail::FailConstruction(*Error);
	}

	auto FTaskEvent::Trigger() -> void
	{
		require(State);
		{
			std::lock_guard Lock(State->Mutex);
			requiref(!State->Triggered, "Task events may only be triggered once.");
			State->Triggered = true;
		}
		Durin::Private::FTaskRuntimeAccess::CompleteExternal(State->Handle, ETaskState::Succeeded);
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

	auto Wait(const FTaskHandle& Task) -> FTaskWaitResult { return WaitTask(Task); }
	auto Cancel(const FTaskHandle& Task) -> bool { return CancelTask(Task); }
	auto Wait(const FTaskGroupJoin& Join) -> FTaskWaitResult { return Durin::Private::FTaskRuntimeAccess::WaitGroup(Join.GetGroupToken()); }
	auto Cancel(const FTaskGroupJoin& Join) -> bool { return Durin::Private::FTaskRuntimeAccess::CloseGroup(Join.GetGroupToken(), ETaskScopeCloseMode::Cancel) == ETaskScopeCloseResult::Closed; }
}
