#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <iostream>

#include "Threading/QueuedThreadPool.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	namespace
	{
		class FSignalRunnable final : public FRunnable
		{
		public:
			explicit FSignalRunnable(FThreadEvent& InStartedEvent)
				: StartedEvent(InStartedEvent)
			{
			}

			auto Run() -> uint32 override
			{
				ObservedThreadName = GetCurrentThreadName();
				bObservedWorkerThread = IsInWorkerThread();
				bObservedTaskThread = IsInTaskThread();
				StartedEvent.Trigger();
				return 0;
			}

			FThreadEvent& StartedEvent;
			std::string ObservedThreadName;
			bool bObservedWorkerThread = false;
			bool bObservedTaskThread = false;
		};

		class FCooperativeStopRunnable final : public FRunnable
		{
		public:
			auto Run() -> uint32 override
			{
				StartedEvent.Trigger();
				StopEvent.Wait();
				return 0;
			}

			auto Stop() -> void override
			{
				bStopCalled.store(true, std::memory_order::release);
				StopEvent.Trigger();
			}

			FThreadEvent StartedEvent;
			FThreadEvent StopEvent;
			std::atomic<bool> bStopCalled = false;
		};

		class FWaitTaskRunnable final : public FRunnable
		{
		public:
			FWaitTaskRunnable(FTaskHandle InTarget, FThreadEvent& InReturnedEvent)
				: Target(std::move(InTarget))
				, ReturnedEvent(InReturnedEvent)
			{
			}

			auto Run() -> uint32 override
			{
				ObservedState = WaitTask(Target);
				ReturnedEvent.Trigger();
				return 0;
			}

			FTaskHandle Target;
			FThreadEvent& ReturnedEvent;
			ETaskState ObservedState = ETaskState::Invalid;
		};

		class FEngineThreadPoolTestGuard
		{
		public:
			~FEngineThreadPoolTestGuard()
			{
				ShutdownTaskScheduler(false);
			}
		};
	} // namespace

	TEST(FThreadEventTests, StartsUnsignaledAndWakesAfterTrigger)
	{
		FThreadEvent Event;

		EXPECT_FALSE(Event.IsTriggered());
		EXPECT_FALSE(Event.WaitFor(0.001));

		Event.Trigger();

		EXPECT_TRUE(Event.IsTriggered());
		EXPECT_TRUE(Event.WaitFor(0.001));
	}

	TEST(FThreadEventTests, ResetReturnsEventToUnsignaledState)
	{
		FThreadEvent Event;
		Event.Trigger();

		ASSERT_TRUE(Event.IsTriggered());

		Event.Reset();

		EXPECT_FALSE(Event.IsTriggered());
		EXPECT_FALSE(Event.WaitFor(0.001));
	}

	TEST(FRunnableThreadTests, ReportsThreadNameAndWorkerRole)
	{
		FThreadEvent StartedEvent;
		FSignalRunnable Runnable(StartedEvent);
		std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Runnable, "ThreadingTestWorker", 0, EThreadPriority::Normal, EThreadRole::WorkerThread));

		ASSERT_NE(Thread, nullptr);
		ASSERT_TRUE(StartedEvent.WaitFor(1.0));
		Thread->WaitForCompletion();

		EXPECT_STREQ("ThreadingTestWorker", Runnable.ObservedThreadName.c_str());
		EXPECT_TRUE(Runnable.bObservedWorkerThread);
		EXPECT_TRUE(Runnable.bObservedTaskThread);
		EXPECT_EQ(EThreadRole::WorkerThread, Thread->GetThreadRole());
		EXPECT_NE(0u, Thread->GetThreadId());
	}

	TEST(FRunnableThreadTests, KillRequestsCooperativeStopAndWaits)
	{
		FCooperativeStopRunnable Runnable;
		std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Runnable, "ThreadingTestStop", 0, EThreadPriority::Normal, EThreadRole::WorkerThread));

		ASSERT_NE(Thread, nullptr);
		ASSERT_TRUE(Runnable.StartedEvent.WaitFor(1.0));

		Thread->Kill(true);

		EXPECT_TRUE(Runnable.bStopCalled.load(std::memory_order::acquire));
	}

	TEST(FRunnableThreadTests, WaitForCompletionJoinsNaturallyFinishedThread)
	{
		FThreadEvent StartedEvent;
		FSignalRunnable Runnable(StartedEvent);
		std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Runnable, "ThreadingTestJoin", 0, EThreadPriority::Normal, EThreadRole::WorkerThread));

		ASSERT_NE(Thread, nullptr);
		ASSERT_TRUE(StartedEvent.WaitFor(1.0));

		Thread->WaitForCompletion();
		Thread->WaitForCompletion();

		EXPECT_TRUE(Runnable.bObservedWorkerThread);
	}

	TEST(FRunnableThreadTests, DestructionCooperativelyStopsJoinableThread)
	{
		FCooperativeStopRunnable Runnable;
		std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Runnable, "ThreadingTestDestructor", 0, EThreadPriority::Normal, EThreadRole::WorkerThread));

		ASSERT_NE(Thread, nullptr);
		ASSERT_TRUE(Runnable.StartedEvent.WaitFor(1.0));

		Thread.reset();

		EXPECT_TRUE(Runnable.bStopCalled.load(std::memory_order::acquire));
	}

	TEST(FRunnableThreadTests, RejectsUnsupportedStackSizeAndPriority)
	{
		FThreadEvent StartedEvent;
		FSignalRunnable Runnable(StartedEvent);

		std::unique_ptr<FRunnableThread> StackThread(FRunnableThread::Create(&Runnable, "UnsupportedStack", 64 * 1024, EThreadPriority::Normal, EThreadRole::WorkerThread));
		std::unique_ptr<FRunnableThread> PriorityThread(FRunnableThread::Create(&Runnable, "UnsupportedPriority", 0, EThreadPriority::AboveNormal, EThreadRole::WorkerThread));

		EXPECT_EQ(nullptr, StackThread);
		EXPECT_EQ(nullptr, PriorityThread);
		EXPECT_FALSE(StartedEvent.IsTriggered());
	}

	TEST(FQueuedThreadPoolTests, StartsFixedWorkersAndShutsDownEmpty)
	{
		FQueuedThreadPool Pool;

		ASSERT_TRUE(Pool.Create(2, "QueuedPoolEmpty"));
		EXPECT_TRUE(Pool.IsRunning());
		EXPECT_EQ(2u, Pool.GetNumThreads());
		EXPECT_EQ(0u, Pool.GetNumQueuedTasks());

		Pool.Destroy(true);

		EXPECT_FALSE(Pool.IsRunning());
		EXPECT_EQ(0u, Pool.GetNumThreads());
	}

	TEST(FQueuedThreadPoolTests, PartialWorkerCreationFailureCleansUpAndAllowsReuse)
	{
		FQueuedThreadPool Pool;

		EXPECT_FALSE(Pool.Create(3, "QueuedPoolPartialFailure", 1));
		EXPECT_FALSE(Pool.IsRunning());
		EXPECT_EQ(0u, Pool.GetNumThreads());

		ASSERT_TRUE(Pool.Create(1, "QueuedPoolRecovered"));
		Pool.Destroy(true);
	}

	TEST(FQueuedThreadPoolTests, SubmittedTasksRunOnWorkerThreads)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1, "QueuedPoolRoles"));

		FThreadEvent TaskFinished;
		std::atomic<bool> bObservedWorkerThread = false;
		std::atomic<bool> bObservedTaskThread = false;
		std::string ObservedThreadName;
		std::mutex ObservedThreadNameMutex;

		ASSERT_TRUE(Pool.Enqueue("ObserveWorkerRole", [&]() {
			bObservedWorkerThread.store(IsInWorkerThread(), std::memory_order::release);
			bObservedTaskThread.store(IsInTaskThread(), std::memory_order::release);
			{
				std::lock_guard Lock(ObservedThreadNameMutex);
				ObservedThreadName = GetCurrentThreadName();
			}
			TaskFinished.Trigger();
		}));

		ASSERT_TRUE(TaskFinished.WaitFor(1.0));
		Pool.WaitForIdle();
		Pool.Destroy(true);

		EXPECT_TRUE(bObservedWorkerThread.load(std::memory_order::acquire));
		EXPECT_TRUE(bObservedTaskThread.load(std::memory_order::acquire));
		std::lock_guard Lock(ObservedThreadNameMutex);
		EXPECT_EQ("QueuedPoolRoles-0", ObservedThreadName);
	}

	TEST(FQueuedThreadPoolTests, ManyTasksCompleteAfterWaitForIdle)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(4, "QueuedPoolMany"));

		constexpr uint32 TaskCount = 128;
		std::atomic<uint32> CompletedTaskCount = 0;

		for (uint32 TaskIndex = 0; TaskIndex < TaskCount; ++TaskIndex)
		{
			ASSERT_TRUE(Pool.Enqueue("CountTask", [&]() {
				CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}));
		}

		Pool.WaitForIdle();
		Pool.Destroy(true);

		EXPECT_EQ(TaskCount, CompletedTaskCount.load(std::memory_order::acquire));
	}

	TEST(FQueuedThreadPoolTests, MultipleProducerThreadsCanEnqueueConcurrently)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(4, "QueuedPoolProducers"));

		constexpr uint32 ProducerCount = 4;
		constexpr uint32 TasksPerProducer = 32;
		std::atomic<uint32> AcceptedTaskCount = 0;
		std::atomic<uint32> CompletedTaskCount = 0;
		std::vector<std::thread> Producers;

		for (uint32 ProducerIndex = 0; ProducerIndex < ProducerCount; ++ProducerIndex)
		{
			Producers.emplace_back([&]() {
				for (uint32 TaskIndex = 0; TaskIndex < TasksPerProducer; ++TaskIndex)
				{
					if (Pool.Enqueue("ConcurrentTask", [&]() {
							CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
						}))
					{
						AcceptedTaskCount.fetch_add(1, std::memory_order::acq_rel);
					}
				}
			});
		}

		for (std::thread& Producer : Producers)
		{
			Producer.join();
		}

		Pool.WaitForIdle();
		Pool.Destroy(true);

		EXPECT_EQ(ProducerCount * TasksPerProducer, AcceptedTaskCount.load(std::memory_order::acquire));
		EXPECT_EQ(AcceptedTaskCount.load(std::memory_order::acquire), CompletedTaskCount.load(std::memory_order::acquire));
	}

	TEST(FQueuedThreadPoolTests, DestroyTrueDrainsPendingWork)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1, "QueuedPoolDrain"));

		constexpr uint32 TaskCount = 16;
		std::atomic<uint32> CompletedTaskCount = 0;

		for (uint32 TaskIndex = 0; TaskIndex < TaskCount; ++TaskIndex)
		{
			ASSERT_TRUE(Pool.Enqueue("DrainTask", [&]() {
				CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}));
		}

		Pool.Destroy(true);

		EXPECT_FALSE(Pool.IsRunning());
		EXPECT_EQ(TaskCount, CompletedTaskCount.load(std::memory_order::acquire));
		EXPECT_FALSE(Pool.Enqueue("RejectedAfterDrain", []() {}));
	}

	TEST(FQueuedThreadPoolTests, StopAcceptingWorkRejectsNewTasksAndDrainsAcceptedTasks)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1, "QueuedPoolQuiesce"));

		std::atomic<uint32> CompletedTaskCount = 0;
		for (uint32 TaskIndex = 0; TaskIndex < 8; ++TaskIndex)
		{
			ASSERT_TRUE(Pool.Enqueue("AcceptedBeforeQuiesce", [&]() {
				CompletedTaskCount.fetch_add(1, std::memory_order_acq_rel);
			}));
		}

		Pool.StopAcceptingWork();
		EXPECT_FALSE(Pool.IsRunning());
		EXPECT_FALSE(Pool.Enqueue("RejectedAfterQuiesce", []() {}));
		Pool.WaitForIdle();
		EXPECT_EQ(8u, CompletedTaskCount.load(std::memory_order_acquire));
		Pool.Destroy(true);
	}

	TEST(FQueuedThreadPoolTests, DestroyFalseDiscardsQueuedWorkAndRejectsLaterWork)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1, "QueuedPoolDiscard"));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		std::atomic<uint32> ExecutedTaskCount = 0;

		ASSERT_TRUE(Pool.Enqueue("BlockingTask", [&]() {
			ExecutedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		}));

		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));

		constexpr uint32 QueuedTaskCount = 8;
		for (uint32 TaskIndex = 0; TaskIndex < QueuedTaskCount; ++TaskIndex)
		{
			ASSERT_TRUE(Pool.Enqueue("DiscardedTask", [&]() {
				ExecutedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}));
		}

		std::thread DestroyThread([&]() {
			Pool.Destroy(false);
		});

		while (Pool.IsRunning())
		{
			std::this_thread::yield();
		}

		EXPECT_FALSE(Pool.Enqueue("RejectedDuringDestroy", []() {}));
		ReleaseBlockingTask.Trigger();
		DestroyThread.join();

		EXPECT_FALSE(Pool.IsRunning());
		EXPECT_EQ(1u, ExecutedTaskCount.load(std::memory_order::acquire));
		EXPECT_FALSE(Pool.Enqueue("RejectedAfterDestroy", []() {}));
	}

	TEST(FQueuedThreadPoolTests, DestroyFalseInvokesDiscardCallbackExactlyOnce)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1, "QueuedPoolDiscardCallback"));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		std::atomic<uint32> DiscardCount = 0;
		ASSERT_TRUE(Pool.Enqueue("BlockingTask", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		}));
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));

		ASSERT_TRUE(Pool.Enqueue("DiscardedTask", []() {}, [&]() { DiscardCount.fetch_add(1, std::memory_order::acq_rel); }));

		std::thread DestroyThread([&]() {
			Pool.Destroy(false);
		});
		while (Pool.IsRunning())
		{
			std::this_thread::yield();
		}

		ReleaseBlockingTask.Trigger();
		DestroyThread.join();

		EXPECT_EQ(1u, DiscardCount.load(std::memory_order::acquire));
	}

	TEST(FQueuedThreadPoolTests, CallableExceptionRestoresIdleBookkeeping)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1, "QueuedPoolException"));

		ASSERT_TRUE(Pool.Enqueue("ThrowingWork", []() {
			throw std::runtime_error("queued failure");
		}));

		EXPECT_TRUE(Pool.WaitForIdle());
		Pool.Destroy(true);
	}

	TEST(FQueuedThreadPoolTests, WorkerIdleWaitOnSamePoolIsRejected)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1, "QueuedPoolWorkerIdleWait"));

		FThreadEvent TaskFinished;
		std::atomic<bool> bWaitResult = true;
		ASSERT_TRUE(Pool.Enqueue("WorkerIdleWait", [&]() {
			bWaitResult.store(Pool.WaitForIdle(), std::memory_order::release);
			TaskFinished.Trigger();
		}));

		ASSERT_TRUE(TaskFinished.WaitFor(1.0));
		EXPECT_FALSE(bWaitResult.load(std::memory_order::acquire));
		Pool.Destroy(true);
	}

	TEST(FQueuedThreadPoolTests, WaitForIdleReturnsForIdleRunningPool)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(2, "QueuedPoolIdle"));

		EXPECT_TRUE(Pool.WaitForIdle());
		EXPECT_TRUE(Pool.IsRunning());

		Pool.Destroy(true);
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
		FTaskHandle Handle = LaunchTask("EnginePoolTask", [&]() {
			bObservedWorkerThread.store(IsInWorkerThread(), std::memory_order::release);
			TaskFinished.Trigger();
		});
		ASSERT_TRUE(Handle.IsValid());

		ASSERT_TRUE(TaskFinished.WaitFor(1.0));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Handle));

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

		FTaskHandle Handle = LaunchTask("IdempotentInitialization", []() {});
		ASSERT_TRUE(Handle.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Handle));
	}

	TEST(FTaskSchedulerTests, ConcurrentAdmissionCloseEitherRejectsOrCompletesEverySubmission)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		constexpr uint32 ProducerCount = 8;
		std::barrier StartBarrier(ProducerCount + 1);
		std::array<FTaskHandle, ProducerCount> Handles;
		std::vector<std::thread> Producers;
		Producers.reserve(ProducerCount);
		for (uint32 ProducerIndex = 0; ProducerIndex < ProducerCount; ++ProducerIndex)
		{
			Producers.emplace_back([&, ProducerIndex]() {
				StartBarrier.arrive_and_wait();
				Handles[ProducerIndex] = LaunchTask("AdmissionRace", []() {});
			});
		}

		StartBarrier.arrive_and_wait();
		ShutdownTaskScheduler(true);
		for (std::thread& Producer : Producers)
		{
			Producer.join();
		}

		for (const FTaskHandle& Handle : Handles)
		{
			if (Handle.IsValid())
			{
				EXPECT_EQ(ETaskState::Succeeded, Handle.GetState());
			}
			else
			{
				EXPECT_EQ(ETaskState::Invalid, Handle.GetState());
			}
		}
		EXPECT_FALSE(LaunchTask("RejectedAfterClose", []() {}).IsValid());
	}

	TEST(FTaskTests, LaunchTaskReturnsValidHandleAndCompletes)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		std::atomic<bool> bTaskRan = false;
		FTaskHandle Handle = LaunchTask("HandleCompletion", [&]() {
			bTaskRan.store(true, std::memory_order::release);
		});

		ASSERT_TRUE(Handle.IsValid());
		EXPECT_STREQ("HandleCompletion", Handle.GetDebugName());

		WaitTask(Handle);

		EXPECT_TRUE(Handle.IsComplete());
		EXPECT_TRUE(bTaskRan.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, WaitAllWaitsForManyTasks)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));

		constexpr uint32 TaskCount = 32;
		std::atomic<uint32> CompletedTaskCount = 0;
		std::vector<FTaskHandle> Handles;
		Handles.reserve(TaskCount);

		for (uint32 TaskIndex = 0; TaskIndex < TaskCount; ++TaskIndex)
		{
			Handles.push_back(LaunchTask("WaitAllTask", [&]() {
				CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}));
			ASSERT_TRUE(Handles.back().IsValid());
		}

		const std::vector<ETaskState> Outcomes = WaitAll(std::span<const FTaskHandle>(Handles.data(), Handles.size()));

		EXPECT_EQ(TaskCount, CompletedTaskCount.load(std::memory_order::acquire));
		ASSERT_EQ(TaskCount, Outcomes.size());
		for (ETaskState Outcome : Outcomes)
		{
			EXPECT_EQ(ETaskState::Succeeded, Outcome);
		}
		for (const FTaskHandle& Handle : Handles)
		{
			EXPECT_TRUE(Handle.IsComplete());
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

		FTaskHandle TargetHandle = LaunchTask("TargetTask", [&]() {
			bTargetTaskRan.store(true, std::memory_order::release);
		});
		ASSERT_TRUE(TargetHandle.IsValid());

		FTaskHandle BlockedHandle = LaunchTask("BlockedTask", [&]() {
			BlockedTaskStarted.Trigger();
			ReleaseBlockedTask.Wait();
		});
		ASSERT_TRUE(BlockedHandle.IsValid());

		WaitTask(TargetHandle);

		EXPECT_TRUE(TargetHandle.IsComplete());
		EXPECT_TRUE(bTargetTaskRan.load(std::memory_order::acquire));
		ASSERT_TRUE(BlockedTaskStarted.WaitFor(1.0));
		EXPECT_FALSE(BlockedHandle.IsComplete());

		ReleaseBlockedTask.Trigger();
		WaitTask(BlockedHandle);
	}

	TEST(FTaskTests, WorkerWaitHelpsNestedTaskOnSingleWorkerPool)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::atomic<bool> bChildTaskRan = false;
		std::atomic<bool> bChildHandleWasValid = false;
		std::atomic<bool> bParentTaskFinished = false;

		FTaskHandle ParentHandle = LaunchTask("ParentTask", [&]() {
			FTaskHandle ChildHandle = LaunchTask("ChildTask", [&]() {
				bChildTaskRan.store(true, std::memory_order::release);
			});

			bChildHandleWasValid.store(ChildHandle.IsValid(), std::memory_order::release);
			WaitTask(ChildHandle);
			bParentTaskFinished.store(true, std::memory_order::release);
		});
		ASSERT_TRUE(ParentHandle.IsValid());

		WaitTask(ParentHandle);

		EXPECT_TRUE(ParentHandle.IsComplete());
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
		FTaskHandle Blocker = LaunchTask("IneligibleWaitBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		FTaskHandle Parent;
		FTaskHandle DependentChild;
		std::atomic<ETaskState> ObservedWaitState = ETaskState::Invalid;
		Parent = LaunchTask("IneligibleWaitParent", [&]() {
			std::array<FTaskHandle, 1> Prerequisites{Parent};
			FTaskLaunchOptions Options;
			Options.Prerequisites = Prerequisites;
			DependentChild = LaunchTask("IneligibleWaitChild", []() {}, Options);
			ObservedWaitState.store(WaitTask(DependentChild), std::memory_order::release);
		});
		ReleaseBlocker.Trigger();

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Parent));
		ASSERT_TRUE(DependentChild.IsValid());
		EXPECT_EQ(ETaskState::Waiting, ObservedWaitState.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(DependentChild));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker));
	}

	TEST(FTaskTests, StandardAndUnknownExceptionsPublishFailureDiagnostics)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FTaskHandle StandardFailure = LaunchTask("StandardFailure", []() {
			throw std::runtime_error("expected task failure");
		});
		FTaskHandle UnknownFailure = LaunchTask("UnknownFailure", []() {
			throw 7;
		});

		EXPECT_EQ(ETaskState::Failed, WaitTask(StandardFailure));
		EXPECT_EQ("expected task failure", StandardFailure.GetDiagnostic());
		EXPECT_EQ(ETaskState::Failed, WaitTask(UnknownFailure));
		EXPECT_EQ("Task callable threw an unknown exception.", UnknownFailure.GetDiagnostic());
	}

	TEST(FTaskTests, DiscardShutdownCancelsQueuedTaskAndCompletesRunningTask)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		FTaskHandle RunningTask = LaunchTask("RunningDuringDiscard", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle QueuedTask = LaunchTask("QueuedDuringDiscard", []() {});
		ASSERT_TRUE(QueuedTask.IsValid());

		std::thread ShutdownThread([]() {
			ShutdownTaskScheduler(false);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}

		EXPECT_EQ(ETaskState::Canceled, WaitTask(QueuedTask));
		EXPECT_FALSE(QueuedTask.GetDiagnostic().empty());
		ReleaseBlockingTask.Trigger();
		ShutdownThread.join();

		EXPECT_EQ(ETaskState::Canceled, RunningTask.GetState());
	}

	TEST(FTaskTests, DrainShutdownCompletesQueuedAndRunningTasks)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		FTaskHandle RunningTask = LaunchTask("RunningDuringDrain", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle QueuedTask = LaunchTask("QueuedDuringDrain", []() {});

		std::thread ShutdownThread([]() {
			ShutdownTaskScheduler(true);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}
		EXPECT_FALSE(QueuedTask.IsComplete());

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
		FTaskHandle BlockingTask = LaunchTask("DiscardRaceBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		std::atomic<uint32> ExecutionCount = 0;
		FTaskHandle RacingTask = LaunchTask("DiscardRaceTarget", [&]() {
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
		EXPECT_TRUE(RacingState == ETaskState::Succeeded || RacingState == ETaskState::Canceled);
		EXPECT_LE(ExecutionCount.load(std::memory_order::acquire), 1u);
		const ETaskState BlockingState = BlockingTask.GetState();
		EXPECT_TRUE(BlockingState == ETaskState::Succeeded || BlockingState == ETaskState::Canceled);
	}

	TEST(FTaskTests, ImmutableDependenciesReleaseChainsFanInAndFanOutOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FThreadEvent ReleaseRoot;
		std::atomic<uint32> ExecutionMask = 0;
		std::atomic<bool> bOrderValid = true;
		FTaskHandle Root = LaunchTask("DependencyRoot", [&]() {
			ReleaseRoot.Wait();
			ExecutionMask.fetch_or(1u, std::memory_order::acq_rel);
		});

		std::array<FTaskHandle, 1> RootPrerequisite{Root};
		FTaskLaunchOptions RootOptions;
		RootOptions.Prerequisites = RootPrerequisite;
		FTaskHandle Left = LaunchTask("DependencyLeft", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 1u) == 0)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(2u, std::memory_order::acq_rel); }, RootOptions);
		FTaskHandle Right = LaunchTask("DependencyRight", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 1u) == 0)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(4u, std::memory_order::acq_rel); }, RootOptions);

		std::array<FTaskHandle, 2> JoinPrerequisites{Left, Right};
		FTaskLaunchOptions JoinOptions;
		JoinOptions.Prerequisites = JoinPrerequisites;
		FTaskHandle Join = LaunchTask("DependencyJoin", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 7u) != 7u)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(8u, std::memory_order::acq_rel); }, JoinOptions);

		std::array<FTaskHandle, 1> JoinPrerequisite{Join};
		FTaskLaunchOptions TailOptions;
		TailOptions.Prerequisites = JoinPrerequisite;
		FTaskHandle Tail = LaunchTask("DependencyTail", [&]() {
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

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Tail));
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
			FTaskHandle Prerequisite = LaunchTask("RegistrationRacePrerequisite", [&]() {
				RaceBarrier.arrive_and_wait();
			});
			FTaskHandle Dependent;
			std::thread Submitter([&]() {
				RaceBarrier.arrive_and_wait();
				std::array<FTaskHandle, 1> Prerequisites{Prerequisite};
				FTaskLaunchOptions Options;
				Options.Prerequisites = Prerequisites;
				Dependent = LaunchTask("RegistrationRaceDependent", [&]() { DependentExecutionCount.fetch_add(1, std::memory_order::acq_rel); }, Options);
			});
			RaceBarrier.arrive_and_wait();
			Submitter.join();

			ASSERT_TRUE(Dependent.IsValid());
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Dependent));
			EXPECT_EQ(1u, DependentExecutionCount.load(std::memory_order::acquire));
		}
	}

	TEST(FTaskTests, SimultaneousPrerequisiteCompletionReleasesFanInOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		std::barrier CompletionBarrier(3);
		FTaskHandle First = LaunchTask("SimultaneousFirst", [&]() {
			CompletionBarrier.arrive_and_wait();
		});
		FTaskHandle Second = LaunchTask("SimultaneousSecond", [&]() {
			CompletionBarrier.arrive_and_wait();
		});
		std::array<FTaskHandle, 2> Prerequisites{First, Second};
		FTaskLaunchOptions Options;
		Options.Prerequisites = Prerequisites;
		std::atomic<uint32> ExecutionCount = 0;
		FTaskHandle Dependent = LaunchTask("SimultaneousFanIn", [&]() { ExecutionCount.fetch_add(1, std::memory_order::acq_rel); }, Options);
		ASSERT_TRUE(Dependent.IsValid());
		EXPECT_EQ(ETaskState::Waiting, Dependent.GetState());

		CompletionBarrier.arrive_and_wait();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Dependent));
		EXPECT_EQ(1u, ExecutionCount.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, InvalidAndForeignLifetimePrerequisitesAreRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FTaskHandle InvalidPrerequisite;
		std::array<FTaskHandle, 1> InvalidPrerequisites{InvalidPrerequisite};
		FTaskLaunchOptions InvalidOptions;
		InvalidOptions.Prerequisites = InvalidPrerequisites;
		EXPECT_FALSE(LaunchTask("InvalidPrerequisiteTask", []() {}, InvalidOptions).IsValid());

		FTaskHandle EarlierLifetimeTask = LaunchTask("EarlierLifetimeTask", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(EarlierLifetimeTask));
		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::array<FTaskHandle, 1> ForeignPrerequisites{EarlierLifetimeTask};
		FTaskLaunchOptions ForeignOptions;
		ForeignOptions.Prerequisites = ForeignPrerequisites;
		EXPECT_FALSE(LaunchTask("ForeignPrerequisiteTask", []() {}, ForeignOptions).IsValid());
	}

	TEST(FTaskTests, FailureAndCancellationPropagateWithoutRunningDependents)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::atomic<bool> bFailedDependentRan = false;
		FTaskHandle FailedPrerequisite = LaunchTask("FailedPrerequisite", []() {
			throw std::runtime_error("dependency failure");
		});
		std::array<FTaskHandle, 1> FailedPrerequisites{FailedPrerequisite};
		FTaskLaunchOptions FailedOptions;
		FailedOptions.Prerequisites = FailedPrerequisites;
		FTaskHandle FailedDependent = LaunchTask("FailedDependent", [&]() { bFailedDependentRan.store(true, std::memory_order::release); }, FailedOptions);

		EXPECT_EQ(ETaskState::Failed, WaitTask(FailedPrerequisite));
		EXPECT_EQ(ETaskState::Canceled, WaitTask(FailedDependent));
		EXPECT_FALSE(bFailedDependentRan.load(std::memory_order::acquire));
		EXPECT_NE(std::string::npos, FailedDependent.GetDiagnostic().find(std::to_string(FailedPrerequisite.GetTaskId())));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = LaunchTask("CancellationPropagationBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		FTaskHandle CanceledPrerequisite = LaunchTask("CanceledPrerequisite", []() {});
		std::array<FTaskHandle, 1> CanceledPrerequisites{CanceledPrerequisite};
		FTaskLaunchOptions CanceledOptions;
		CanceledOptions.Prerequisites = CanceledPrerequisites;
		std::atomic<bool> bCanceledDependentRan = false;
		FTaskHandle CanceledDependent = LaunchTask("CanceledDependent", [&]() { bCanceledDependentRan.store(true, std::memory_order::release); }, CanceledOptions);

		EXPECT_TRUE(CancelTask(CanceledPrerequisite));
		EXPECT_EQ(ETaskState::Canceled, WaitTask(CanceledDependent));
		EXPECT_FALSE(bCanceledDependentRan.load(std::memory_order::acquire));
		EXPECT_NE(std::string::npos, CanceledDependent.GetDiagnostic().find(std::to_string(CanceledPrerequisite.GetTaskId())));
		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker));
	}

	TEST(FTaskTests, SharedCancellationSourceCancelsWaitingQueuedAndRunningTasks)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FTaskCancellationSource Source;
		FTaskLaunchOptions SharedOptions;
		SharedOptions.CancellationToken = Source.GetToken();
		FThreadEvent RunningStarted;
		FThreadEvent ReleaseRunning;
		std::atomic<bool> bRunningObservedCancellation = false;
		FTaskHandle Running = LaunchCancelableTask("SharedCancellationRunning", [&](const FTaskCancellationToken& Token) {
			RunningStarted.Trigger();
			ReleaseRunning.Wait();
			bRunningObservedCancellation.store(Token.IsCancellationRequested(), std::memory_order::release); }, SharedOptions);
		ASSERT_TRUE(RunningStarted.WaitFor(1.0));

		std::atomic<bool> bQueuedRan = false;
		FTaskHandle Queued = LaunchTask("SharedCancellationQueued", [&]() { bQueuedRan.store(true, std::memory_order::release); }, SharedOptions);
		std::array<FTaskHandle, 1> QueuedPrerequisite{Queued};
		FTaskLaunchOptions WaitingOptions = SharedOptions;
		WaitingOptions.Prerequisites = QueuedPrerequisite;
		std::atomic<bool> bWaitingRan = false;
		FTaskHandle Waiting = LaunchTask("SharedCancellationWaiting", [&]() { bWaitingRan.store(true, std::memory_order::release); }, WaitingOptions);

		Source.RequestCancellation();
		Source.RequestCancellation();
		EXPECT_TRUE(Source.IsCancellationRequested());
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Queued));
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Waiting));
		ReleaseRunning.Trigger();
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Running));
		EXPECT_TRUE(bRunningObservedCancellation.load(std::memory_order::acquire));
		EXPECT_FALSE(bQueuedRan.load(std::memory_order::acquire));
		EXPECT_FALSE(bWaitingRan.load(std::memory_order::acquire));

		FTaskCancellationSource PreCanceledSource;
		PreCanceledSource.RequestCancellation();
		FTaskLaunchOptions PreCanceledOptions;
		PreCanceledOptions.CancellationToken = PreCanceledSource.GetToken();
		std::atomic<bool> bPreCanceledTaskRan = false;
		FTaskHandle PreCanceledTask = LaunchTask("PreCanceledSourceTask", [&]() { bPreCanceledTaskRan.store(true, std::memory_order::release); }, PreCanceledOptions);
		ASSERT_TRUE(PreCanceledTask.IsValid());
		EXPECT_EQ(ETaskState::Canceled, WaitTask(PreCanceledTask));
		EXPECT_FALSE(bPreCanceledTaskRan.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, ExceptionWinsOverRunningCancellationAndLateCancellationIsIgnored)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent Started;
		FThreadEvent Release;
		std::atomic<bool> bTokenObservedCancellation = false;
		FTaskHandle Task = LaunchCancelableTask("ExceptionAfterCancellation", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			Release.Wait();
			bTokenObservedCancellation.store(Token.IsCancellationRequested(), std::memory_order::release);
			throw std::runtime_error("failure after cancellation");
		});
		ASSERT_TRUE(Started.WaitFor(1.0));
		EXPECT_TRUE(CancelTask(Task));
		Release.Trigger();
		EXPECT_EQ(ETaskState::Failed, WaitTask(Task));
		EXPECT_TRUE(bTokenObservedCancellation.load(std::memory_order::acquire));
		EXPECT_EQ("failure after cancellation", Task.GetDiagnostic());
		EXPECT_FALSE(CancelTask(Task));
		EXPECT_EQ(ETaskState::Failed, Task.GetState());

		FTaskHandle CompletedTask = LaunchTask("CompletedBeforeCancellation", []() {});
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(CompletedTask));
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
		FTaskHandle Task = LaunchTask("MultipleWaiters", [&]() {
			Started.Trigger();
			Release.Wait();
		});
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::array<ETaskState, 4> Outcomes;
		std::vector<std::thread> Waiters;
		for (ETaskState& Outcome : Outcomes)
		{
			Waiters.emplace_back([&Task, &Outcome]() {
				Outcome = WaitTask(Task);
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

	TEST(FTaskTests, DrainAndDiscardTerminalizeWaitingDependencyGraphs)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent DrainRootStarted;
		FThreadEvent ReleaseDrainRoot;
		FTaskHandle DrainRoot = LaunchTask("DrainGraphRoot", [&]() {
			DrainRootStarted.Trigger();
			ReleaseDrainRoot.Wait();
		});
		ASSERT_TRUE(DrainRootStarted.WaitFor(1.0));
		std::array<FTaskHandle, 1> DrainPrerequisite{DrainRoot};
		FTaskLaunchOptions DrainOptions;
		DrainOptions.Prerequisites = DrainPrerequisite;
		FTaskHandle DrainDependent = LaunchTask("DrainGraphDependent", []() {}, DrainOptions);
		std::thread DrainThread([]() {
			ShutdownTaskScheduler(true);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(ETaskState::Waiting, DrainDependent.GetState());
		ReleaseDrainRoot.Trigger();
		DrainThread.join();
		EXPECT_EQ(ETaskState::Succeeded, DrainRoot.GetState());
		EXPECT_EQ(ETaskState::Succeeded, DrainDependent.GetState());

		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent DiscardRootStarted;
		FThreadEvent ReleaseDiscardRoot;
		FTaskHandle DiscardRoot = LaunchTask("DiscardGraphRoot", [&]() {
			DiscardRootStarted.Trigger();
			ReleaseDiscardRoot.Wait();
		});
		ASSERT_TRUE(DiscardRootStarted.WaitFor(1.0));
		std::array<FTaskHandle, 1> DiscardPrerequisite{DiscardRoot};
		FTaskLaunchOptions DiscardOptions;
		DiscardOptions.Prerequisites = DiscardPrerequisite;
		FTaskHandle DiscardDependent = LaunchTask("DiscardGraphDependent", []() {}, DiscardOptions);
		std::thread DiscardThread([]() {
			ShutdownTaskScheduler(false);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(ETaskState::Canceled, WaitTask(DiscardDependent));
		ReleaseDiscardRoot.Trigger();
		DiscardThread.join();
		EXPECT_EQ(ETaskState::Canceled, DiscardRoot.GetState());
	}

	TEST(FTaskTests, SelfWaitIsRejectedWithoutBlockingWorker)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		FTaskHandle BlockingTask = LaunchTask("SelfWaitBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));

		FTaskHandle SelfTask;
		std::atomic<ETaskState> ObservedWaitState = ETaskState::Invalid;
		SelfTask = LaunchTask("SelfWait", [&]() {
			ObservedWaitState.store(WaitTask(SelfTask), std::memory_order::release);
		});
		ReleaseBlockingTask.Trigger();

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(SelfTask));
		EXPECT_EQ(ETaskState::Running, ObservedWaitState.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(BlockingTask));
	}

	TEST(FTaskTests, RenderingThreadWaitIsRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		FTaskHandle BlockingTask = LaunchTask("RenderingWaitBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle QueuedTask = LaunchTask("RenderingWaitTarget", []() {});

		FThreadEvent WaitReturned;
		FWaitTaskRunnable Runnable(QueuedTask, WaitReturned);
		std::unique_ptr<FRunnableThread> RenderingThread(FRunnableThread::Create(&Runnable, "TaskWaitRenderingThread", 0, EThreadPriority::Normal, EThreadRole::RenderingThread));
		ASSERT_NE(RenderingThread, nullptr);
		ASSERT_TRUE(WaitReturned.WaitFor(1.0));
		RenderingThread->WaitForCompletion();

		EXPECT_EQ(ETaskState::Queued, Runnable.ObservedState);
		ReleaseBlockingTask.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(QueuedTask));
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(BlockingTask));
	}

	TEST(FTaskTests, RestartIsRejectedUntilShutdownCompletesAndOldHandleRemainsQueryable)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		FTaskHandle BlockingTask = LaunchTask("ShutdownBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle RetainedTask = LaunchTask("RetainedAcrossRestart", []() {});
		ASSERT_TRUE(RetainedTask.IsValid());

		std::thread ShutdownThread([]() {
			ShutdownTaskScheduler(true);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}

		EXPECT_FALSE(InitializeTaskScheduler(1));
		EXPECT_FALSE(LaunchTask("RejectedDuringShutdown", []() {}).IsValid());

		ReleaseBlockingTask.Trigger();
		ShutdownThread.join();
		EXPECT_EQ(ETaskState::Succeeded, BlockingTask.GetState());
		EXPECT_EQ(ETaskState::Succeeded, RetainedTask.GetState());

		ASSERT_TRUE(InitializeTaskScheduler(1));
		FTaskHandle NewTask = LaunchTask("SequentialFixtureRestart", []() {});
		ASSERT_TRUE(NewTask.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(NewTask));
		EXPECT_EQ(ETaskState::Succeeded, RetainedTask.GetState());
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

	TEST(DISABLED_FParallelForBenchmarks, MeasuresSerialParallelCrossover)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));

		constexpr uint32 WarmupCount = 2;
		constexpr uint32 SampleCount = 9;
		constexpr uint32 WorkRounds = 64;
		constexpr std::array<uint64, 8> RangeSizes{64, 256, 1024, 4096, 16384, 65536, 262144, 1048576};
		FParallelForOptions Options;
		Options.MinBatchSize = 256;
		uint64 Checksum = 0;

		std::cout << "ParallelForMeasurement,workers=4,min_batch=256,warmups=" << WarmupCount
			<< ",samples=" << SampleCount << ",work_rounds=" << WorkRounds << '\n';
		std::cout << "range,serial_median_ns,parallel_median_ns,parallel_chunks\n";
		for (uint64 Num : RangeSizes)
		{
			std::vector<uint64> Output(Num);
			auto Work = [&](uint64 Index) {
				uint64 Value = Index + 0x9e3779b97f4a7c15ull;
				for (uint32 Round = 0; Round < WorkRounds; ++Round)
				{
					Value ^= Value >> 12;
					Value ^= Value << 25;
					Value ^= Value >> 27;
					Value *= 0x2545f4914f6cdd1dull;
				}
				Output[Index] = Value;
			};

			for (uint32 WarmupIndex = 0; WarmupIndex < WarmupCount; ++WarmupIndex)
			{
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					Work(Index);
				}
				ASSERT_EQ(ETaskState::Succeeded, ParallelFor("CrossoverWarmup", Num, Work, Options).State);
			}

			std::vector<uint64> SerialSamples;
			std::vector<uint64> ParallelSamples;
			SerialSamples.reserve(SampleCount);
			ParallelSamples.reserve(SampleCount);
			uint32 ParallelChunkCount = 0;
			for (uint32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
			{
				const auto SerialStart = std::chrono::steady_clock::now();
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					Work(Index);
				}
				SerialSamples.emplace_back(static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SerialStart
				).count()));

				const auto ParallelStart = std::chrono::steady_clock::now();
				const FParallelForResult Result = ParallelFor("CrossoverSample", Num, Work, Options);
				ParallelSamples.emplace_back(static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - ParallelStart
				).count()));
				ASSERT_EQ(ETaskState::Succeeded, Result.State);
				ParallelChunkCount = Result.ChunkCount;
			}

			std::sort(SerialSamples.begin(), SerialSamples.end());
			std::sort(ParallelSamples.begin(), ParallelSamples.end());
			const uint64 SerialMedian = SerialSamples[SampleCount / 2];
			const uint64 ParallelMedian = ParallelSamples[SampleCount / 2];
			std::cout << Num << ',' << SerialMedian << ',' << ParallelMedian << ',' << ParallelChunkCount << '\n';
			Checksum ^= Output[Num / 2];
		}
		EXPECT_NE(0u, Checksum);
	}

	TEST(FTaskDiagnosticsTests, ReportsRelationshipsTimingCountersLongWaitsAndRetainedHandles)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = LaunchTask("DiagnosticBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		FTaskHandle Queued = LaunchTask("DiagnosticQueued", []() {});

		FTaskHandle InvalidPrerequisite;
		std::array<FTaskHandle, 1> InvalidPrerequisites{InvalidPrerequisite};
		FTaskLaunchOptions InvalidOptions;
		InvalidOptions.Prerequisites = InvalidPrerequisites;
		EXPECT_FALSE(LaunchTask("DiagnosticRejected", []() {}, InvalidOptions).IsValid());

		FTaskSchedulerDiagnostics ActiveDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_TRUE(ActiveDiagnostics.bRunning);
		EXPECT_EQ(1u, ActiveDiagnostics.WorkerCount);
		EXPECT_EQ(1u, ActiveDiagnostics.ActiveWorkerCount);
		EXPECT_GE(ActiveDiagnostics.QueueDepth, 1u);
		EXPECT_EQ(2u, ActiveDiagnostics.NonterminalTaskCount);
		EXPECT_EQ(1u, ActiveDiagnostics.RejectedTaskCount);
		ASSERT_EQ(2u, ActiveDiagnostics.NonterminalTasks.size());

		FThreadEvent WaiterStarted;
		std::thread Waiter([&]() {
			WaiterStarted.Trigger();
			WaitTask(Blocker);
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
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker));
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Queued));

		FThreadEvent AllowParentWork;
		FTaskHandle Child;
		FTaskHandle Parent = LaunchTask("DiagnosticParent", [&]() {
			AllowParentWork.Wait();
			Child = LaunchTask("DiagnosticChild", []() {});
			WaitTask(Child);
		});
		AllowParentWork.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Parent));
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

		FTaskHandle Prerequisite = LaunchTask("DiagnosticPrerequisite", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Prerequisite));
		std::array<FTaskHandle, 1> Prerequisites{Prerequisite};
		FTaskLaunchOptions DependentOptions;
		DependentOptions.Prerequisites = Prerequisites;
		FTaskHandle Dependent = LaunchTask("DiagnosticDependent", []() {}, DependentOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Dependent));
		ASSERT_EQ(1u, Dependent.GetDiagnostics().PrerequisiteTaskIds.size());
		EXPECT_EQ(Prerequisite.GetTaskId(), Dependent.GetDiagnostics().PrerequisiteTaskIds[0]);

		FTaskHandle Failed = LaunchTask("DiagnosticFailure", []() {
			throw std::runtime_error("diagnostic failure");
		});
		EXPECT_EQ(ETaskState::Failed, WaitTask(Failed));
		EXPECT_EQ("diagnostic failure", Failed.GetDiagnostics().Diagnostic);

		const FTaskSchedulerDiagnostics TerminalDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_GE(TerminalDiagnostics.CompletedTaskCount, 7u);
		EXPECT_EQ(1u, TerminalDiagnostics.FailedTaskCount);
		EXPECT_EQ(1u, TerminalDiagnostics.CanceledTaskCount);
		EXPECT_EQ(0u, TerminalDiagnostics.NonterminalTaskCount);

		ShutdownTaskScheduler(true);
		const FTaskSchedulerDiagnostics ShutdownDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_FALSE(ShutdownDiagnostics.bRunning);
		EXPECT_EQ(0u, ShutdownDiagnostics.NonterminalTaskCount);
		EXPECT_EQ(0u, ShutdownDiagnostics.ActiveWorkerCount);
		EXPECT_GE(ShutdownDiagnostics.RetainedTerminalHandleCount, 7u);
	}

	TEST(FTaskTests, InvalidHandlesAreNoOp)
	{
		FTaskHandle InvalidHandle;

		EXPECT_FALSE(InvalidHandle.IsValid());
		EXPECT_FALSE(InvalidHandle.IsComplete());
		EXPECT_EQ(ETaskState::Invalid, InvalidHandle.GetState());
		EXPECT_STREQ("", InvalidHandle.GetDebugName());

		EXPECT_EQ(ETaskState::Invalid, WaitTask(InvalidHandle));
		WaitAll(std::span<const FTaskHandle>(&InvalidHandle, 1));
	}

	TEST(FTaskTests, LaunchTaskReturnsInvalidHandleWhenSchedulerIsStopped)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;

		FTaskHandle Handle = LaunchTask("RejectedTask", []() {});

		EXPECT_FALSE(Handle.IsValid());
		EXPECT_FALSE(Handle.IsComplete());
	}
} // namespace Durin
