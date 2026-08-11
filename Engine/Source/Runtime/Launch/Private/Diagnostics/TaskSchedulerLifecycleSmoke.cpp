#include "TaskSchedulerLifecycleSmoke.h"

#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	// Retains every qualification handle and observation across task-system shutdown.
	struct FTaskSchedulerLifecycleSmokeState
	{
		FThreadEvent AdmissionProbeStarted;
		FTaskHandle AdmissionProbe;
		FTaskHandle SlowTask;
		FTaskHandle ShortTask;
		FTaskHandle DependentTask;
		FTaskHandle FailedTask;
		FTaskHandle FailedDependentTask;
		FTaskHandle CancelableTask;
		FTaskHandle ParallelTask;
		FTaskHandle WaiterTask;
		FTaskHandle GameThreadSource;
		FTaskHandle GameThreadDeferred;
		FParallelForResult ParallelResult;
		ETaskState WaitedState = ETaskState::Invalid;
		uint64 ParallelChecksum = 0;
		bool bAdmissionRejected = false;
		bool bGameThreadDeferredRan = false;
	};

	auto BeginTaskSchedulerLifecycleSmoke()
		-> std::shared_ptr<FTaskSchedulerLifecycleSmokeState>
	{
		auto State = std::make_shared<FTaskSchedulerLifecycleSmokeState>();
		FTaskSchedulerLifecycleSmokeState* StatePtr = State.get();
		State->ShortTask = LaunchTask("EngineSmoke.Short", []() {});
		std::array<FTaskHandle, 1> ShortPrerequisites{State->ShortTask};
		FTaskLaunchOptions DependentOptions;
		DependentOptions.Prerequisites = ShortPrerequisites;
		State->DependentTask = LaunchTask("EngineSmoke.Dependent", []() {}, DependentOptions);

		State->FailedTask = LaunchTask("EngineSmoke.Failure", []() {
			throw std::runtime_error("intentional engine lifecycle smoke failure");
		});
		std::array<FTaskHandle, 1> FailedPrerequisites{State->FailedTask};
		FTaskLaunchOptions FailedDependentOptions;
		FailedDependentOptions.Prerequisites = FailedPrerequisites;
		State->FailedDependentTask = LaunchTask(
			"EngineSmoke.FailureDependent", []() {}, FailedDependentOptions);

		State->CancelableTask = LaunchCancelableTask(
			"EngineSmoke.Canceled",
			[](const FTaskCancellationToken& Token) {
				while (!Token.IsCancellationRequested()) std::this_thread::yield();
			});
		const bool bCancellationRequested = CancelTask(State->CancelableTask);
		checkf(bCancellationRequested,
			"Engine scheduler lifecycle smoke could not cancel its task.");

		State->ParallelTask = LaunchTask("EngineSmoke.ParallelFor", [StatePtr]() {
			constexpr uint64 Num = 65'536;
			std::vector<uint64> Output(Num);
			FParallelForOptions Options;
			Options.MinBatchSize = 256;
			StatePtr->ParallelResult = ParallelFor(
				"EngineSmoke.ParallelWork", Num,
				[&Output](uint64 Index) {
					uint64 Value = Index + 0x9e3779b97f4a7c15ull;
					for (uint32 Round = 0; Round < 64; ++Round)
					{
						Value ^= Value >> 12;
						Value ^= Value << 25;
						Value ^= Value >> 27;
						Value *= 0x2545f4914f6cdd1dull;
					}
					Output[Index] = Value;
				}, Options);
			StatePtr->ParallelChecksum = Output[Num / 2];
		});

		State->WaiterTask = LaunchTask("EngineSmoke.Waiter", [StatePtr]() {
			StatePtr->WaitedState = WaitTask(StatePtr->DependentTask);
		});

		checkf(State->ShortTask.IsValid() && State->DependentTask.IsValid()
			&& State->FailedTask.IsValid() && State->FailedDependentTask.IsValid()
			&& State->CancelableTask.IsValid() && State->ParallelTask.IsValid()
			&& State->WaiterTask.IsValid(),
			"Engine scheduler lifecycle smoke could not launch its workload.");
		const std::array<FTaskHandle, 7> QualificationTasks{
			State->ShortTask, State->DependentTask, State->FailedTask,
			State->FailedDependentTask, State->CancelableTask,
			State->ParallelTask, State->WaiterTask};
		WaitAll(QualificationTasks);

		State->GameThreadSource = LaunchTask("EngineSmoke.GameThreadSource", []() {});
		FTaskContinuationOptions GameThreadOptions;
		GameThreadOptions.Target = ETaskTarget::GameThreadDeferred;
		GameThreadOptions.EstimatedPayloadBytes = 64;
		State->GameThreadDeferred = Then(
			State->GameThreadSource,
			"EngineSmoke.GameThreadDeferred",
			[StatePtr]() { StatePtr->bGameThreadDeferredRan = IsInGameThread(); },
			GameThreadOptions);
		checkf(State->GameThreadSource.IsValid() && State->GameThreadDeferred.IsValid(),
			"Engine scheduler lifecycle smoke could not launch its deferred chain.");

		State->AdmissionProbe = LaunchTask("EngineSmoke.AdmissionProbe", [StatePtr]() {
			StatePtr->AdmissionProbeStarted.Trigger();
			while (IsTaskSchedulerRunning()) std::this_thread::yield();
			StatePtr->bAdmissionRejected = !LaunchTask(
				"EngineSmoke.RejectedAfterClose", []() {}).IsValid();
		});
		checkf(State->AdmissionProbe.IsValid(),
			"Engine scheduler lifecycle smoke could not launch its admission probe.");
		const bool bAdmissionProbeStarted = State->AdmissionProbeStarted.WaitFor(1.0);
		checkf(bAdmissionProbeStarted,
			"Engine scheduler lifecycle smoke admission probe did not start.");
		State->SlowTask = LaunchTask("EngineSmoke.Long", []() {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		});
		checkf(State->SlowTask.IsValid(),
			"Engine scheduler lifecycle smoke could not launch its long task.");
		const FTaskSchedulerDiagnostics Diagnostics = GetTaskSchedulerDiagnostics();
		checkf(Diagnostics.bRunning && Diagnostics.NonterminalTaskCount > 0,
			"Engine scheduler lifecycle smoke found no active workload before exit.");
		return State;
	}

	auto ValidateTaskSchedulerLifecycleSmoke(
		const std::shared_ptr<FTaskSchedulerLifecycleSmokeState>& State) -> void
	{
		check(State != nullptr);
		checkf(!IsTaskSchedulerRunning(),
			"Engine scheduler lifecycle smoke left the scheduler running.");
		checkf(State->bAdmissionRejected,
			"Engine scheduler lifecycle smoke admitted work after close.");
		checkf(State->AdmissionProbe.GetState() == ETaskState::Succeeded
			&& State->SlowTask.GetState() == ETaskState::Succeeded
			&& State->ShortTask.GetState() == ETaskState::Succeeded
			&& State->DependentTask.GetState() == ETaskState::Succeeded
			&& State->WaiterTask.GetState() == ETaskState::Succeeded
			&& State->GameThreadSource.GetState() == ETaskState::Succeeded
			&& State->GameThreadDeferred.GetState() == ETaskState::Succeeded
			&& State->bGameThreadDeferredRan,
			"Engine scheduler lifecycle smoke did not drain accepted work.");
		checkf(State->FailedTask.GetState() == ETaskState::Failed
			&& State->FailedDependentTask.GetState() == ETaskState::Canceled
			&& State->CancelableTask.GetState() == ETaskState::Canceled,
			"Engine scheduler lifecycle smoke produced incorrect failure or cancellation propagation.");
		checkf(State->WaitedState == ETaskState::Succeeded,
			"Engine scheduler lifecycle smoke waiter observed the wrong state.");
		checkf(State->ParallelTask.GetState() == ETaskState::Succeeded
			&& State->ParallelResult.State == ETaskState::Succeeded
			&& State->ParallelResult.ChunkCount > 1 && State->ParallelChecksum != 0,
			"Engine scheduler lifecycle smoke parallel workload failed.");

		const FTaskSchedulerDiagnostics Diagnostics = GetTaskSchedulerDiagnostics();
		const FGameThreadDeferredWorkQueueDiagnostics DeferredDiagnostics =
			GetGameThreadDeferredWorkQueueDiagnostics();
		checkf(!Diagnostics.bRunning
			&& Diagnostics.NonterminalTaskCount == 0
			&& Diagnostics.ActiveWorkerCount == 0
			&& Diagnostics.FailedTaskCount >= 1
			&& Diagnostics.CanceledTaskCount >= 2
			&& Diagnostics.RejectedTaskCount >= 1
			&& Diagnostics.LongWaitCount >= 1
			&& Diagnostics.RetainedTerminalHandleCount >= 9,
			"Engine scheduler lifecycle smoke diagnostics were incomplete.");
		checkf(!DeferredDiagnostics.bInstalled
			&& DeferredDiagnostics.PumpedCallbackCount >= 1,
			"Engine scheduler lifecycle smoke did not drain the GameThread executor.");
		DURIN_INFO(
			"Task scheduler lifecycle smoke passed. (completed: {}, failed: {}, canceled: {}, rejected: {}, long waits: {}, retained handles: {})",
			Diagnostics.CompletedTaskCount,
			Diagnostics.FailedTaskCount,
			Diagnostics.CanceledTaskCount,
			Diagnostics.RejectedTaskCount,
			Diagnostics.LongWaitCount,
			Diagnostics.RetainedTerminalHandleCount);
	}
}
