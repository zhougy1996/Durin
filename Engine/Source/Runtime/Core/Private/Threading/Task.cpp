#include "Threading/Task.h"

namespace Durin::Tasks
{
	namespace Detail
	{
		[[noreturn]] auto FailConstruction(FTaskConstructionError Error) -> void
		{
			requiref(false, "Task construction failed (code {}, task {}).", static_cast<uint32>(Error.Code), Error.RelatedTaskId);
			std::terminate();
		}

		auto WaitForSuccess(const Tasks::FTask& Handle) -> void
		{
			const auto WaitResult = WaitTask(Handle);
			require(WaitResult.WaitStatus == ETaskWaitStatus::Completed && Handle.GetState() == ETaskState::Succeeded);
		}

		auto ReadFailure(const Tasks::FTask& Handle) -> FTaskFailure
		{
			requiref(Handle.GetState() == ETaskState::Failed, "Failure access requires Failed (task {} {}, state {}).", Handle.GetTaskId(), Handle.GetDebugName(), static_cast<uint32>(Handle.GetState()));
			return Durin::Private::FTaskRuntimeAccess::GetFailure(Handle);
		}
	}

	auto SubscribeTask(const Tasks::FTask& Task, std::function<void(ETaskState)> Callback) -> FTaskSubscription
	{
		require(Task.IsValid() && Callback);
		FTaskSubscription Subscription;
		Subscription.Callback = std::make_shared<std::function<void(ETaskState)>>(std::move(Callback));
		auto Hook = std::make_shared<Durin::Private::FTaskTerminalHook>();
		Hook->Function = [Weak = std::weak_ptr(Subscription.Callback)](ETaskState State) {
			if (auto Pinned = Weak.lock()) { try { (*Pinned)(State); } catch (...) {} }
		};
		Durin::Private::FTaskRuntimeAccess::BindTerminal(Task, std::move(Hook));
		return Subscription;
	}

	auto AddNested(const Tasks::FTask& Completion) -> void
	{
		if (auto Error = Durin::Private::FTaskRuntimeAccess::AddNested(Completion))
			Detail::FailConstruction(*Error);
	}

	FTaskEvent::FTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement)
	{
		SetHandle(Durin::Private::FTaskRuntimeAccess::CreateTaskEvent(DebugName, bUnknownExecutionRequirement));
	}

	auto FTaskEvent::AddPrerequisites(std::span<const Durin::Tasks::FTask> Prerequisites) -> void
	{
		if (auto Error = Durin::Private::FTaskRuntimeAccess::AddTaskEventPrerequisites(*this, Prerequisites))
			Detail::FailConstruction(*Error);
	}

	auto FTaskEvent::Trigger() -> void
	{
		Durin::Private::FTaskRuntimeAccess::TriggerTaskEvent(*this);
	}

	auto ParallelFor(const char* Name, uint64 Num, FParallelForFunction&& Function,
		const FParallelForPolicyOptions& Options) -> FParallelForResult
	{
		FParallelForOptions Native;
		Native.CancellationToken = Options.Cancellation;
		Native.Attribution = Options.Attribution;
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

	auto Wait(const Tasks::FTask& Task) -> FTaskWaitResult { return WaitTask(Task); }
	auto Wait(const Tasks::FTask& Task, double TimeoutSeconds) -> FTaskWaitResult { return WaitTask(Task, TimeoutSeconds); }
	auto Cancel(const Tasks::FTask& Task) -> bool { return CancelTask(Task); }
}
