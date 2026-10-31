#include "TaskKernelTestSupport.h"
#include <gtest/gtest.h>

#include <barrier>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Profiling/Profiling.h"
#include "Threading/QueuedThreadPool.h"
#include "Threading/Private/QueuedWorkNode.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	static_assert(sizeof(FTaskAttribution) == sizeof(uint16) * 2);
	static_assert(std::is_trivially_copyable_v<FTaskAttribution>);
	static_assert(std::is_standard_layout_v<FTaskAttribution>);
	static_assert(std::equality_comparable<FTaskAttribution>);
	static_assert(noexcept(Profiling::TaskAggregatePlots(2, 2, 0, 0, 0, 0)));

	namespace
	{
		class FWaitTaskRunnable final : public FRunnable
		{
		public:
			FWaitTaskRunnable(Tasks::FTask InTarget, FThreadEvent& InReturnedEvent)
				: Target(std::move(InTarget))
				, ReturnedEvent(InReturnedEvent)
			{
			}

			auto Run() -> uint32 override
			{
				ObservedResult = WaitTask(Target);
				ReturnedEvent.Trigger();
				return 0;
			}

			Tasks::FTask Target;
			FThreadEvent& ReturnedEvent;
			FTaskWaitResult ObservedResult;
		};

		class FEngineThreadPoolTestGuard
		{
		public:
			~FEngineThreadPoolTestGuard()
			{
				ShutdownTaskScheduler(false);
			}
		};

		class FTaskTerminalPublicationTestHookGuard
		{
		public:
			explicit FTaskTerminalPublicationTestHookGuard(std::function<void(uint64)>&& Hook)
			{
				Private::SetTaskTerminalPublicationTestHook(std::move(Hook));
			}

			~FTaskTerminalPublicationTestHookGuard()
			{
				Private::SetTaskTerminalPublicationTestHook({});
			}
		};

		class FTaskSchedulerSnapshotTestHookGuard
		{
		public:
			explicit FTaskSchedulerSnapshotTestHookGuard(std::function<void()>&& Hook)
			{
				Private::SetTaskSchedulerSnapshotTestHook(std::move(Hook));
			}

			~FTaskSchedulerSnapshotTestHookGuard()
			{
				Private::SetTaskSchedulerSnapshotTestHook({});
			}
		};

		auto EnsureGameThreadForTaskTest() -> void
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
		}
	} // namespace

	TEST(FParallelForTests, SerialPathPreservesFailureCancellationAndCaptureRetirement)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		for (const bool WithScheduler : {false, true})
		{
			if (WithScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
			FTaskCancellationSource Cancellation;
			FParallelForOptions Options;
			Options.CancellationToken = Cancellation.GetToken();
			uint32 Calls = 0;
			auto Lifetime = std::make_shared<int>(1);
			std::weak_ptr<int> Weak = Lifetime;
			auto Result = ParallelForCancelable("SerialCancelAndThrow", 4,
				[&, Owner = std::move(Lifetime)](uint64 Index, const FParallelForCancellationToken& Token) {
					++Calls;
					EXPECT_FALSE(Token.IsCancellationRequested());
					if (Index == 1) { Cancellation.RequestCancellation(); throw std::runtime_error("serial failure"); }
				}, Options);
			EXPECT_EQ(Result.State, ETaskState::Failed);
			EXPECT_EQ(Result.Diagnostic, "ParallelFor chunk [0, 4) failed: serial failure");
			EXPECT_EQ(Result.ChunkCount, 1u);
			EXPECT_EQ(Calls, 2u);
			EXPECT_TRUE(Weak.expired());
			FTaskCancellationSource LastIteration;
			Options.CancellationToken = LastIteration.GetToken();
			auto Canceled = ParallelForCancelable("SerialLastIterationCancel", 1,
				[&](uint64, const FParallelForCancellationToken&) { LastIteration.RequestCancellation(); }, Options);
			EXPECT_EQ(Canceled.State, ETaskState::Canceled);
			EXPECT_EQ(Canceled.ChunkCount, 1u);
			auto Empty = ParallelForCancelable("SerialEmpty", 0,
				[](uint64, const FParallelForCancellationToken&) { ADD_FAILURE(); }, Options);
			EXPECT_EQ(Empty.State, ETaskState::Succeeded);
			EXPECT_EQ(Empty.ChunkCount, 0u);
			auto Unknown = ParallelFor("SerialUnknown", 2, [](uint64) { throw 1; });
			EXPECT_EQ(Unknown.State, ETaskState::Failed);
			EXPECT_EQ(Unknown.Diagnostic, "ParallelFor chunk [0, 2) failed: callable threw an unknown exception.");
		}
	}

	TEST(FTaskSchedulerTests, InitializesFacadeAndRunsWork)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;

		EXPECT_GE(GetDefaultThreadPoolThreadCount(), 1u);
		ASSERT_TRUE(InitializeTaskScheduler(2));
		EXPECT_TRUE(IsTaskSchedulerRunning());

		FThreadEvent TaskFinished;
		std::atomic<bool> bObservedWorkerThread = false;
		Tasks::FTask Handle = SubmitKernelTask("EnginePoolTask", [&]() {
			bObservedWorkerThread.store(IsInWorkerThread(), std::memory_order::release);
			TaskFinished.Trigger();
		});
		ASSERT_TRUE(Handle.IsValid());

		ASSERT_TRUE(TaskFinished.WaitFor(1.0));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Handle).TaskState);

		ShutdownTaskScheduler(true);

		EXPECT_FALSE(IsTaskSchedulerRunning());
		EXPECT_TRUE(bObservedWorkerThread.load(std::memory_order::acquire));
	}

	TEST(FTaskSchedulerTests, InitializationIsIdempotentWhileRunning)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_TRUE(InitializeTaskScheduler(4));

		Tasks::FTask Handle = SubmitKernelTask("IdempotentInitialization", []() {});
		ASSERT_TRUE(Handle.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Handle).TaskState);
	}

	TEST(FTaskSchedulerTests, ConcurrentProducersCompleteBeforeSchedulerClosure)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		constexpr uint32 ProducerCount = 8;
		std::barrier StartBarrier(ProducerCount + 1);
		std::array<Tasks::FTask, ProducerCount> Handles;
		std::vector<std::thread> Producers;
		Producers.reserve(ProducerCount);
		for (uint32 ProducerIndex = 0; ProducerIndex < ProducerCount; ++ProducerIndex)
		{
			Producers.emplace_back([&, ProducerIndex]() {
				StartBarrier.arrive_and_wait();
				Handles[ProducerIndex] = SubmitKernelTask("AdmissionRace", []() {});
			});
		}

		StartBarrier.arrive_and_wait();
		for (std::thread& Producer : Producers)
		{
			Producer.join();
		}

		ShutdownTaskScheduler(true);
		for (const Tasks::FTask& Handle : Handles)
		{
			ASSERT_TRUE(Handle.IsValid());
			EXPECT_EQ(ETaskState::Succeeded, Handle.GetState());
		}
		EXPECT_DEATH({ (void)SubmitKernelTask("RejectedAfterClose", []() {}); }, "");
	}




















	TEST(FTaskTests, LaunchTaskReturnsValidHandleAndCompletes)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		std::atomic<bool> bTaskRan = false;
		Tasks::FTask Handle = SubmitKernelTask("HandleCompletion", [&]() {
			bTaskRan.store(true, std::memory_order::release);
		});

		ASSERT_TRUE(Handle.IsValid());
		EXPECT_STREQ("HandleCompletion", Handle.GetDebugName());

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Handle).TaskState);

		EXPECT_TRUE(Handle.IsCompleted());
		EXPECT_TRUE(bTaskRan.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, WaitAllWaitsForManyTasks)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));

		constexpr uint32 TaskCount = 32;
		std::atomic<uint32> CompletedTaskCount = 0;
		std::vector<Tasks::FTask> Handles;
		Handles.reserve(TaskCount);

		for (uint32 TaskIndex = 0; TaskIndex < TaskCount; ++TaskIndex)
		{
			Handles.push_back(SubmitKernelTask("WaitAllTask", [&]() {
				CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}));
			ASSERT_TRUE(Handles.back().IsValid());
		}

		const std::vector<FTaskWaitResult> Outcomes = WaitAll(std::span<const Tasks::FTask>(Handles.data(), Handles.size()));

		EXPECT_EQ(TaskCount, CompletedTaskCount.load(std::memory_order::acquire));
		ASSERT_EQ(TaskCount, Outcomes.size());
		for (const FTaskWaitResult& Outcome : Outcomes)
		{
			EXPECT_EQ(ETaskWaitStatus::Completed, Outcome.WaitStatus);
			EXPECT_EQ(ETaskState::Succeeded, Outcome.TaskState);
		}
		for (const Tasks::FTask& Handle : Handles)
		{
			EXPECT_TRUE(Handle.IsCompleted());
		}
	}

	TEST(FTaskTests, WaitingOneHandleDoesNotRequirePoolIdle)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent ReleaseBlockedTask;
		FThreadEvent BlockedTaskStarted;
		std::atomic<bool> bTargetTaskRan = false;

		Tasks::FTask TargetHandle = SubmitKernelTask("TargetTask", [&]() {
			bTargetTaskRan.store(true, std::memory_order::release);
		});
		ASSERT_TRUE(TargetHandle.IsValid());

		Tasks::FTask BlockedHandle = SubmitKernelTask("BlockedTask", [&]() {
			BlockedTaskStarted.Trigger();
			ReleaseBlockedTask.Wait();
		});
		ASSERT_TRUE(BlockedHandle.IsValid());

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(TargetHandle).TaskState);

		EXPECT_TRUE(TargetHandle.IsCompleted());
		EXPECT_TRUE(bTargetTaskRan.load(std::memory_order::acquire));
		ASSERT_TRUE(BlockedTaskStarted.WaitFor(1.0));
		EXPECT_FALSE(BlockedHandle.IsCompleted());

		ReleaseBlockedTask.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(BlockedHandle).TaskState);
	}

	TEST(FTaskTests, WorkerWaitHelpsNestedTaskOnSingleWorkerPool)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::atomic<bool> bChildTaskRan = false;
		std::atomic<bool> bChildHandleWasValid = false;
		std::atomic<bool> bParentTaskFinished = false;

		Tasks::FTask ParentHandle = SubmitKernelTask("ParentTask", [&]() {
			Tasks::FTask ChildHandle = SubmitKernelTask("ChildTask", [&]() {
				bChildTaskRan.store(true, std::memory_order::release);
			});

			bChildHandleWasValid.store(ChildHandle.IsValid(), std::memory_order::release);
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(ChildHandle).TaskState);
			bParentTaskFinished.store(true, std::memory_order::release);
		});
		ASSERT_TRUE(ParentHandle.IsValid());

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(ParentHandle).TaskState);

		EXPECT_TRUE(ParentHandle.IsCompleted());
		EXPECT_TRUE(bChildHandleWasValid.load(std::memory_order::acquire));
		EXPECT_TRUE(bChildTaskRan.load(std::memory_order::acquire));
		EXPECT_TRUE(bParentTaskFinished.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, WorkerWaitRejectsDependentThatCannotRunUntilCurrentTaskCompletes)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		Tasks::FTask Blocker = SubmitKernelTask("IneligibleWaitBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		Tasks::FTask Parent;
		Tasks::FTask DependentChild;
		std::atomic<ETaskWaitStatus> ObservedWaitStatus = ETaskWaitStatus::InvalidTask;
		std::atomic<ETaskState> ObservedWaitState = ETaskState::Invalid;
		Parent = SubmitKernelTask("IneligibleWaitParent", [&]() {
			std::array<Tasks::FTask, 1> Prerequisites{Parent};
			FTaskLaunchOptions Options;
			Options.Prerequisites = Prerequisites;
			DependentChild = SubmitKernelTask("IneligibleWaitChild", []() {}, Options);
			const FTaskWaitResult WaitResult = WaitTask(DependentChild);
			ObservedWaitStatus.store(WaitResult.WaitStatus, std::memory_order::release);
			ObservedWaitState.store(WaitResult.TaskState, std::memory_order::release);
		});
		ReleaseBlocker.Trigger();

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Parent).TaskState);
		ASSERT_TRUE(DependentChild.IsValid());
		EXPECT_EQ(ETaskWaitStatus::DependencyCycle, ObservedWaitStatus.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Waiting, ObservedWaitState.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(DependentChild).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
	}

	TEST(FTaskTests, StandardAndUnknownExceptionsPublishFailureDiagnostics)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		Tasks::FTask StandardFailure = SubmitKernelTask("StandardFailure", []() {
			throw std::runtime_error("expected task failure");
		});
		Tasks::FTask UnknownFailure = SubmitKernelTask("UnknownFailure", []() {
			throw 7;
		});

		EXPECT_EQ(ETaskState::Failed, WaitTask(StandardFailure).TaskState);
		EXPECT_EQ("expected task failure", StandardFailure.GetDiagnostic());
		EXPECT_EQ(ETaskState::Failed, WaitTask(UnknownFailure).TaskState);
		EXPECT_EQ("Task callable threw an unknown exception.", UnknownFailure.GetDiagnostic());
	}

	TEST(FTaskTests, DrainShutdownCompletesQueuedAndRunningTasks)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		Tasks::FTask RunningTask = SubmitKernelTask("RunningDuringDrain", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		Tasks::FTask QueuedTask = SubmitKernelTask("QueuedDuringDrain", []() {});

		std::thread ShutdownThread([]() {
			ShutdownTaskScheduler(true);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}
		EXPECT_FALSE(QueuedTask.IsCompleted());

		ReleaseBlockingTask.Trigger();
		ShutdownThread.join();

		EXPECT_EQ(ETaskState::Succeeded, RunningTask.GetState());
		EXPECT_EQ(ETaskState::Succeeded, QueuedTask.GetState());
	}

	TEST(FTaskTests, CompletionRacingDiscardAlwaysPublishesOneTerminalState)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		Tasks::FTask BlockingTask = SubmitKernelTask("DiscardRaceBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		std::atomic<uint32> ExecutionCount = 0;
		Tasks::FTask RacingTask = SubmitKernelTask("DiscardRaceTarget", [&]() {
			ExecutionCount.fetch_add(1, std::memory_order::acq_rel);
		});

		std::barrier RaceBarrier(2);
		std::thread ShutdownThread([&]() {
			RaceBarrier.arrive_and_wait();
			ShutdownTaskScheduler(false);
		});
		RaceBarrier.arrive_and_wait();
		ReleaseBlockingTask.Trigger();
		ShutdownThread.join();

		const ETaskState RacingState = RacingTask.GetState();
		EXPECT_TRUE(RacingState == ETaskState::Succeeded || RacingState == ETaskState::Succeeded);
		EXPECT_LE(ExecutionCount.load(std::memory_order::acquire), 1u);
		const ETaskState BlockingState = BlockingTask.GetState();
		EXPECT_TRUE(BlockingState == ETaskState::Succeeded || BlockingState == ETaskState::Succeeded);
	}

	TEST(FTaskTests, ImmutableDependenciesReleaseChainsFanInAndFanOutOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FThreadEvent ReleaseRoot;
		std::atomic<uint32> ExecutionMask = 0;
		std::atomic<bool> bOrderValid = true;
		Tasks::FTask Root = SubmitKernelTask("DependencyRoot", [&]() {
			ReleaseRoot.Wait();
			ExecutionMask.fetch_or(1u, std::memory_order::acq_rel);
		});

		std::array<Tasks::FTask, 1> RootPrerequisite{Root};
		FTaskLaunchOptions RootOptions;
		RootOptions.Prerequisites = RootPrerequisite;
		Tasks::FTask Left = SubmitKernelTask("DependencyLeft", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 1u) == 0)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(2u, std::memory_order::acq_rel); }, RootOptions);
		Tasks::FTask Right = SubmitKernelTask("DependencyRight", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 1u) == 0)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(4u, std::memory_order::acq_rel); }, RootOptions);

		std::array<Tasks::FTask, 2> JoinPrerequisites{Left, Right};
		FTaskLaunchOptions JoinOptions;
		JoinOptions.Prerequisites = JoinPrerequisites;
		Tasks::FTask Join = SubmitKernelTask("DependencyJoin", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 7u) != 7u)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(8u, std::memory_order::acq_rel); }, JoinOptions);

		std::array<Tasks::FTask, 1> JoinPrerequisite{Join};
		FTaskLaunchOptions TailOptions;
		TailOptions.Prerequisites = JoinPrerequisite;
		Tasks::FTask Tail = SubmitKernelTask("DependencyTail", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 15u) != 15u)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(16u, std::memory_order::acq_rel); }, TailOptions);

		EXPECT_EQ(ETaskState::Waiting, Left.GetState());
		EXPECT_EQ(ETaskState::Waiting, Right.GetState());
		EXPECT_EQ(ETaskState::Waiting, Join.GetState());
		EXPECT_EQ(ETaskState::Waiting, Tail.GetState());
		ReleaseRoot.Trigger();

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Tail).TaskState);
		EXPECT_TRUE(bOrderValid.load(std::memory_order::acquire));
		EXPECT_EQ(31u, ExecutionMask.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, DependencyRegistrationRacingTerminalPublicationReleasesExactlyOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		constexpr uint32 IterationCount = 64;
		for (uint32 Iteration = 0; Iteration < IterationCount; ++Iteration)
		{
			std::barrier RaceBarrier(3);
			std::atomic<uint32> DependentExecutionCount = 0;
			Tasks::FTask Prerequisite = SubmitKernelTask("RegistrationRacePrerequisite", [&]() {
				RaceBarrier.arrive_and_wait();
			});
			Tasks::FTask Dependent;
			std::thread Submitter([&]() {
				RaceBarrier.arrive_and_wait();
				std::array<Tasks::FTask, 1> Prerequisites{Prerequisite};
				FTaskLaunchOptions Options;
				Options.Prerequisites = Prerequisites;
				Dependent = SubmitKernelTask("RegistrationRaceDependent", [&]() { DependentExecutionCount.fetch_add(1, std::memory_order::acq_rel); }, Options);
			});
			RaceBarrier.arrive_and_wait();
			Submitter.join();

			ASSERT_TRUE(Dependent.IsValid());
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Dependent).TaskState);
			EXPECT_EQ(1u, DependentExecutionCount.load(std::memory_order::acquire));
		}
	}

	TEST(FTaskTests, SimultaneousPrerequisiteCompletionReleasesFanInOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		std::barrier CompletionBarrier(3);
		Tasks::FTask First = SubmitKernelTask("SimultaneousFirst", [&]() {
			CompletionBarrier.arrive_and_wait();
		});
		Tasks::FTask Second = SubmitKernelTask("SimultaneousSecond", [&]() {
			CompletionBarrier.arrive_and_wait();
		});
		std::array<Tasks::FTask, 2> Prerequisites{First, Second};
		FTaskLaunchOptions Options;
		Options.Prerequisites = Prerequisites;
		std::atomic<uint32> ExecutionCount = 0;
		Tasks::FTask Dependent = SubmitKernelTask("SimultaneousFanIn", [&]() { ExecutionCount.fetch_add(1, std::memory_order::acq_rel); }, Options);
		ASSERT_TRUE(Dependent.IsValid());
		EXPECT_EQ(ETaskState::Waiting, Dependent.GetState());

		CompletionBarrier.arrive_and_wait();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Dependent).TaskState);
		EXPECT_EQ(1u, ExecutionCount.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, EmptyPrerequisitesAreIgnoredAndForeignLifetimeTasksAreRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		Tasks::FTask InvalidPrerequisite;
		std::array<Tasks::FTask, 1> InvalidPrerequisites{InvalidPrerequisite};
		FTaskLaunchOptions InvalidOptions;
		InvalidOptions.Prerequisites = InvalidPrerequisites;
		auto EmptyDependency = SubmitKernelTask("EmptyPrerequisiteTask", []() {}, InvalidOptions);
		ASSERT_EQ(EmptyDependency.Wait().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(EmptyDependency.GetDiagnostics().PrerequisiteTaskIds.empty());

		Tasks::FTask EarlierLifetimeTask = SubmitKernelTask("EarlierLifetimeTask", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(EarlierLifetimeTask).TaskState);
		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::array<Tasks::FTask, 1> ForeignPrerequisites{EarlierLifetimeTask};
		FTaskLaunchOptions ForeignOptions;
		ForeignOptions.Prerequisites = ForeignPrerequisites;
		EXPECT_DEATH({ (void)SubmitKernelTask("ForeignPrerequisiteTask", []() {}, ForeignOptions); }, "");
	}

	TEST(FTaskTests, FailedAndCanceledPrerequisitesReleaseOrdinaryDependents)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::atomic<bool> bFailedDependentRan = false;
		Tasks::FTask FailedPrerequisite = SubmitKernelTask("FailedPrerequisite", []() {
			throw std::runtime_error("dependency failure");
		});
		std::array<Tasks::FTask, 1> FailedPrerequisites{FailedPrerequisite};
		FTaskLaunchOptions FailedOptions;
		FailedOptions.Prerequisites = FailedPrerequisites;
		Tasks::FTask FailedDependent = SubmitKernelTask("FailedDependent", [&]() { bFailedDependentRan.store(true, std::memory_order::release); }, FailedOptions);

		EXPECT_EQ(ETaskState::Failed, WaitTask(FailedPrerequisite).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(FailedDependent).TaskState);
		EXPECT_TRUE(bFailedDependentRan.load(std::memory_order::acquire));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		Tasks::FTask Blocker = SubmitKernelTask("CancellationPropagationBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		Tasks::FTask CanceledPrerequisite = SubmitKernelTask("CanceledPrerequisite", []() {});
		std::array<Tasks::FTask, 1> CanceledPrerequisites{CanceledPrerequisite};
		FTaskLaunchOptions CanceledOptions;
		CanceledOptions.Prerequisites = CanceledPrerequisites;
		std::atomic<bool> bCanceledDependentRan = false;
		Tasks::FTask CanceledDependent = SubmitKernelTask("CanceledDependent", [&]() { bCanceledDependentRan.store(true, std::memory_order::release); }, CanceledOptions);

		EXPECT_TRUE(CancelTask(CanceledPrerequisite));
		EXPECT_FALSE(bCanceledDependentRan.load(std::memory_order::acquire));
		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(CanceledDependent).TaskState);
		EXPECT_TRUE(bCanceledDependentRan.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
	}

	TEST(FTaskTests, ExceptionWinsOverRunningCancellationAndLateCancellationIsIgnored)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent Started;
		FThreadEvent Release;
		std::atomic<bool> bTokenObservedCancellation = false;
		Tasks::FTask Task = SubmitCancelableKernelTask("ExceptionAfterCancellation", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			Release.Wait();
			bTokenObservedCancellation.store(Token.IsCancellationRequested(), std::memory_order::release);
			throw std::runtime_error("failure after cancellation");
		});
		ASSERT_TRUE(Started.WaitFor(1.0));
		EXPECT_TRUE(CancelTask(Task));
		Release.Trigger();
		EXPECT_EQ(ETaskState::Failed, WaitTask(Task).TaskState);
		EXPECT_TRUE(bTokenObservedCancellation.load(std::memory_order::acquire));
		EXPECT_EQ("failure after cancellation", Task.GetDiagnostic());
		EXPECT_FALSE(CancelTask(Task));
		EXPECT_EQ(ETaskState::Failed, Task.GetState());

		Tasks::FTask CompletedTask = SubmitKernelTask("CompletedBeforeCancellation", []() {});
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(CompletedTask).TaskState);
		EXPECT_FALSE(CancelTask(CompletedTask));
		EXPECT_EQ(ETaskState::Succeeded, CompletedTask.GetState());
	}

	TEST(FTaskTests, MultipleWaitersObserveTheSameOutcome)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent Started;
		FThreadEvent Release;
		Tasks::FTask Task = SubmitKernelTask("MultipleWaiters", [&]() {
			Started.Trigger();
			Release.Wait();
		});
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::array<ETaskState, 4> Outcomes;
		std::vector<std::thread> Waiters;
		for (ETaskState& Outcome : Outcomes)
		{
			Waiters.emplace_back([&Task, &Outcome]() {
				Outcome = WaitTask(Task).TaskState;
			});
		}
		Release.Trigger();
		for (std::thread& Waiter : Waiters)
		{
			Waiter.join();
		}
		for (ETaskState Outcome : Outcomes)
		{
			EXPECT_EQ(ETaskState::Succeeded, Outcome);
		}
	}

	TEST(FTaskTests, SelfWaitIsRejectedWithoutBlockingWorker)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		Tasks::FTask BlockingTask = SubmitKernelTask("SelfWaitBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));

		Tasks::FTask SelfTask;
		std::atomic<ETaskWaitStatus> ObservedWaitStatus = ETaskWaitStatus::InvalidTask;
		std::atomic<ETaskState> ObservedWaitState = ETaskState::Invalid;
		SelfTask = SubmitKernelTask("SelfWait", [&]() {
			const FTaskWaitResult WaitResult = WaitTask(SelfTask);
			ObservedWaitStatus.store(WaitResult.WaitStatus, std::memory_order::release);
			ObservedWaitState.store(WaitResult.TaskState, std::memory_order::release);
		});
		ReleaseBlockingTask.Trigger();

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(SelfTask).TaskState);
		EXPECT_EQ(ETaskWaitStatus::SelfWait, ObservedWaitStatus.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Running, ObservedWaitState.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(BlockingTask).TaskState);
	}

	TEST(FTaskTests, RenderingThreadJoinsExplicitIndependentCPUWork)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (const int Outcome : {0, 1, 2})
		{
			const bool Fail = Outcome == 1;
			FThreadEvent Started, Release, Returned;
			auto Blocker = SubmitKernelTask("IndependentQueueBlocker", [&] { Started.Trigger(); Release.Wait(); });
			ASSERT_TRUE(Started.WaitFor(1.0));
			uint32 ExecutionThreadId = 0;
			auto Task = Tasks::LaunchIndependentTask("IndependentRenderPreparation", [&, Fail] {
				ExecutionThreadId = FPlatformLTS::GetCurrentThreadId();
				if (Fail) throw std::runtime_error("independent task failure");
				return 42;
			});
			if (Outcome == 2) EXPECT_TRUE(Tasks::Cancel(Task));
			FWaitTaskRunnable Runnable(Durin::Tasks::FTask(Task), Returned);
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Runnable,
				"IndependentRenderingJoin", 0, EThreadPriority::Normal, EThreadRole::RenderingThread));
			EXPECT_NE(Thread, nullptr);
			if (Thread) EXPECT_TRUE(Returned.WaitFor(2.0));
			Release.Trigger();
			if (Thread)
			{
				EXPECT_TRUE(Returned.WaitFor(2.0));
				Thread->WaitForCompletion();
				if (Outcome != 2) EXPECT_EQ(ExecutionThreadId, Thread->GetThreadId());
				EXPECT_EQ(Runnable.ObservedResult.WaitStatus, ETaskWaitStatus::Completed);
				EXPECT_EQ(Runnable.ObservedResult.TaskState, Fail ? ETaskState::Failed : ETaskState::Succeeded);
			}
			EXPECT_EQ(WaitTask(Blocker).TaskState, ETaskState::Succeeded);
			if (Outcome == 0) EXPECT_EQ(Task.GetResult(), 42);
		}
	}

	TEST(FTaskTests, IndependentCPUContractRejectsDependenciesChildrenAndBlockingWaits)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		Tasks::FTaskEvent Pending("IndependentLeafGate");
		const Tasks::FTask PendingHandle = Durin::Tasks::FTask(Pending);
		for (int InvalidKind = 0; InvalidKind < 2; ++InvalidKind)
		{
			FTaskLaunchOptions Options;
			Options.bIndependentCPU = true;
			if (InvalidKind == 0) Options.Prerequisites = std::span<const Tasks::FTask>(&PendingHandle, 1);
			if (InvalidKind == 1) Options.Target = ETaskTarget::BlockingIO;
			EXPECT_DEATH({ (void)Private::LaunchCancelableTaskWithCompletion("InvalidIndependent",
				[](const FTaskCancellationToken&) {}, {}, Options); }, "");
		}
		EXPECT_DEATH({
			auto Invalid = Tasks::LaunchIndependentTask("ForbiddenChildParent", [] {
				(void)Tasks::LaunchTask("ForbiddenChild", [] {});
			});
			Invalid.Wait();
		}, "");
		auto Leaf = Tasks::LaunchIndependentTask("IndependentLeafContract", [&] {
			EXPECT_EQ(WaitTask(PendingHandle).WaitStatus, ETaskWaitStatus::UnsupportedThread);
			return true;
		});
		EXPECT_TRUE(Leaf.GetResult());
		EXPECT_DEATH({
			auto Invalid = Tasks::LaunchIndependentTask("ForbiddenNestedParent", [&] {
				Tasks::AddNested(Pending);
			});
			Invalid.Wait();
		}, "");
		Pending.Trigger();
		EXPECT_EQ(Pending.Wait().TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskTests, RenderingThreadWaitIsRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		Tasks::FTask BlockingTask = SubmitKernelTask("RenderingWaitBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		Tasks::FTask QueuedTask = SubmitKernelTask("RenderingWaitTarget", []() {});

		FThreadEvent WaitReturned;
		FWaitTaskRunnable Runnable(QueuedTask, WaitReturned);
		std::unique_ptr<FRunnableThread> RenderingThread(FRunnableThread::Create(&Runnable, "TaskWaitRenderingThread", 0, EThreadPriority::Normal, EThreadRole::RenderingThread));
		ASSERT_NE(RenderingThread, nullptr);
		ASSERT_TRUE(WaitReturned.WaitFor(1.0));
		RenderingThread->WaitForCompletion();

		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Runnable.ObservedResult.WaitStatus);
		EXPECT_EQ(ETaskState::Queued, Runnable.ObservedResult.TaskState);
		ReleaseBlockingTask.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(QueuedTask).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(BlockingTask).TaskState);
	}

	TEST(FTaskTests, RestartIsRejectedUntilShutdownCompletesAndOldHandleRemainsQueryable)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		Tasks::FTask BlockingTask = SubmitKernelTask("ShutdownBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		Tasks::FTask RetainedTask = SubmitKernelTask("RetainedAcrossRestart", []() {});
		ASSERT_TRUE(RetainedTask.IsValid());

		std::thread ShutdownThread([]() {
			ShutdownTaskScheduler(true);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}

		EXPECT_FALSE(InitializeTaskScheduler(1));
		EXPECT_DEATH({ (void)SubmitKernelTask("RejectedDuringShutdown", []() {}); }, "");

		ReleaseBlockingTask.Trigger();
		ShutdownThread.join();
		EXPECT_EQ(ETaskState::Succeeded, BlockingTask.GetState());
		EXPECT_EQ(ETaskState::Succeeded, RetainedTask.GetState());

		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTask NewTask = SubmitKernelTask("SequentialFixtureRestart", []() {});
		ASSERT_TRUE(NewTask.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(NewTask).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, RetainedTask.GetState());
	}

	TEST(FParallelForTests, PolicyWrapperOwnsUniqueCapturesForSerialAndParallelExecution)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		for (const auto Policy : {Tasks::EParallelForPolicy::Serial, Tasks::EParallelForPolicy::ExplicitBatch})
		{
			std::atomic<uint64> Sum = 0;
			std::atomic<uint32> Destroyed = 0;
			auto Delete = [&Destroyed](uint64* Value) { delete Value; ++Destroyed; };
			const auto Result = Tasks::ParallelFor("UniqueParallelCapture", 131,
				[Value = std::unique_ptr<uint64, decltype(Delete)>(new uint64(7), Delete), &Sum](uint64 Index) {
					Sum.fetch_add(Index + *Value, std::memory_order_relaxed);
				}, {.Policy = Policy, .BatchSize = 1});
			EXPECT_EQ(ETaskState::Succeeded, Result.State);
			EXPECT_EQ(Policy == Tasks::EParallelForPolicy::Serial ? 1u : 3u, Result.ChunkCount);
			EXPECT_EQ(131u * 130u / 2u + 131u * 7u, Sum.load());
			EXPECT_EQ(1u, Destroyed.load());
		}
	}

	TEST(FParallelForTests, CancelableUniqueCapturesReleaseAfterCancellationAndFailure)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		for (const bool bThrow : {false, true})
		{
			FTaskCancellationSource Cancellation;
			std::atomic<uint32> Destroyed = 0;
			auto Delete = [&Destroyed](uint64* Value) { delete Value; ++Destroyed; };
			FParallelForOptions Options;
			Options.MinBatchSize = 1;
			Options.CancellationToken = Cancellation.GetToken();
			const auto Result = ParallelForCancelable("UniqueCancelableCapture", 131,
				[Value = std::unique_ptr<uint64, decltype(Delete)>(new uint64(0), Delete), &Cancellation, bThrow]
				(uint64 Index, const FParallelForCancellationToken&) {
					if (Index != *Value) return;
					if (bThrow) throw std::runtime_error("unique capture failure");
					Cancellation.RequestCancellation();
				}, Options);
			EXPECT_EQ(bThrow ? ETaskState::Failed : ETaskState::Canceled, Result.State);
			EXPECT_EQ(3u, Result.ChunkCount);
			EXPECT_EQ(1u, Destroyed.load());
		}
	}

	TEST(FParallelForTests, CoversEdgeUnevenAndLargeRangesWithBoundedChunks)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));

		FParallelForOptions Options;
		Options.MinBatchSize = 4;
		for (uint64 Num : std::array<uint64, 6>{0, 1, 3, 4, 128, 131})
		{
			std::vector<uint32> Coverage(Num, 0);
			const FParallelForResult Result = ParallelFor("ExactCoverage", Num, [&](uint64 Index) {
				++Coverage[Index];
			}, Options);

			EXPECT_EQ(ETaskState::Succeeded, Result.State);
			EXPECT_LE(Result.ChunkCount, 5u);
			if (Num == 0)
			{
				EXPECT_EQ(0u, Result.ChunkCount);
			}
			else
			{
				EXPECT_GE(Result.ChunkCount, 1u);
			}
			for (uint32 ExecutionCount : Coverage)
			{
				EXPECT_EQ(1u, ExecutionCount);
			}
		}
	}

	TEST(FParallelForTests, NestedLoopsRunSeriallyWithoutLosingCoverage)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));

		constexpr uint64 OuterCount = 32;
		constexpr uint64 InnerCount = 7;
		std::vector<uint32> Coverage(OuterCount * InnerCount, 0);
		std::vector<uint32> NestedChunkCounts(OuterCount, 0);
		std::vector<ETaskState> NestedStates(OuterCount, ETaskState::Invalid);
		FParallelForOptions Options;
		Options.MinBatchSize = 1;
		const FParallelForResult OuterResult = ParallelFor("NestedOuter", OuterCount, [&](uint64 OuterIndex) {
			const FParallelForResult InnerResult = ParallelFor("NestedInner", InnerCount, [&](uint64 InnerIndex) {
				++Coverage[OuterIndex * InnerCount + InnerIndex];
			}, Options);
			NestedChunkCounts[OuterIndex] = InnerResult.ChunkCount;
			NestedStates[OuterIndex] = InnerResult.State;
		}, Options);

		EXPECT_EQ(ETaskState::Succeeded, OuterResult.State);
		EXPECT_EQ(5u, OuterResult.ChunkCount);
		for (uint32 ChunkCount : NestedChunkCounts)
		{
			EXPECT_EQ(1u, ChunkCount);
		}
		for (ETaskState State : NestedStates)
		{
			EXPECT_EQ(ETaskState::Succeeded, State);
		}
		for (uint32 ExecutionCount : Coverage)
		{
			EXPECT_EQ(1u, ExecutionCount);
		}
	}

	TEST(FParallelForTests, FailureAndCancellationNeverReportPartialCoverageAsSuccess)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));

		FParallelForOptions FineGrainedOptions;
		FineGrainedOptions.MinBatchSize = 1;
		std::barrier FailureBarrier(2);
		const FParallelForResult WorkerFailure = ParallelFor("StableWorkerFailure", 20, [&](uint64 Index) {
			if (Index == 4 || Index == 8)
			{
				FailureBarrier.arrive_and_wait();
				throw std::runtime_error("worker chunk failure");
			}
		}, FineGrainedOptions);
		EXPECT_EQ(ETaskState::Failed, WorkerFailure.State);
		EXPECT_NE(std::string::npos, WorkerFailure.Diagnostic.find("[4, 8)"));

		const FParallelForResult CallerFailure = ParallelFor("CallerFailure", 16, [](uint64 Index) {
			if (Index == 0)
			{
				throw std::runtime_error("caller chunk failure");
			}
		}, FineGrainedOptions);
		EXPECT_EQ(ETaskState::Failed, CallerFailure.State);
		EXPECT_NE(std::string::npos, CallerFailure.Diagnostic.find("[0, 4)"));

		FTaskCancellationSource CancellationSource;
		FParallelForOptions CancellationOptions = FineGrainedOptions;
		CancellationOptions.CancellationToken = CancellationSource.GetToken();
		FThreadEvent IterationStarted;
		FParallelForResult CancellationResult;
		std::atomic<uint64> StartedIterationCount = 0;
		std::thread ParallelThread([&]() {
			CancellationResult = ParallelForCancelable("CancelableParallelFor", 1024, [&](uint64, const FParallelForCancellationToken& Token) {
				StartedIterationCount.fetch_add(1, std::memory_order::acq_rel);
				IterationStarted.Trigger();
				while (!Token.IsCancellationRequested())
				{
					std::this_thread::yield();
				}
			}, CancellationOptions);
		});
		ASSERT_TRUE(IterationStarted.WaitFor(1.0));
		CancellationSource.RequestCancellation();
		ParallelThread.join();

		EXPECT_EQ(ETaskState::Canceled, CancellationResult.State);
		EXPECT_LE(CancellationResult.ChunkCount, 5u);
		EXPECT_LT(StartedIterationCount.load(std::memory_order::acquire), 1024u);
	}

	TEST(FTaskDiagnosticsTests, ReportsRelationshipsTimingCountersLongWaitsAndRetainedHandles)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		Tasks::FTask Blocker = SubmitKernelTask("DiagnosticBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		Tasks::FTask Queued = SubmitKernelTask("DiagnosticQueued", []() {});

		EXPECT_DEATH({ (void)Private::LaunchCancelableTaskWithCompletion("DiagnosticRejected", {}, {}, {}); }, "");

		FTaskSchedulerDiagnostics ActiveDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_TRUE(ActiveDiagnostics.bRunning);
		EXPECT_EQ(1u, ActiveDiagnostics.WorkerCount);
		EXPECT_EQ(1u, ActiveDiagnostics.ActiveWorkerCount);
		EXPECT_GE(ActiveDiagnostics.QueueDepth, 1u);
		EXPECT_EQ(2u, ActiveDiagnostics.NonterminalTaskCount);
		EXPECT_EQ(0u, ActiveDiagnostics.RejectedTaskCount);
		ASSERT_EQ(2u, ActiveDiagnostics.NonterminalTasks.size());

		FThreadEvent WaiterStarted;
		std::thread Waiter([&]() {
			WaiterStarted.Trigger();
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
		});
		ASSERT_TRUE(WaiterStarted.WaitFor(1.0));
		const auto LongWaitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().LongWaitCount == 0 && std::chrono::steady_clock::now() < LongWaitDeadline)
		{
			std::this_thread::yield();
		}
		const FTaskSchedulerDiagnostics LongWaitDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_TRUE(CancelTask(Queued));
		ReleaseBlocker.Trigger();
		Waiter.join();
		EXPECT_GE(LongWaitDiagnostics.LongWaitCount, 1u);
		EXPECT_EQ("ExternalThread", LongWaitDiagnostics.LastLongWaiterName);
		EXPECT_EQ(Blocker.GetTaskId(), LongWaitDiagnostics.LastLongWaitTargetTaskId);
		EXPECT_EQ("DiagnosticBlocker", LongWaitDiagnostics.LastLongWaitTargetName);
		EXPECT_EQ(ETaskState::Running, LongWaitDiagnostics.LastLongWaitTargetState);
		EXPECT_GE(LongWaitDiagnostics.LastLongWaitElapsedNanoseconds, 50'000'000u);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Queued).TaskState);

		FThreadEvent AllowParentWork;
		Tasks::FTask Child;
		Tasks::FTask Parent = SubmitKernelTask("DiagnosticParent", [&]() {
			AllowParentWork.Wait();
			Child = SubmitKernelTask("DiagnosticChild", []() {});
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Child).TaskState);
		});
		AllowParentWork.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Parent).TaskState);
		ASSERT_TRUE(Child.IsValid());
		const FTaskDiagnostics ParentDiagnostics = Parent.GetDiagnostics();
		const FTaskDiagnostics ChildDiagnostics = Child.GetDiagnostics();
		EXPECT_EQ(Parent.GetTaskId(), ChildDiagnostics.ParentTaskId);
		EXPECT_EQ(ETaskState::Succeeded, ChildDiagnostics.State);
		EXPECT_GT(ChildDiagnostics.EnqueueTimeNanoseconds, 0u);
		EXPECT_GE(ChildDiagnostics.StartTimeNanoseconds, ChildDiagnostics.EnqueueTimeNanoseconds);
		EXPECT_GE(ChildDiagnostics.FinishTimeNanoseconds, ChildDiagnostics.StartTimeNanoseconds);
		EXPECT_FALSE(ChildDiagnostics.ExecutingThreadName.empty());
		EXPECT_EQ(0u, ParentDiagnostics.ParentTaskId);

		Tasks::FTask Prerequisite = SubmitKernelTask("DiagnosticPrerequisite", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Prerequisite).TaskState);
		std::array<Tasks::FTask, 1> Prerequisites{Prerequisite};
		FTaskLaunchOptions DependentOptions;
		DependentOptions.Prerequisites = Prerequisites;
		Tasks::FTask Dependent = SubmitKernelTask("DiagnosticDependent", []() {}, DependentOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Dependent).TaskState);
		ASSERT_EQ(1u, Dependent.GetDiagnostics().PrerequisiteTaskIds.size());
		EXPECT_EQ(Prerequisite.GetTaskId(), Dependent.GetDiagnostics().PrerequisiteTaskIds[0]);

		Tasks::FTask Failed = SubmitKernelTask("DiagnosticFailure", []() {
			throw std::runtime_error("diagnostic failure");
		});
		EXPECT_EQ(ETaskState::Failed, WaitTask(Failed).TaskState);
		EXPECT_EQ("diagnostic failure", Failed.GetDiagnostics().Diagnostic);

		const FTaskSchedulerDiagnostics TerminalDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_GE(TerminalDiagnostics.CompletedTaskCount, 7u);
		EXPECT_EQ(1u, TerminalDiagnostics.FailedTaskCount);
		EXPECT_EQ(0u, TerminalDiagnostics.CanceledTaskCount);
		EXPECT_EQ(0u, TerminalDiagnostics.NonterminalTaskCount);

		ShutdownTaskScheduler(true);
		const FTaskSchedulerDiagnostics ShutdownDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_FALSE(ShutdownDiagnostics.bRunning);
		EXPECT_EQ(0u, ShutdownDiagnostics.NonterminalTaskCount);
		EXPECT_EQ(0u, ShutdownDiagnostics.ActiveWorkerCount);
		EXPECT_GE(ShutdownDiagnostics.RetainedTerminalHandleCount, 7u);
	}


	TEST(FTaskDiagnosticsTests, DeepSnapshotDoesNotBlockAdmissionTerminalPublicationOrShutdown)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FThreadEvent TaskStarted;
		FThreadEvent AllowTaskReturn;
		FThreadEvent SnapshotPinned;
		FThreadEvent ReleaseSnapshot;
		FThreadEvent CloseStarted;
		FThreadEvent ReleasePublication;
		FThreadEvent ShutdownFinished;
		FTaskSchedulerDiagnostics Snapshot;

		Tasks::FTask Running = SubmitKernelTask("SnapshotContentionRunning", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
		});
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		Tasks::FTask Waiting = SubmitKernelTask("SnapshotContentionWaiting", []() {}, {.Prerequisites = std::span(&Running, 1), .Attribution = Private::FTaskRuntimeAccess::GetAttribution(Running)});
		ASSERT_TRUE(Waiting.IsValid());

		FTaskSchedulerSnapshotTestHookGuard SnapshotHook([&]() {
			Private::SetTaskSchedulerSnapshotTestHook({});
			SnapshotPinned.Trigger();
			ReleaseSnapshot.Wait();
		});
		FTaskTerminalPublicationTestHookGuard TerminalHook([&](uint64 TaskId) {
			if (TaskId != Running.GetTaskId()) return;
			Private::SetTaskTerminalPublicationTestHook({});
			CloseStarted.Trigger();
			ReleasePublication.Wait();
		});

		std::thread SnapshotThread([&]() { Snapshot = GetTaskSchedulerDiagnostics(); });
		EXPECT_TRUE(SnapshotPinned.WaitFor(1.0));
		Tasks::FTask Root = SubmitKernelTask("SnapshotContentionRoot", []() {});
		Tasks::FTask LateContinuation = SubmitKernelTask("SnapshotContentionLateContinuation", []() {}, {.Prerequisites = std::span(&Running, 1), .Attribution = Private::FTaskRuntimeAccess::GetAttribution(Running)});
		EXPECT_TRUE(Root.IsValid());
		EXPECT_TRUE(LateContinuation.IsValid());

		AllowTaskReturn.Trigger();
		EXPECT_TRUE(CloseStarted.WaitFor(1.0));
		std::thread ShutdownThread([&]() {
			ShutdownTaskScheduler(true);
			ShutdownFinished.Trigger();
		});
		ReleasePublication.Trigger();
		const bool bShutdownFinishedWithoutSnapshot = ShutdownFinished.WaitFor(2.0);
		ReleaseSnapshot.Trigger();
		ShutdownThread.join();
		SnapshotThread.join();

		EXPECT_TRUE(bShutdownFinishedWithoutSnapshot);
		EXPECT_TRUE(Snapshot.bRunning);
		EXPECT_EQ(0u, Snapshot.NonterminalTaskCount);
		EXPECT_TRUE(Snapshot.NonterminalTasks.empty());
		EXPECT_FALSE(IsTaskSchedulerRunning());
	}


	TEST(FTaskCapacityTests, ConfigurationValidatesThresholdsWithoutChangingRunningScheduler)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		EXPECT_FALSE(InitializeTaskScheduler({.NumWorkerThreads = 1, .TaskWarningThreshold = 0}));
		EXPECT_FALSE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 0}));
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_EQ(16'384u, GetTaskSchedulerDiagnostics().TaskWarningThreshold);
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 7, .TaskWarningThreshold = 8}));
		const FTaskSchedulerDiagnostics Unchanged = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(1u, Unchanged.WorkerCount);
		EXPECT_EQ(16'384u, Unchanged.TaskWarningThreshold);
	}


	TEST(FTaskCapacityTests, ConcurrentProducersQueueBeyondWarningThreshold)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .TaskWarningThreshold = 8}));
		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		Tasks::FTask Blocker = SubmitKernelTask("CapacityConcurrentBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		std::mutex AcceptedMutex;
		std::vector<Tasks::FTask> Accepted;
		std::vector<std::thread> Producers;
		for (uint32 Index = 0; Index < 32; ++Index)
		{
			Producers.emplace_back([&]() {
				Tasks::FTask Task = SubmitKernelTask("CapacityConcurrentProducer", []() {});
				if (Task.IsValid())
				{
					std::lock_guard Lock(AcceptedMutex);
					Accepted.emplace_back(std::move(Task));
				}
			});
		}
		for (std::thread& Producer : Producers) Producer.join();
		const FTaskSchedulerDiagnostics Saturated = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(33u, Saturated.CurrentTrackedTaskCount);
		EXPECT_EQ(33u, Saturated.PeakTrackedTaskCount);
		EXPECT_EQ(32u, Accepted.size());

		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
		for (const Tasks::FTask& Task : Accepted) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		const auto ReservationReleaseDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount != 0
			&& std::chrono::steady_clock::now() < ReservationReleaseDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
	}

	TEST(FTaskCapacityTests, DrainAndCancelShutdownReleaseEveryReservation)
	{
		ShutdownTaskScheduler(false);
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .TaskWarningThreshold = 8}));
		FThreadEvent DrainStarted;
		FThreadEvent ReleaseDrain;
		Tasks::FTask DrainRoot = SubmitKernelTask("CapacityDrainRoot", [&]() {
			DrainStarted.Trigger();
			ReleaseDrain.Wait();
		});
		ASSERT_TRUE(DrainStarted.WaitFor(1.0));
		FTaskLaunchOptions DrainWaitingOptions;
		DrainWaitingOptions.Prerequisites = std::span<const Tasks::FTask>(&DrainRoot, 1);
		std::vector<Tasks::FTask> DrainTasks;
		for (uint32 Index = 0; Index < 7; ++Index)
		{
			DrainTasks.emplace_back(SubmitKernelTask("CapacityDrainWaiting", []() {}, DrainWaitingOptions));
		}
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
		ReleaseDrain.Trigger();
		ShutdownTaskScheduler(true);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().PeakTrackedTaskCount);

		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .TaskWarningThreshold = 8}));
		FThreadEvent Started;
		Tasks::FTask Running = SubmitCancelableKernelTask("CapacityCancelRunning", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			while (!Token.IsCancellationRequested()) std::this_thread::yield();
		});
		ASSERT_TRUE(Started.WaitFor(1.0));
		FTaskLaunchOptions WaitingOptions;
		WaitingOptions.Prerequisites = std::span<const Tasks::FTask>(&Running, 1);
		std::vector<Tasks::FTask> Waiting;
		for (uint32 Index = 0; Index < 7; ++Index) Waiting.emplace_back(SubmitKernelTask("CapacityCancelWaiting", []() {}, WaitingOptions));
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
		ShutdownTaskScheduler(false);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().PeakTrackedTaskCount);
		EXPECT_EQ(ETaskState::Succeeded, Running.GetState());
		// Canceling Running can unlock a waiter before shutdown visits that waiter.
		for (const Tasks::FTask& Task : Waiting)
			EXPECT_TRUE(Task.GetState() == ETaskState::Succeeded || Task.GetState() == ETaskState::Succeeded);
	}



	TEST(FTaskMoveOnlyCallableTests, CompletionOwnsUniqueCaptureUntilTerminalPublication)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<int> Calls = 0, Destroyed = 0;
		auto Delete = [&Destroyed](int* Value) { delete Value; ++Destroyed; };
		auto Admission = Private::LaunchCancelableTaskWithCompletion("UniqueCompletion",
			[](const FTaskCancellationToken&) {},
			[Capture = std::unique_ptr<int, decltype(Delete)>(new int(7), Delete), &Calls](ETaskState State) {
				EXPECT_EQ(ETaskState::Succeeded, State);
				EXPECT_EQ(7, *Capture);
				++Calls;
			}, {});
		ASSERT_TRUE(Admission.IsValid());
		auto Task = std::move(Admission);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		EXPECT_EQ(1, Calls.load());
		EXPECT_EQ(1, Destroyed.load());
	}

	TEST(FTaskMoveOnlyCallableTests, PortableCallbackPreservesEmptyAndMoveOnlySemantics)
	{
		using FCallback = TMoveOnlyFunction<int()>;
		static_assert(!std::is_copy_constructible_v<FCallback>);
		static_assert(!std::is_copy_assignable_v<FCallback>);
		static_assert(std::is_nothrow_move_constructible_v<FCallback>);
		// The native standard-library implementation may declare move
		// assignment potentially throwing (including MSVC 14.44).
		static_assert(std::is_move_assignable_v<FCallback>);

		int (*NullFunction)() = nullptr;
		FCallback Empty(NullFunction);
		EXPECT_FALSE(Empty);
		FCallback Null(nullptr);
		EXPECT_FALSE(Null);

		struct FReceiver { int Value = 9; };
		int FReceiver::* NullMember = nullptr;
		TMoveOnlyFunction<int&(FReceiver&)> EmptyMember(NullMember);
		EXPECT_FALSE(EmptyMember);
		TMoveOnlyFunction<int&(FReceiver&)> Member(&FReceiver::Value);
		FReceiver Receiver;
		Member(Receiver) = 12;
		EXPECT_EQ(12, Receiver.Value);

		auto Owner = std::make_shared<int>(7);
		std::weak_ptr<int> WeakOwner = Owner;
		FCallback Callback([Value = std::move(Owner)] { return *Value; });
		Empty = std::move(Callback);
		EXPECT_EQ(7, Empty());
		Empty = nullptr;
		EXPECT_TRUE(WeakOwner.expired());

		Empty = [Value = std::make_unique<int>(11)] { return *Value; };
		EXPECT_EQ(11, Empty());
	}

	TEST(FTaskMoveOnlyCallableTests, ErasureTransfersOwnershipAndDestroysExactlyOnce)
	{
		struct FTrackedCallable
		{
			std::shared_ptr<std::atomic<uint32>> DestructionCount;
			std::array<std::byte, 128> LargeCapture{};

			FTrackedCallable(std::shared_ptr<std::atomic<uint32>> InDestructionCount)
				: DestructionCount(std::move(InDestructionCount))
			{
			}
			FTrackedCallable(FTrackedCallable&&) noexcept = default;
			FTrackedCallable(const FTrackedCallable&) = delete;
			~FTrackedCallable()
			{
				if (DestructionCount) DestructionCount->fetch_add(1, std::memory_order::acq_rel);
			}
			auto operator()() -> void {}
		};

		using FMoveOnlyVoidFunction = TMoveOnlyFunction<void()>;
		FMoveOnlyVoidFunction Empty;
		EXPECT_FALSE(static_cast<bool>(Empty));

		auto ValueOwner = std::make_unique<int>(7);
		int* ValueAddress = ValueOwner.get();
		FMoveOnlyVoidFunction Small([Value = std::move(ValueOwner), &ValueAddress]() {
			EXPECT_EQ(ValueAddress, Value.get());
		});
		FMoveOnlyVoidFunction MovedSmall(std::move(Small));
		ASSERT_TRUE(static_cast<bool>(MovedSmall));
		MovedSmall();

		auto DestructionCount = std::make_shared<std::atomic<uint32>>(0);
		{
			FMoveOnlyVoidFunction Large{FTrackedCallable(DestructionCount)};
			FMoveOnlyVoidFunction MovedLarge(std::move(Large));
			MovedLarge();
		}
		EXPECT_EQ(1u, DestructionCount->load(std::memory_order::acquire));

		FMoveOnlyVoidFunction Throwing([]() { throw std::runtime_error("move-only failure"); });
		EXPECT_THROW(Throwing(), std::runtime_error);
	}


	TEST(FTaskMoveOnlyCallableTests, RunningCancellationHidesCompletionUntilCaptureDestructionFinishes)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started, ReleaseBody, Destroying, ReleaseDestruction;
		std::atomic<int> Destroyed = 0;
		auto Capture = std::shared_ptr<int>(new int(1), [&](int* Value) {
			Destroying.Trigger();
			EXPECT_TRUE(ReleaseDestruction.WaitFor(5.0));
			delete Value; ++Destroyed;
		});
		auto Task = SubmitKernelTask("RunningCaptureRelease", [Capture = std::move(Capture), &Started, &ReleaseBody] {
			Started.Trigger(); EXPECT_TRUE(ReleaseBody.WaitFor(5.0));
		});
		EXPECT_TRUE(Started.WaitFor(1.0));
		EXPECT_TRUE(CancelTask(Task));
		EXPECT_EQ(ETaskState::Running, Task.GetState());
		ReleaseBody.Trigger();
		EXPECT_TRUE(Destroying.WaitFor(1.0));
		EXPECT_EQ(ETaskState::Running, Task.GetState());
		EXPECT_EQ(0, Destroyed.load());
		ReleaseDestruction.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		EXPECT_EQ(1, Destroyed.load());
	}

	TEST(FGameThreadDeferredTaskTests, PriorityFifoAndItemBudgetAreDeterministic)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTask Root = SubmitKernelTask("DeferredPriorityRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		std::vector<int> Order;
		auto Enqueue = [&](int Value, ETaskPriority Priority) {
			FTaskLaunchOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.Priority = Priority;
			Options.EstimatedPayloadBytes = 1;
			Options.Prerequisites = std::span(&Root, 1);
			Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(Root);
			return SubmitKernelTask("DeferredPriorityEntry", [&, Value]() { Order.push_back(Value); }, Options);
		};
		Tasks::FTask Low = Enqueue(1, ETaskPriority::Low);
		Tasks::FTask HighFirst = Enqueue(2, ETaskPriority::High);
		Tasks::FTask Normal = Enqueue(3, ETaskPriority::Normal);
		Tasks::FTask HighSecond = Enqueue(4, ETaskPriority::High);

		FGameThreadDeferredPumpBudget TwoItems;
		TwoItems.MaxCallbacks = 2;
		TwoItems.MaxSeconds = 1.0;
		EXPECT_EQ(2u, PumpGameThreadDeferredWork(TwoItems).ExecutedCallbacks);
		EXPECT_EQ((std::vector<int>{2, 4}), Order);
		EXPECT_EQ(ETaskState::Queued, Normal.GetState());
		EXPECT_EQ(ETaskState::Queued, Low.GetState());

		FGameThreadDeferredPumpBudget Unlimited{.bUnlimited = true};
		EXPECT_EQ(2u, PumpGameThreadDeferredWork(Unlimited).ExecutedCallbacks);
		EXPECT_EQ((std::vector<int>{2, 4, 3, 1}), Order);
		EXPECT_EQ(ETaskState::Succeeded, HighFirst.GetState());
		EXPECT_EQ(ETaskState::Succeeded, HighSecond.GetState());
	}

	TEST(FGameThreadDeferredTaskTests, CountAndByteThresholdsDoNotRejectWork)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.QueuedEntriesWarningThreshold = 2;
		Config.QueuedBytesWarningThreshold = 10;
		Config.EntryBytesWarningThreshold = 8;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));
		Tasks::FTask Root = SubmitKernelTask("DeferredLimitRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		auto Enqueue = [&](uint64 Bytes) {
			FTaskLaunchOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = Bytes;
			Options.Prerequisites = std::span(&Root, 1);
			Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(Root);
			return SubmitKernelTask("DeferredLimitEntry", []() {}, Options);
		};
		Tasks::FTask First = Enqueue(5);
		Tasks::FTask Second = Enqueue(5);
		Tasks::FTask CountRejected = Enqueue(1);
		Tasks::FTask EntryRejected = Enqueue(9);
		Tasks::FTask ZeroRejected = Enqueue(0);

		EXPECT_EQ(ETaskState::Queued, First.GetState());
		EXPECT_EQ(ETaskState::Queued, Second.GetState());
		EXPECT_EQ(ETaskState::Queued, CountRejected.GetState());
		EXPECT_TRUE(EntryRejected.IsValid());
		EXPECT_TRUE(ZeroRejected.IsValid());
		EXPECT_TRUE(CancelTask(First));
		Tasks::FTask Replacement = Enqueue(5);
		EXPECT_EQ(ETaskState::Queued, Replacement.GetState());
		const auto Diagnostics = GetGameThreadDeferredWorkQueueDiagnostics();
		EXPECT_EQ(6u, Diagnostics.QueueDepth);
		EXPECT_GT(Diagnostics.QueuedPayloadBytes, 10u);
		EXPECT_EQ(0u, Diagnostics.RejectedCount);
		EXPECT_EQ(Diagnostics.CanceledCount, 0u);
		PumpGameThreadDeferredWork({.bUnlimited = true});
	}

	TEST(FGameThreadDeferredTaskTests, TimeBudgetStopsAfterAnOverBudgetCallback)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTask Root = SubmitKernelTask("DeferredTimeBudgetRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskLaunchOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		Options.Prerequisites = std::span(&Root, 1);
		Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(Root);
		Tasks::FTask Slow = SubmitKernelTask("DeferredSlowCallback", []() {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}, Options);
		Options.Prerequisites = std::span(&Root, 1);
		Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(Root);
		Tasks::FTask Later = SubmitKernelTask("DeferredLaterCallback", []() {}, Options);
		FGameThreadDeferredPumpBudget Budget;
		Budget.MaxCallbacks = 64;
		Budget.MaxSeconds = 0.0001;
		EXPECT_EQ(1u, PumpGameThreadDeferredWork(Budget).ExecutedCallbacks);
		EXPECT_EQ(ETaskState::Succeeded, Slow.GetState());
		EXPECT_EQ(ETaskState::Queued, Later.GetState());
		EXPECT_GE(GetGameThreadDeferredWorkQueueDiagnostics().LongCallbackCount, 1u);
		PumpGameThreadDeferredWork({.bUnlimited = true});
	}

	TEST(FGameThreadDeferredTaskTests, ReentrantPumpAndCallbackFailureRemainObservable)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTask Root = SubmitKernelTask("DeferredFailureRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskLaunchOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		Options.Prerequisites = std::span(&Root, 1);
		Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(Root);
		Tasks::FTask Failed = SubmitKernelTask("DeferredFailure", []() {
			EXPECT_EQ(0u, PumpGameThreadDeferredWork().ExecutedCallbacks);
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
			EXPECT_TRUE(IsTaskSchedulerRunning());
			throw std::runtime_error("deferred failure");
		}, Options);
		EXPECT_EQ(1u, PumpGameThreadDeferredWork().ExecutedCallbacks);
		EXPECT_EQ(ETaskState::Failed, Failed.GetState());
		EXPECT_EQ("deferred failure", Failed.GetDiagnostic());
		const auto Diagnostics = GetGameThreadDeferredWorkQueueDiagnostics();
		EXPECT_EQ(1u, Diagnostics.ReentrantPumpCount);
		EXPECT_EQ(1u, Diagnostics.CallbackFailureCount);
	}

	TEST(FGameThreadDeferredTaskTests, CrossExecutorDrainAndCancelLeaveEveryHandleTerminal)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		const uint64 DrainAdapterGeneration = GetGameThreadDeferredWorkQueueDiagnostics().AdapterGeneration;

		FThreadEvent DrainRootStarted;
		FThreadEvent ReleaseDrainRoot;
		Tasks::FTask DrainRoot = SubmitKernelTask("CrossDrainRoot", [&]() {
			DrainRootStarted.Trigger();
			ReleaseDrainRoot.Wait();
		});
		FTaskLaunchOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		std::atomic<bool> bDrainRan = false;
		Options.Prerequisites = std::span(&DrainRoot, 1);
		Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(DrainRoot);
		Tasks::FTask DrainTail = SubmitKernelTask("CrossDrainTail", [&]() {
			bDrainRan = true;
			EXPECT_DEATH({ (void)SubmitKernelTask("RejectedShutdownRoot", []() {}); }, "");
		}, Options);
		ASSERT_TRUE(DrainRootStarted.WaitFor(1.0));
		std::thread Releaser([&]() { ReleaseDrainRoot.Trigger(); });
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		Releaser.join();
		EXPECT_EQ(ETaskState::Succeeded, DrainRoot.GetState());
		EXPECT_EQ(ETaskState::Succeeded, DrainTail.GetState());
		EXPECT_TRUE(bDrainRan.load());
		EXPECT_FALSE(GetGameThreadDeferredWorkQueueDiagnostics().bInstalled);

		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		EXPECT_GT(GetGameThreadDeferredWorkQueueDiagnostics().AdapterGeneration, DrainAdapterGeneration);
		Tasks::FTask CancelRoot = SubmitKernelTask("CrossCancelRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(CancelRoot).TaskState);
		Options.Prerequisites = std::span(&CancelRoot, 1);
		Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(CancelRoot);
		Tasks::FTask CancelTail = SubmitKernelTask("CrossCancelTail", []() {}, Options);
		ASSERT_EQ(ETaskState::Queued, CancelTail.GetState());
		ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		EXPECT_EQ(ETaskState::Succeeded, CancelTail.GetState());
		EXPECT_EQ(ETaskTerminalReason::None, CancelTail.GetDiagnostics().TerminalReason);
		EXPECT_TRUE(CancelTail.IsCancellationRequested());
	}

	TEST(FGameThreadDeferredTaskTests, DeferredCallbacksPreserveAccounting)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.QueuedEntriesWarningThreshold = 256;
		Config.QueuedBytesWarningThreshold = 256 * 64;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));
		Tasks::FTask Root = SubmitKernelTask("DeferredAccountingRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		constexpr uint32 CallbackCount = 16;
		constexpr uint64 DeclaredCaptureBytes = 64;
		std::atomic<uint32> ExecutedCount = 0;
		std::vector<Tasks::FTask> Handles;
		Handles.reserve(CallbackCount);
		for (uint32 Index = 0; Index < CallbackCount; ++Index)
		{
			FTaskLaunchOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = DeclaredCaptureBytes;
			Options.Prerequisites = std::span(&Root, 1);
			Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(Root);
			Handles.emplace_back(SubmitKernelTask("DeferredAccountingCallback", [&]() {
				ExecutedCount.fetch_add(1, std::memory_order::acq_rel);
			}, Options));
		}

		const FGameThreadDeferredWorkQueueDiagnostics BeforePump = GetGameThreadDeferredWorkQueueDiagnostics();
		ASSERT_EQ(CallbackCount, BeforePump.QueueDepth);
		ASSERT_GT(BeforePump.QueuedPayloadBytes, CallbackCount * DeclaredCaptureBytes);
		const FGameThreadDeferredPumpResult Pump = PumpGameThreadDeferredWork({.bUnlimited = true});
		ASSERT_EQ(CallbackCount, Pump.ExecutedCallbacks);
		EXPECT_EQ(CallbackCount, ExecutedCount.load(std::memory_order::acquire));

		uint32 SucceededCount = 0;
		for (const Tasks::FTask& Handle : Handles)
		{
			const FTaskDiagnostics Diagnostics = Handle.GetDiagnostics();
			SucceededCount += Diagnostics.State == ETaskState::Succeeded;
		}
		EXPECT_EQ(CallbackCount, SucceededCount);
	}

	TEST(FTaskTests, InvalidHandlesAreNoOp)
	{
		Tasks::FTask InvalidHandle;

		EXPECT_FALSE(InvalidHandle.IsValid());
		EXPECT_TRUE(InvalidHandle.IsCompleted());
		EXPECT_EQ(ETaskState::Invalid, InvalidHandle.GetState());
		EXPECT_STREQ("", InvalidHandle.GetDebugName());

		const FTaskWaitResult WaitResult = WaitTask(InvalidHandle);
		EXPECT_EQ(ETaskWaitStatus::Completed, WaitResult.WaitStatus);
		EXPECT_EQ(ETaskState::Invalid, WaitResult.TaskState);
		const std::vector<FTaskWaitResult> WaitResults = WaitAll(std::span<const Tasks::FTask>(&InvalidHandle, 1));
		ASSERT_EQ(1u, WaitResults.size());
		EXPECT_EQ(ETaskWaitStatus::Completed, WaitResults[0].WaitStatus);
		EXPECT_EQ(ETaskState::Invalid, WaitResults[0].TaskState);
	}

	TEST(FTaskTests, LaunchTaskFailsConstructionWhenSchedulerIsStopped)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;

		EXPECT_DEATH({ (void)SubmitKernelTask("RejectedTask", []() {}); }, "");
	}
	TEST(FTaskDiagnosticsTests, ClosingPublishesResultDiagnosticsAndDependentsTogether)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FThreadEvent TaskStarted;
		FThreadEvent AllowTaskReturn;
		FThreadEvent CloseStarted;
		FThreadEvent ReleasePublication;
		std::atomic<uint64> HookTaskId = 0;
		std::atomic<bool> bDependentRan = false;
		FTaskTerminalPublicationTestHookGuard HookGuard([&](uint64 TaskId) {
			Private::SetTaskTerminalPublicationTestHook({});
			HookTaskId.store(TaskId, std::memory_order::release);
			CloseStarted.Trigger();
			ReleasePublication.Wait();
		});

		auto Producer = Tasks::LaunchTask("PublicationBarrierProducer", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
			return 42;
		});
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		Tasks::FTask Dependent = Durin::Tasks::FTask(Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.DebugName = "PublicationBarrierDependent"}, [&, Producer]() {
			const auto& Value = Producer.GetResult();
			EXPECT_EQ(42, Value);
			bDependentRan.store(true, std::memory_order::release);
		}, Producer));
		AllowTaskReturn.Trigger();
		ASSERT_TRUE(CloseStarted.WaitFor(1.0));
		ASSERT_EQ(Producer.GetTaskId(), HookTaskId.load(std::memory_order::acquire));

		const FTaskDiagnostics DuringPublication = Producer.GetDiagnostics();
		EXPECT_EQ(ETaskState::Running, Producer.GetState());
		EXPECT_EQ(ETaskState::Running, DuringPublication.State);
		EXPECT_EQ(0u, DuringPublication.FinishTimeNanoseconds);
		EXPECT_EQ(ETaskTerminalReason::None, DuringPublication.TerminalReason);
		EXPECT_TRUE(DuringPublication.Diagnostic.empty());
		EXPECT_EQ(nullptr, Producer.GetResultShared());
		EXPECT_EQ(ETaskState::Waiting, Dependent.GetState());
		EXPECT_FALSE(bDependentRan.load(std::memory_order::acquire));

		const FTaskSchedulerDiagnostics SchedulerDuringPublication = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(2u, SchedulerDuringPublication.NonterminalTaskCount);
		EXPECT_EQ(0u, SchedulerDuringPublication.RetainedTerminalHandleCount);
		EXPECT_EQ(0u, SchedulerDuringPublication.RetainedTerminalResultCount);

		ReleasePublication.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Dependent).TaskState);
		ASSERT_NE(nullptr, Producer.GetResultShared());
		EXPECT_EQ(42, *Producer.GetResultShared());
		EXPECT_TRUE(bDependentRan.load(std::memory_order::acquire));
		const FTaskDiagnostics Published = Producer.GetDiagnostics();
		EXPECT_EQ(ETaskState::Succeeded, Published.State);
		EXPECT_GT(Published.FinishTimeNanoseconds, 0u);
		EXPECT_TRUE(Published.bHasResultStorage);

		const uint64 ExpectedCompletedCount = SchedulerDuringPublication.CompletedTaskCount + 2;
		while (GetTaskSchedulerDiagnostics().CompletedTaskCount < ExpectedCompletedCount)
		{
			std::this_thread::yield();
		}
		FTaskSchedulerDiagnostics Retained = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(0u, Retained.NonterminalTaskCount);
		EXPECT_EQ(2u, Retained.RetainedTerminalHandleCount);
		EXPECT_EQ(1u, Retained.RetainedTerminalResultCount);

		Producer = {};
		Dependent = {};
		const auto RetentionDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(1);
		do
		{
			Retained = GetTaskSchedulerDiagnostics();
			if (Retained.RetainedTerminalHandleCount == 0
				&& Retained.RetainedTerminalResultCount == 0)
			{
				break;
			}
			std::this_thread::yield();
		}
		while (std::chrono::steady_clock::now() < RetentionDeadline);
		EXPECT_EQ(0u, Retained.RetainedTerminalHandleCount);
		EXPECT_EQ(0u, Retained.RetainedTerminalResultCount);
	}

	TEST(FTaskDiagnosticsTests, NodeOwnsResultStorageBeforeExecutionAndAfterCompletion)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("ResultOwnershipGate", false);
		auto Storage = std::make_shared<TTaskResultState<int>>();
		std::weak_ptr<TTaskResultState<int>> Weak = Storage;
		const std::array<Tasks::FTask, 1> Prerequisites{Gate};
		auto Admission = Private::LaunchCancelableTaskWithCompletion("OwnedResult",
			[Weak](const FTaskCancellationToken&) {
				auto Pinned = Weak.lock(); ASSERT_NE(nullptr, Pinned); Pinned->SetPending(42);
			},
			[Weak](ETaskState Terminal) {
				auto Pinned = Weak.lock(); ASSERT_NE(nullptr, Pinned); Pinned->Complete(Terminal);
			},
			{.Prerequisites = Prerequisites}, 0, {Storage, true});
		ASSERT_TRUE(Admission.IsValid());
		auto Task = std::move(Admission);
		Storage.reset();
		EXPECT_FALSE(Weak.expired());
		Gate.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		ASSERT_NE(nullptr, Weak.lock());
		EXPECT_EQ(42, *Weak.lock()->PeekPublished());
		EXPECT_TRUE(Task.GetDiagnostics().bHasResultStorage);
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);
		ShutdownTaskScheduler(true);
		Task = {};
		EXPECT_TRUE(Weak.expired());
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);
	}

	TEST(FTaskDiagnosticsTests, LifetimeCountersRemainIsolatedAcrossStoppedAndRestartedSchedulers)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto PreviousLifetime = Tasks::LaunchTask("PreviousLifetime", []() { return 7; });
		auto ReleasedWhileStopped = Tasks::LaunchTask("ReleasedWhileStopped", []() { return 8; });
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Durin::Tasks::FTask(PreviousLifetime)).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Durin::Tasks::FTask(ReleasedWhileStopped)).TaskState);
		while (GetTaskSchedulerDiagnostics().CompletedTaskCount < 2)
		{
			std::this_thread::yield();
		}
		ShutdownTaskScheduler(true);
		EXPECT_EQ(2u, GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount);
		EXPECT_EQ(2u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);
		ReleasedWhileStopped = {};
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount);
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);

		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);
		PreviousLifetime = {};
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);
	}

}
