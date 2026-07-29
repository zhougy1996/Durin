#include <gtest/gtest.h>

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

		class FEngineThreadPoolTestGuard
		{
		public:
			~FEngineThreadPoolTestGuard()
			{
				ShutdownEngineThreadPool(false);
			}
		};
	}

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

	TEST(FQueuedThreadPoolTests, WaitForIdleReturnsForIdleRunningPool)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(2, "QueuedPoolIdle"));

		Pool.WaitForIdle();
		EXPECT_TRUE(Pool.IsRunning());

		Pool.Destroy(true);
	}

	TEST(FEngineThreadPoolTests, InitializesGlobalPoolAndRunsWork)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;

		EXPECT_GE(GetDefaultThreadPoolThreadCount(), 1u);
		ASSERT_TRUE(InitEngineThreadPool(2));
		ASSERT_NE(GThreadPool, nullptr);
		EXPECT_TRUE(GThreadPool->IsRunning());
		EXPECT_EQ(2u, GThreadPool->GetNumThreads());

		FThreadEvent TaskFinished;
		std::atomic<bool> bObservedWorkerThread = false;
		ASSERT_TRUE(GThreadPool->Enqueue("EnginePoolTask", [&]() {
			bObservedWorkerThread.store(IsInWorkerThread(), std::memory_order::release);
			TaskFinished.Trigger();
		}));

		ASSERT_TRUE(TaskFinished.WaitFor(1.0));
		GThreadPool->WaitForIdle();

		ShutdownEngineThreadPool(true);

		EXPECT_EQ(GThreadPool, nullptr);
		EXPECT_TRUE(bObservedWorkerThread.load(std::memory_order::acquire));
	}

	TEST(FTaskTests, LaunchTaskReturnsValidHandleAndCompletes)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(2));

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
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(4));

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

		WaitAll(std::span<const FTaskHandle>(Handles.data(), Handles.size()));

		EXPECT_EQ(TaskCount, CompletedTaskCount.load(std::memory_order::acquire));
		for (const FTaskHandle& Handle : Handles)
		{
			EXPECT_TRUE(Handle.IsComplete());
		}
	}

	TEST(FTaskTests, WaitingOneHandleDoesNotRequirePoolIdle)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(1));

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
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitEngineThreadPool(1));

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

	TEST(FTaskTests, InvalidHandlesAreNoOp)
	{
		FTaskHandle InvalidHandle;

		EXPECT_FALSE(InvalidHandle.IsValid());
		EXPECT_FALSE(InvalidHandle.IsComplete());
		EXPECT_STREQ("", InvalidHandle.GetDebugName());

		WaitTask(InvalidHandle);
		WaitAll(std::span<const FTaskHandle>(&InvalidHandle, 1));
	}

	TEST(FTaskTests, LaunchTaskReturnsInvalidHandleWhenGlobalPoolIsStopped)
	{
		ShutdownEngineThreadPool(false);
		FEngineThreadPoolTestGuard Guard;

		FTaskHandle Handle = LaunchTask("RejectedTask", []() {});

		EXPECT_FALSE(Handle.IsValid());
		EXPECT_FALSE(Handle.IsComplete());
	}
}
