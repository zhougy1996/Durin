#include "Threading/TaskComposition.h"

namespace Durin::Tasks
{
	namespace Detail
	{
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

		auto ReadFailure(const FTaskHandle& Handle, const FTaskFailure* Failure) -> FTaskFailure
		{
			require(Handle.GetState() == ETaskState::Failed);
			if (Failure) return *Failure;
			return FTaskFailure{Handle.GetDiagnostics().TerminalReason};
		}
	}

	auto SubscribeTask(const FTaskHandle& Task, std::function<void(ETaskState)> Callback) -> FTaskSubscription
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
	FTaskReadiness::FTaskReadiness() : State(std::make_shared<std::atomic<bool>>(false)) {}
	auto ObserveTaskReady(const FTaskHandle& Task, const FTaskReadiness& Readiness) -> FTaskSubscription
	{
		return SubscribeTask(Task, [State = Readiness.State](ETaskState) { State->store(true, std::memory_order_release); });
	}

	auto AddNested(const FTaskHandle& Completion) -> void
	{
		if (auto Error = Durin::Private::FTaskRuntimeAccess::AddNested(Completion))
			Detail::FailConstruction(*Error);
	}

	struct FTaskEvent::FState
	{
		std::mutex Mutex;
		bool Triggered = false;
		~FState() { requiref(Triggered, "Task events must be triggered before their last handle is released."); }
	};

	FTaskEvent::FTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement)
	{
		FTaskLaunchOptions Options;
		Options.bExternalCompletion = true;
		Options.bUnknownExecutionRequirement = bUnknownExecutionRequirement;
		Options.bQueueOnSaturation = true;
		auto Admission = Durin::Private::TryLaunchCancelableTaskWithCompletion(DebugName, [](const FTaskCancellationToken&) {}, {}, Options);
		if (!Admission.HasValue()) Detail::FailConstruction(Admission.GetError());
		try { State = std::make_shared<FState>(); }
		catch (...)
		{
			Durin::Private::FTaskRuntimeAccess::CompleteExternal(std::move(Admission).TakeValue(), ETaskState::Failed);
			throw;
		}
		SetHandle(std::move(Admission).TakeValue());
	}

	auto FTaskEvent::AddPrerequisites(std::span<const Durin::FTaskHandle> Prerequisites) -> void
	{
		require(State);
		std::lock_guard Lock(State->Mutex);
		requiref(!State->Triggered, "Task event prerequisites must be added before Trigger.");
		for (const auto& Prerequisite : Prerequisites)
			if (auto Error = Durin::Private::FTaskRuntimeAccess::AddCompletionDependency(*this, Prerequisite))
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
		Durin::Private::FTaskRuntimeAccess::CompleteExternal(*this, ETaskState::Succeeded);
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

	auto Wait(const FTaskHandle& Task) -> FTaskWaitResult { return WaitTask(Task); }
	auto Wait(const FTaskHandle& Task, double TimeoutSeconds) -> FTaskWaitResult { return WaitTask(Task, TimeoutSeconds); }
	auto Cancel(const FTaskHandle& Task) -> bool { return CancelTask(Task); }
}
