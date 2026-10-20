#include "TaskKernelTestSupport.h"
#include <gtest/gtest.h>

#include <barrier>

#include "Profiling/Profiling.h"
#include "Threading/QueuedThreadPool.h"
#include "Threading/Private/QueuedWorkNode.h"
#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	static_assert(sizeof(FTaskAttribution) == sizeof(uint16) * 2);
	static_assert(std::is_trivially_copyable_v<FTaskAttribution>);
	static_assert(std::is_standard_layout_v<FTaskAttribution>);
	static_assert(std::equality_comparable<FTaskAttribution>);
	static_assert(noexcept(Profiling::TaskEnqueued(1, 2, 2, 0)));
	static_assert(noexcept(Profiling::TaskExecution(1, 2, 2, 0)));
	static_assert(noexcept(Profiling::TaskTerminal(1, 2, 2, 0, 0)));
	static_assert(noexcept(Profiling::TaskAggregatePlots(2, 2, 0, 0, 0, 0)));

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

	TEST(FQueuedThreadPoolTests, ExecutionNodeHasOneShotMembershipAcrossPools)
	{
		EnsureGameThreadForTaskTest();
		struct FNode final : Private::FQueuedWorkNode
		{
			auto Execute() -> void override { ++Runs; }
			auto Discard() -> void override { ++Discards; }
			auto GetName() const -> const char* override { return "DirectExecutionNode"; }
			std::atomic<uint32> Runs = 0, Discards = 0;
		};
		FQueuedThreadPool Pool, Other;
		ASSERT_TRUE(Pool.Create(1)); ASSERT_TRUE(Other.Create(1));
		FThreadEvent Started, Release;
		ASSERT_TRUE(Pool.Enqueue("NodeMembershipBlocker", [&] { Started.Trigger(); Release.WaitFor(2); }));
		ASSERT_TRUE(Started.WaitFor(1));
		auto Node = std::make_shared<FNode>();
		ASSERT_TRUE(Pool.EnqueueNode(Node, EQueuedWorkPriority::Normal));
		EXPECT_FALSE(Pool.EnqueueNode(Node, EQueuedWorkPriority::High));
		EXPECT_FALSE(Other.EnqueueNode(Node, EQueuedWorkPriority::Normal));
		EXPECT_FALSE(Other.TryExecuteNode(Node));
		EXPECT_TRUE(Pool.TryExecuteNode(Node));
		EXPECT_FALSE(Pool.TryExecuteNode(Node));
		EXPECT_FALSE(Pool.EnqueueNode(Node, EQueuedWorkPriority::Normal));
		Release.Trigger();
		EXPECT_TRUE(Pool.WaitForIdle());
		EXPECT_EQ(Node->Runs.load(), 1u);
		EXPECT_EQ(Node->Discards.load(), 0u);
	}

	TEST(FQueuedThreadPoolTests, AllocationRollbackReleasesOwnerAndCallableOutsideLock)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1));
		bool Destroyed = false;
		auto Capture = std::shared_ptr<int>(new int(7), [&](int* Value) {
			Destroyed = true;
			delete Value;
		});
		Private::SetQueuedWorkAllocationFailureForTests(true);
		FQueuedWorkHandle Handle;
		EXPECT_THROW(Pool.Enqueue("FailedQueueAllocation", [Capture = std::move(Capture)] {}, {},
			EQueuedWorkPriority::Normal, &Handle), std::bad_alloc);
		Private::SetQueuedWorkAllocationFailureForTests(false);
		EXPECT_TRUE(Destroyed);
		EXPECT_FALSE(Pool.TryExecuteQueuedTask(Handle));
		EXPECT_EQ(0u, Pool.GetNumQueuedTasks());
		EXPECT_TRUE(Pool.Enqueue("RetryQueueAllocation", [] {}, {}));
	}

	TEST(FQueuedThreadPoolTests, DirectRetractionPreservesQueueOrderAndReleasesCapturedOwners)
	{
		EnsureGameThreadForTaskTest();
		FQueuedThreadPool Pool, OtherPool;
		ASSERT_TRUE(Pool.Create(1)); ASSERT_TRUE(OtherPool.Create(1));
		FThreadEvent Started, Release;
		ASSERT_TRUE(Pool.Enqueue("HandleTestBlocker", [&] { Started.Trigger(); Release.WaitFor(2); }));
		ASSERT_TRUE(Started.WaitFor(1));
		std::vector<int> Order;
		std::array<FQueuedWorkHandle, 4> Handles;
		std::atomic<uint32> Discarded = 0;
		auto Owner = std::make_shared<int>(3);
		std::weak_ptr<int> WeakOwner = Owner;
		for (int Index = 0; Index < 4; ++Index)
		{
			EXPECT_TRUE(Pool.Enqueue("DirectQueueNode", [&, Index, Capture = Owner] { Order.push_back(Index); },
				[&] { ++Discarded; }, EQueuedWorkPriority::Normal, &Handles[Index]));
		}
		Owner.reset();
		EXPECT_FALSE(OtherPool.TryExecuteQueuedTask(Handles[2]));
		EXPECT_TRUE(Pool.TryExecuteQueuedTask(Handles[1])); // middle
		EXPECT_TRUE(Pool.TryExecuteQueuedTask(Handles[3])); // tail
		EXPECT_TRUE(Pool.TryExecuteQueuedTask(Handles[0])); // head
		EXPECT_FALSE(Pool.TryExecuteQueuedTask(Handles[1]));
		EXPECT_EQ(Pool.GetNumQueuedTasks(), 1u);
		Release.Trigger();
		EXPECT_TRUE(Pool.WaitForIdle());
		EXPECT_EQ(Order, (std::vector<int>{1, 3, 0, 2}));
		EXPECT_EQ(Discarded.load(), 0u);
		EXPECT_TRUE(WeakOwner.expired());
		for (const auto& Handle : Handles) EXPECT_FALSE(Pool.TryExecuteQueuedTask(Handle));
		Pool.Destroy(false);
		ASSERT_TRUE(Pool.Create(1));
		for (const auto& Handle : Handles) EXPECT_FALSE(Pool.TryExecuteQueuedTask(Handle));
	}

	TEST(FQueuedThreadPoolTests, ShutdownInvalidatesRetractionHandlesAndDiscardsOutsideQueueLock)
	{
		EnsureGameThreadForTaskTest();
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1));
		FThreadEvent Started, Release, DiscardEntered;
		ASSERT_TRUE(Pool.Enqueue("ShutdownHandleBlocker", [&] { Started.Trigger(); Release.WaitFor(2); }));
		ASSERT_TRUE(Started.WaitFor(1));
		std::atomic<uint32> Runs = 0, Discards = 0, Destructions = 0;
		auto Owner = std::shared_ptr<int>(new int(5), [&](int* Value) {
			EXPECT_EQ(Pool.GetNumQueuedTasks(), 0u);
			++Destructions; delete Value;
		});
		std::weak_ptr<int> WeakOwner = Owner;
		FQueuedWorkHandle Handle;
		EXPECT_TRUE(Pool.Enqueue("ShutdownQueueNode", [&, Capture = std::move(Owner)] { ++Runs; },
			[&] { ++Discards; DiscardEntered.Trigger(); }, EQueuedWorkPriority::Normal, &Handle));
		std::thread Shutdown([&] { Pool.Destroy(false); });
		EXPECT_TRUE(DiscardEntered.WaitFor(1));
		EXPECT_FALSE(Pool.TryExecuteQueuedTask(Handle));
		Release.Trigger(); Shutdown.join();
		EXPECT_EQ(Runs.load(), 0u);
		EXPECT_EQ(Discards.load(), 1u);
		EXPECT_EQ(Destructions.load(), 1u);
		EXPECT_TRUE(WeakOwner.expired());
		EXPECT_FALSE(Pool.TryExecuteQueuedTask(Handle));
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

		ASSERT_TRUE(Pool.Enqueue("DiscardedTask", []() {}, [Count = std::make_unique<uint32>(1), &DiscardCount]() {
			DiscardCount.fetch_add(*Count, std::memory_order::acq_rel);
		}));

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

	TEST(FTaskSchedulerTests, ConcurrentAdmissionCloseEitherRejectsOrCompletesEverySubmission)
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
		ShutdownTaskScheduler(true);
		for (std::thread& Producer : Producers)
		{
			Producer.join();
		}

		for (const Tasks::FTask& Handle : Handles)
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
		EXPECT_FALSE(SubmitKernelTask("RejectedAfterClose", []() {}).IsValid());
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

		EXPECT_TRUE(TargetHandle.IsComplete());
		EXPECT_TRUE(bTargetTaskRan.load(std::memory_order::acquire));
		ASSERT_TRUE(BlockedTaskStarted.WaitFor(1.0));
		EXPECT_FALSE(BlockedHandle.IsComplete());

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

	TEST(FTaskTests, DiscardShutdownCancelsQueuedTaskAndCompletesRunningTask)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		Tasks::FTask RunningTask = SubmitKernelTask("RunningDuringDiscard", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		Tasks::FTask QueuedTask = SubmitKernelTask("QueuedDuringDiscard", []() {});
		ASSERT_TRUE(QueuedTask.IsValid());

		std::thread ShutdownThread([]() {
			ShutdownTaskScheduler(false);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}

		EXPECT_EQ(ETaskState::Canceled, WaitTask(QueuedTask).TaskState);
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

	TEST(FTaskTests, InvalidAndForeignLifetimePrerequisitesAreRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		Tasks::FTask InvalidPrerequisite;
		std::array<Tasks::FTask, 1> InvalidPrerequisites{InvalidPrerequisite};
		FTaskLaunchOptions InvalidOptions;
		InvalidOptions.Prerequisites = InvalidPrerequisites;
		EXPECT_FALSE(SubmitKernelTask("InvalidPrerequisiteTask", []() {}, InvalidOptions).IsValid());

		Tasks::FTask EarlierLifetimeTask = SubmitKernelTask("EarlierLifetimeTask", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(EarlierLifetimeTask).TaskState);
		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::array<Tasks::FTask, 1> ForeignPrerequisites{EarlierLifetimeTask};
		FTaskLaunchOptions ForeignOptions;
		ForeignOptions.Prerequisites = ForeignPrerequisites;
		EXPECT_FALSE(SubmitKernelTask("ForeignPrerequisiteTask", []() {}, ForeignOptions).IsValid());
	}

	TEST(FTaskTests, FailureAndCancellationPropagateWithoutRunningDependents)
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
		EXPECT_EQ(ETaskState::Canceled, WaitTask(FailedDependent).TaskState);
		EXPECT_FALSE(bFailedDependentRan.load(std::memory_order::acquire));
		EXPECT_NE(std::string::npos, FailedDependent.GetDiagnostic().find(std::to_string(FailedPrerequisite.GetTaskId())));

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
		EXPECT_EQ(ETaskState::Canceled, WaitTask(CanceledDependent).TaskState);
		EXPECT_FALSE(bCanceledDependentRan.load(std::memory_order::acquire));
		EXPECT_NE(std::string::npos, CanceledDependent.GetDiagnostic().find(std::to_string(CanceledPrerequisite.GetTaskId())));
		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
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
		Tasks::FTask Running = SubmitCancelableKernelTask("SharedCancellationRunning", [&](const FTaskCancellationToken& Token) {
			RunningStarted.Trigger();
			ReleaseRunning.Wait();
			bRunningObservedCancellation.store(Token.IsCancellationRequested(), std::memory_order::release); }, SharedOptions);
		ASSERT_TRUE(RunningStarted.WaitFor(1.0));

		std::atomic<bool> bQueuedRan = false;
		Tasks::FTask Queued = SubmitKernelTask("SharedCancellationQueued", [&]() { bQueuedRan.store(true, std::memory_order::release); }, SharedOptions);
		std::array<Tasks::FTask, 1> QueuedPrerequisite{Queued};
		FTaskLaunchOptions WaitingOptions = SharedOptions;
		WaitingOptions.Prerequisites = QueuedPrerequisite;
		std::atomic<bool> bWaitingRan = false;
		Tasks::FTask Waiting = SubmitKernelTask("SharedCancellationWaiting", [&]() { bWaitingRan.store(true, std::memory_order::release); }, WaitingOptions);

		Source.RequestCancellation();
		Source.RequestCancellation();
		EXPECT_TRUE(Source.IsCancellationRequested());
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Queued).TaskState);
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Waiting).TaskState);
		ReleaseRunning.Trigger();
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Running).TaskState);
		EXPECT_TRUE(bRunningObservedCancellation.load(std::memory_order::acquire));
		EXPECT_FALSE(bQueuedRan.load(std::memory_order::acquire));
		EXPECT_FALSE(bWaitingRan.load(std::memory_order::acquire));

		FTaskCancellationSource PreCanceledSource;
		PreCanceledSource.RequestCancellation();
		FTaskLaunchOptions PreCanceledOptions;
		PreCanceledOptions.CancellationToken = PreCanceledSource.GetToken();
		std::atomic<bool> bPreCanceledTaskRan = false;
		Tasks::FTask PreCanceledTask = SubmitKernelTask("PreCanceledSourceTask", [&]() { bPreCanceledTaskRan.store(true, std::memory_order::release); }, PreCanceledOptions);
		ASSERT_TRUE(PreCanceledTask.IsValid());
		EXPECT_EQ(ETaskState::Canceled, WaitTask(PreCanceledTask).TaskState);
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

	TEST(FTaskTests, DrainAndDiscardTerminalizeWaitingDependencyGraphs)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent DrainRootStarted;
		FThreadEvent ReleaseDrainRoot;
		Tasks::FTask DrainRoot = SubmitKernelTask("DrainGraphRoot", [&]() {
			DrainRootStarted.Trigger();
			ReleaseDrainRoot.Wait();
		});
		ASSERT_TRUE(DrainRootStarted.WaitFor(1.0));
		std::array<Tasks::FTask, 1> DrainPrerequisite{DrainRoot};
		FTaskLaunchOptions DrainOptions;
		DrainOptions.Prerequisites = DrainPrerequisite;
		Tasks::FTask DrainDependent = SubmitKernelTask("DrainGraphDependent", []() {}, DrainOptions);
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
		Tasks::FTask DiscardRoot = SubmitKernelTask("DiscardGraphRoot", [&]() {
			DiscardRootStarted.Trigger();
			ReleaseDiscardRoot.Wait();
		});
		ASSERT_TRUE(DiscardRootStarted.WaitFor(1.0));
		std::array<Tasks::FTask, 1> DiscardPrerequisite{DiscardRoot};
		FTaskLaunchOptions DiscardOptions;
		DiscardOptions.Prerequisites = DiscardPrerequisite;
		Tasks::FTask DiscardDependent = SubmitKernelTask("DiscardGraphDependent", []() {}, DiscardOptions);
		std::thread DiscardThread([]() {
			ShutdownTaskScheduler(false);
		});
		while (IsTaskSchedulerRunning())
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(ETaskState::Canceled, WaitTask(DiscardDependent).TaskState);
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
			auto Task = Tasks::LaunchIndependentTask("IndependentRenderPreparation", [Fail] {
				if (Fail) throw std::runtime_error("independent task failure");
				return 42;
			});
			FWaitTaskRunnable Runnable(Durin::Tasks::FTask(Task), Returned);
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(&Runnable,
				"IndependentRenderingJoin", 0, EThreadPriority::Normal, EThreadRole::RenderingThread));
			EXPECT_NE(Thread, nullptr);
			if (Thread) EXPECT_FALSE(Returned.WaitFor(0.02));
			if (Outcome == 2) EXPECT_TRUE(Tasks::Cancel(Task));
			Release.Trigger();
			if (Thread)
			{
				EXPECT_TRUE(Returned.WaitFor(2.0));
				Thread->WaitForCompletion();
				EXPECT_EQ(Runnable.ObservedResult.WaitStatus, ETaskWaitStatus::Completed);
				EXPECT_EQ(Runnable.ObservedResult.TaskState, Outcome == 2 ? ETaskState::Canceled : Fail ? ETaskState::Failed : ETaskState::Succeeded);
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

		Tasks::FTaskExecutionOptions SourceOptions;
		auto Source = Tasks::TCompletionSource<void>::Create(SourceOptions);
		auto Pending = Source.TakeTask();
		const Tasks::FTask PendingHandle = Durin::Tasks::FTask(Pending);
		for (int InvalidKind = 0; InvalidKind < 3; ++InvalidKind)
		{
			FTaskLaunchOptions Options;
			Options.bIndependentCPU = true;
			if (InvalidKind == 0) Options.Prerequisites = std::span<const Tasks::FTask>(&PendingHandle, 1);
			if (InvalidKind == 1) Options.bExternalCompletion = true;
			if (InvalidKind == 2) Options.Target = ETaskTarget::BlockingIO;
			auto Admission = Private::TryLaunchCancelableTaskWithCompletion("InvalidIndependent",
				[](const FTaskCancellationToken&) {}, {}, Options);
			ASSERT_FALSE(Admission.HasValue());
			EXPECT_EQ(Admission.GetError().Code, Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract);
		}
		auto Leaf = Tasks::LaunchIndependentTask("IndependentLeafContract", [&] {
			auto Child = Private::TryLaunchCancelableTaskWithCompletion("ForbiddenChild",
				[](const FTaskCancellationToken&) {}, {}, {});
			EXPECT_FALSE(Child.HasValue());
			if (!Child.HasValue()) EXPECT_EQ(Child.GetError().Code, Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract);
			EXPECT_EQ(WaitTask(PendingHandle).WaitStatus, ETaskWaitStatus::UnsupportedThread);
			return true;
		});
		const auto LeafHandle = Durin::Tasks::FTask(Leaf);
		EXPECT_TRUE(Leaf.GetResult());
		const auto Dynamic = Private::FTaskRuntimeAccess::BindDynamicDependency(LeafHandle, PendingHandle);
		ASSERT_TRUE(Dynamic);
		EXPECT_EQ(Dynamic->Code, Tasks::ETaskAdmissionErrorCode::InvalidExecutionContract);
		EXPECT_TRUE(Source.TrySetValue());
		Pending.GetResult();
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
		EXPECT_FALSE(SubmitKernelTask("RejectedDuringShutdown", []() {}).IsValid());

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

		Tasks::FTask InvalidPrerequisite;
		std::array<Tasks::FTask, 1> InvalidPrerequisites{InvalidPrerequisite};
		FTaskLaunchOptions InvalidOptions;
		InvalidOptions.Prerequisites = InvalidPrerequisites;
		EXPECT_FALSE(SubmitKernelTask("DiagnosticRejected", []() {}, InvalidOptions).IsValid());

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
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Queued).TaskState);

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
		EXPECT_EQ(1u, TerminalDiagnostics.CanceledTaskCount);
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
		FThreadEvent RawTerminalReached;
		FThreadEvent ReleasePublication;
		FThreadEvent ShutdownFinished;
		FTaskSchedulerDiagnostics Snapshot;

		Tasks::FTask Running = SubmitKernelTask("SnapshotContentionRunning", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
		});
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		Tasks::FTask Waiting = SubmitKernelContinuation(Running, "SnapshotContentionWaiting", []() {});
		ASSERT_TRUE(Waiting.IsValid());

		FTaskSchedulerSnapshotTestHookGuard SnapshotHook([&]() {
			Private::SetTaskSchedulerSnapshotTestHook({});
			SnapshotPinned.Trigger();
			ReleaseSnapshot.Wait();
		});
		FTaskTerminalPublicationTestHookGuard TerminalHook([&](uint64 TaskId) {
			if (TaskId != Running.GetTaskId()) return;
			Private::SetTaskTerminalPublicationTestHook({});
			RawTerminalReached.Trigger();
			ReleasePublication.Wait();
		});

		std::thread SnapshotThread([&]() { Snapshot = GetTaskSchedulerDiagnostics(); });
		EXPECT_TRUE(SnapshotPinned.WaitFor(1.0));
		Tasks::FTask Root = SubmitKernelTask("SnapshotContentionRoot", []() {});
		Tasks::FTask LateContinuation = SubmitKernelContinuation(Running, "SnapshotContentionLateContinuation", []() {});
		EXPECT_TRUE(Root.IsValid());
		EXPECT_TRUE(LateContinuation.IsValid());

		AllowTaskReturn.Trigger();
		EXPECT_TRUE(RawTerminalReached.WaitFor(1.0));
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


	TEST(FTaskAdmissionTests, PreparingCancellationPreservesOriginalFailureAttribution)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Failed = SubmitKernelTask("CompletedFailure", [] { throw std::runtime_error("original failure"); });
		ASSERT_EQ(ETaskState::Failed, WaitTask(Failed).TaskState);
		auto Edge = Durin::SubmitKernelContinuation(Failed, "AlreadyFailedEdge", [] {});
		ASSERT_TRUE(Edge.IsValid());
		ASSERT_EQ(ETaskState::Canceled, WaitTask(Edge).TaskState);
		EXPECT_EQ(ETaskTerminalReason::DependencyFailed, Edge.GetDiagnostics().TerminalReason);
		EXPECT_EQ(Failed.GetTaskId(), Edge.GetDiagnostics().DirectBlockingTaskId);
	}



	TEST(FTaskAdmissionTests, ContinuationValidatesConstruction)
	{
		using ECode = Tasks::ETaskAdmissionErrorCode;
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		auto Root = SubmitKernelTask("AdmissionPredecessor", [] {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		auto Continue = [&](const Tasks::FTask& Predecessor, const FTaskContinuationOptions& Options) {
			return Private::TryLaunchContinuationTask(Predecessor, "AdmissionContinuation",
				[](const FTaskCancellationToken&) {}, {}, Options, ETaskDependencyKind::Success);
		};
		FTaskContinuationOptions Options;
		Options.Target = static_cast<ETaskTarget>(255);
		auto Unsupported = Continue(Root, Options);
		ASSERT_FALSE(Unsupported.HasValue());
		EXPECT_EQ(ECode::UnsupportedExecutor, Unsupported.GetError().Code);
		Options.Target = ETaskTarget::GameThreadDeferred;
		auto InvalidPayload = Continue(Root, Options);
		ASSERT_FALSE(InvalidPayload.HasValue());
		EXPECT_EQ(ECode::InvalidPayloadDeclaration, InvalidPayload.GetError().Code);
		Options = {};
		auto Invalid = Continue({}, Options);
		ASSERT_FALSE(Invalid.HasValue());
		EXPECT_EQ(ECode::InvalidPrerequisite, Invalid.GetError().Code);
		Options = {};
		auto Accepted = Continue(Root, Options);
		ASSERT_TRUE(Accepted.HasValue());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(std::move(Accepted).TakeValue()).TaskState);
		ShutdownTaskScheduler(false);
		auto Closed = Continue(Root, Options);
		ASSERT_FALSE(Closed.HasValue());
		EXPECT_EQ(ECode::LifetimeClosed, Closed.GetError().Code);
	}

	TEST(FTaskAdmissionTests, CapacityRejectsBothEdgesAndReleasesCallableOutsideLocks)
	{
		using ECode = Tasks::ETaskAdmissionErrorCode;
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 1}));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		auto Root = SubmitKernelTask("CapacityAdmissionRoot", [] {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		const auto ReservationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().CurrentTaskReservationCount != 0
			&& std::chrono::steady_clock::now() < ReservationDeadline) std::this_thread::yield();
		ASSERT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		FTaskContinuationOptions Deferred;
		Deferred.Target = ETaskTarget::GameThreadDeferred;
		Deferred.EstimatedPayloadBytes = 32;
		auto Pending = SubmitKernelContinuation(Root, "CapacityAdmissionPending", [] {}, Deferred);
		ASSERT_TRUE(Pending.IsValid());
		ASSERT_FALSE(Pending.IsComplete());
		// Reenter scheduler diagnostics from destruction to detect lock ownership.
		std::atomic<int> Destroyed = 0;
		auto Capture = std::shared_ptr<int>(new int(1), [&](int* Value) {
			(void)GetTaskSchedulerDiagnostics();
			++Destroyed;
			delete Value;
		});
		auto RejectedRoot = Private::TryLaunchCancelableTaskWithCompletion("FullRoot",
			[Capture = std::move(Capture)](const FTaskCancellationToken&) {}, {}, {});
		ASSERT_FALSE(RejectedRoot.HasValue());
		EXPECT_EQ(ECode::CapacityExhausted, RejectedRoot.GetError().Code);
		EXPECT_EQ(1, Destroyed.load());
		auto RejectedEdge = Private::TryLaunchContinuationTask(Pending, "FullEdge",
			[](const FTaskCancellationToken&) {}, {}, {}, ETaskDependencyKind::Success);
		ASSERT_FALSE(RejectedEdge.HasValue());
		EXPECT_EQ(ECode::CapacityExhausted, RejectedEdge.GetError().Code);
		EXPECT_TRUE(CancelTask(Pending));
		ASSERT_EQ(ETaskState::Canceled, WaitTask(Pending).TaskState);
		auto Reused = Private::TryLaunchCancelableTaskWithCompletion("Reused",
			[](const FTaskCancellationToken&) {}, {}, {});
		ASSERT_TRUE(Reused.HasValue());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(std::move(Reused).TakeValue()).TaskState);
	}

	TEST(FTaskCapacityTests, ConfigurationValidationPreservesLegacyAndRunningBehavior)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		EXPECT_FALSE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 0}));
		EXPECT_FALSE(InitializeTaskScheduler({
			.NumWorkerThreads = 1,
			.MaxNonterminalTasks = static_cast<uint64>(std::numeric_limits<uint32>::max()) + 1,
		}));
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_EQ(16'384u, GetTaskSchedulerDiagnostics().TaskReservationCapacity);
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 7, .MaxNonterminalTasks = 8}));
		const FTaskSchedulerDiagnostics Unchanged = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(1u, Unchanged.WorkerCount);
		EXPECT_EQ(16'384u, Unchanged.TaskReservationCapacity);
	}


	TEST(FTaskCapacityTests, ConcurrentProducersNeverOversubscribe)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 8}));
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
		EXPECT_EQ(8u, Saturated.CurrentTaskReservationCount);
		EXPECT_EQ(8u, Saturated.PeakTaskReservationCount);
		EXPECT_EQ(7u, Accepted.size());
		EXPECT_EQ(25u, Saturated.CapacityRejectedTaskCount);

		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
		for (const Tasks::FTask& Task : Accepted) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		const auto ReservationReleaseDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().CurrentTaskReservationCount != 0
			&& std::chrono::steady_clock::now() < ReservationReleaseDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
	}

	TEST(FTaskCapacityTests, DrainAndCancelShutdownReleaseEveryReservation)
	{
		ShutdownTaskScheduler(false);
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 8}));
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
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		ReleaseDrain.Trigger();
		ShutdownTaskScheduler(true);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().PeakTaskReservationCount);

		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 8}));
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
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		ShutdownTaskScheduler(false);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().PeakTaskReservationCount);
		EXPECT_EQ(ETaskState::Canceled, Running.GetState());
		for (const Tasks::FTask& Task : Waiting) EXPECT_EQ(ETaskState::Canceled, Task.GetState());
	}



	TEST(FTaskMoveOnlyCallableTests, CompletionOwnsUniqueCaptureUntilTerminalPublication)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<int> Calls = 0, Destroyed = 0;
		auto Delete = [&Destroyed](int* Value) { delete Value; ++Destroyed; };
		auto Admission = Private::TryLaunchCancelableTaskWithCompletion("UniqueCompletion",
			[](const FTaskCancellationToken&) {},
			[Capture = std::unique_ptr<int, decltype(Delete)>(new int(7), Delete), &Calls](ETaskState State) {
				EXPECT_EQ(ETaskState::Succeeded, State);
				EXPECT_EQ(7, *Capture);
				++Calls;
			}, {});
		ASSERT_TRUE(Admission.HasValue());
		auto Task = std::move(Admission).TakeValue();
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


	TEST(FTaskMoveOnlyCallableTests, QueuedCancellationReleasesCapturesWithoutExecutorProgress)
	{
		EnsureGameThreadForTaskTest();
		for (const ETaskTarget Target : {ETaskTarget::AnyWorker, ETaskTarget::BlockingIO, ETaskTarget::GameThreadDeferred})
		{
			SCOPED_TRACE(static_cast<int>(Target));
			ShutdownTaskScheduler(false);
			FEngineThreadPoolTestGuard Guard;
			ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 1}));
			ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			FThreadEvent Started, Release;
			FTaskLaunchOptions Options;
			Options.Target = Target;
			Options.EstimatedPayloadBytes = 64;
			Tasks::FTask Blocker;
			if (Target != ETaskTarget::GameThreadDeferred)
			{
				Blocker = SubmitKernelTask("CaptureReleaseBlocker", [&] {
					Started.Trigger(); EXPECT_TRUE(Release.WaitFor(5.0));
				}, Options);
				EXPECT_TRUE(Started.WaitFor(1.0));
			}
			std::atomic<int> Destroyed = 0, Calls = 0;
			Tasks::FTask Task;
			auto Capture = std::shared_ptr<int>(new int(1), [&](int* Value) {
				// Reenter both task and scheduler queries while destruction owns no locks.
				EXPECT_EQ(ETaskState::Queued, Task.GetState());
				(void)GetTaskSchedulerDiagnostics();
				delete Value; ++Destroyed;
			});
			Task = SubmitKernelTask("CanceledQueuedCapture", [Capture = std::move(Capture), &Calls] { ++Calls; }, Options);
			EXPECT_TRUE(CancelTask(Task));
			EXPECT_EQ(ETaskState::Canceled, WaitTask(Task).TaskState);
			EXPECT_EQ(1, Destroyed.load());
			EXPECT_EQ(0, Calls.load());
			Release.Trigger();
			if (Blocker.IsValid()) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
			ShutdownTaskScheduler(false);
			EXPECT_EQ(1, Destroyed.load());
			EXPECT_EQ(0, Calls.load());
		}
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
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Task).TaskState);
		EXPECT_EQ(1, Destroyed.load());
	}

	TEST(FTaskMoveOnlyCallableTests, CancellationDestroysCaptureOutsideTaskStateLock)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		Tasks::FTask Blocker = SubmitKernelTask("MoveOnlyDestructionBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		auto CanceledHandle = std::make_shared<Tasks::FTask>();
		auto DestructionCount = std::make_shared<std::atomic<uint32>>(0);
		struct FReentrantDestruction
		{
			std::shared_ptr<Tasks::FTask> Handle;
			std::shared_ptr<std::atomic<uint32>> Count;
			std::unique_ptr<int> Ownership = std::make_unique<int>(1);

			FReentrantDestruction(std::shared_ptr<Tasks::FTask> InHandle, std::shared_ptr<std::atomic<uint32>> InCount)
				: Handle(std::move(InHandle)), Count(std::move(InCount))
			{
			}
			FReentrantDestruction(FReentrantDestruction&&) noexcept = default;
			FReentrantDestruction(const FReentrantDestruction&) = delete;
			~FReentrantDestruction()
			{
				if (!Ownership) return;
				// Cleanup precedes visible completion and may reenter task queries.
				EXPECT_EQ(ETaskState::Waiting, Handle->GetState());
				Count->fetch_add(1, std::memory_order::acq_rel);
			}
			auto operator()() -> void {}
		};

		FTaskLaunchOptions WaitingOptions;
		WaitingOptions.Prerequisites = std::span<const Tasks::FTask>(&Blocker, 1);
		*CanceledHandle = SubmitKernelTask("MoveOnlyReentrantDestruction",
			FReentrantDestruction(CanceledHandle, DestructionCount), WaitingOptions);
		ASSERT_TRUE(CanceledHandle->IsValid());
		ASSERT_TRUE(CancelTask(*CanceledHandle));
		EXPECT_EQ(1u, DestructionCount->load(std::memory_order::acquire));

		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
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
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.Priority = Priority;
			Options.EstimatedPayloadBytes = 1;
			return SubmitKernelContinuation(Root, "DeferredPriorityEntry", [&, Value]() { Order.push_back(Value); }, Options);
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

	TEST(FGameThreadDeferredTaskTests, CountAndPayloadLimitsRejectWithoutUnboundedGrowth)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.MaxQueuedEntries = 2;
		Config.MaxQueuedPayloadBytes = 10;
		Config.MaxPayloadBytesPerEntry = 8;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));
		Tasks::FTask Root = SubmitKernelTask("DeferredLimitRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		auto Enqueue = [&](uint64 Bytes) {
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = Bytes;
			return SubmitKernelContinuation(Root, "DeferredLimitEntry", []() {}, Options);
		};
		Tasks::FTask First = Enqueue(5);
		Tasks::FTask Second = Enqueue(5);
		Tasks::FTask CountRejected = Enqueue(1);
		Tasks::FTask EntryRejected = Enqueue(9);
		Tasks::FTask ZeroRejected = Enqueue(0);

		EXPECT_EQ(ETaskState::Queued, First.GetState());
		EXPECT_EQ(ETaskState::Queued, Second.GetState());
		EXPECT_EQ(ETaskState::Canceled, CountRejected.GetState());
		EXPECT_FALSE(EntryRejected.IsValid());
		EXPECT_FALSE(ZeroRejected.IsValid());
		EXPECT_TRUE(CancelTask(First));
		Tasks::FTask Replacement = Enqueue(5);
		EXPECT_EQ(ETaskState::Queued, Replacement.GetState());
		const auto Diagnostics = GetGameThreadDeferredWorkQueueDiagnostics();
		EXPECT_EQ(2u, Diagnostics.QueueDepth);
		EXPECT_EQ(10u, Diagnostics.QueuedPayloadBytes);
		EXPECT_EQ(1u, Diagnostics.RejectedCount);
		EXPECT_GE(Diagnostics.CanceledCount, 1u);
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

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		Tasks::FTask Slow = SubmitKernelContinuation(Root, "DeferredSlowCallback", []() {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}, Options);
		Tasks::FTask Later = SubmitKernelContinuation(Root, "DeferredLaterCallback", []() {}, Options);
		FGameThreadDeferredPumpBudget Budget;
		Budget.MaxCallbacks = 64;
		Budget.MaxSeconds = 0.0001;
		EXPECT_EQ(1u, PumpGameThreadDeferredWork(Budget).ExecutedCallbacks);
		EXPECT_EQ(ETaskState::Succeeded, Slow.GetState());
		EXPECT_EQ(ETaskState::Queued, Later.GetState());
		EXPECT_GE(GetGameThreadDeferredWorkQueueDiagnostics().LongCallbackCount, 1u);
		PumpGameThreadDeferredWork({.bUnlimited = true});
	}

	TEST(FGameThreadDeferredTaskTests, CoalescingAndGenerationChecksPublishStableReasons)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTask Root = SubmitKernelTask("DeferredPolicyRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskContinuationOptions CoalescedOptions;
		CoalescedOptions.Target = ETaskTarget::GameThreadDeferred;
		CoalescedOptions.EstimatedPayloadBytes = 4;
		CoalescedOptions.CoalescingKey = FTaskCoalescingKey{11, 22, 33};
		Tasks::FTask Superseded = SubmitKernelContinuation(Root, "DeferredSuperseded", []() {}, CoalescedOptions);
		std::atomic<bool> bReplacementRan = false;
		Tasks::FTask Replacement = SubmitKernelContinuation(Root, "DeferredReplacement", [&]() { bReplacementRan = true; }, CoalescedOptions);
		EXPECT_EQ(ETaskState::Canceled, Superseded.GetState());
		EXPECT_EQ(ETaskTerminalReason::Superseded, Superseded.GetDiagnostics().TerminalReason);
		const FTaskDiagnostics ReplacementDiagnostics = Replacement.GetDiagnostics();
		EXPECT_EQ(ETaskTarget::GameThreadDeferred, ReplacementDiagnostics.Target);
		EXPECT_EQ(4u, ReplacementDiagnostics.EstimatedPayloadBytes);
		EXPECT_EQ(11u, ReplacementDiagnostics.CoalescingOwnerDomain);
		EXPECT_EQ(22u, ReplacementDiagnostics.CoalescingWorkId);
		EXPECT_EQ(33u, ReplacementDiagnostics.CoalescingGeneration);

		FTaskGenerationSource Generation;
		FTaskContinuationOptions StaleOptions;
		StaleOptions.Target = ETaskTarget::GameThreadDeferred;
		StaleOptions.EstimatedPayloadBytes = 1;
		StaleOptions.GenerationToken = Generation.Capture();
		Tasks::FTask Stale = SubmitKernelContinuation(Root, "DeferredStale", []() {}, StaleOptions);
		Generation.Advance();

		PumpGameThreadDeferredWork({.bUnlimited = true});
		EXPECT_EQ(ETaskState::Succeeded, Replacement.GetState());
		EXPECT_TRUE(bReplacementRan.load());
		EXPECT_EQ(ETaskState::Canceled, Stale.GetState());
		EXPECT_EQ(ETaskTerminalReason::StaleGeneration, Stale.GetDiagnostics().TerminalReason);
		const auto Diagnostics = GetGameThreadDeferredWorkQueueDiagnostics();
		EXPECT_EQ(1u, Diagnostics.SupersededCount);
		EXPECT_EQ(1u, Diagnostics.ExpiredGenerationCount);
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

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		Tasks::FTask Failed = SubmitKernelContinuation(Root, "DeferredFailure", []() {
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
		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		std::atomic<bool> bDrainRan = false;
		Tasks::FTask DrainTail = SubmitKernelContinuation(DrainRoot, "CrossDrainTail", [&]() {
			bDrainRan = true;
			EXPECT_FALSE(SubmitKernelTask("RejectedShutdownRoot", []() {}).IsValid());
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
		Tasks::FTask CancelTail = SubmitKernelContinuation(CancelRoot, "CrossCancelTail", []() {}, Options);
		ASSERT_EQ(ETaskState::Queued, CancelTail.GetState());
		ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		EXPECT_EQ(ETaskState::Canceled, CancelTail.GetState());
		EXPECT_EQ(ETaskTerminalReason::ShutdownCanceled, CancelTail.GetDiagnostics().TerminalReason);
	}

	TEST(FGameThreadDeferredTaskTests, MixedGenerationCallbacksPreserveAccountingAndDropStaleWork)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.MaxQueuedEntries = 256;
		Config.MaxQueuedPayloadBytes = 256 * 64;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));
		Tasks::FTask Root = SubmitKernelTask("DeferredAccountingRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		constexpr uint32 CallbackCount = 16;
		constexpr uint32 StaleCallbackCount = 2;
		constexpr uint64 DeclaredCaptureBytes = 64;
		FTaskGenerationSource StaleGeneration;
		const FTaskGenerationToken StaleToken = StaleGeneration.Capture();
		std::atomic<uint32> ExecutedCount = 0;
		std::vector<Tasks::FTask> Handles;
		Handles.reserve(CallbackCount);
		for (uint32 Index = 0; Index < CallbackCount; ++Index)
		{
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = DeclaredCaptureBytes;
			if (Index < StaleCallbackCount) Options.GenerationToken = StaleToken;
			Handles.emplace_back(SubmitKernelContinuation(Root, "DeferredAccountingCallback", [&]() {
				ExecutedCount.fetch_add(1, std::memory_order::acq_rel);
			}, Options));
		}
		StaleGeneration.Advance();

		const FGameThreadDeferredWorkQueueDiagnostics BeforePump = GetGameThreadDeferredWorkQueueDiagnostics();
		ASSERT_EQ(CallbackCount, BeforePump.QueueDepth);
		ASSERT_EQ(CallbackCount * DeclaredCaptureBytes, BeforePump.QueuedPayloadBytes);
		const FGameThreadDeferredPumpResult Pump = PumpGameThreadDeferredWork({.bUnlimited = true});
		ASSERT_EQ(CallbackCount - StaleCallbackCount, Pump.ExecutedCallbacks);
		EXPECT_EQ(CallbackCount - StaleCallbackCount, ExecutedCount.load(std::memory_order::acquire));

		uint32 SucceededCount = 0;
		uint32 StaleCount = 0;
		for (const Tasks::FTask& Handle : Handles)
		{
			const FTaskDiagnostics Diagnostics = Handle.GetDiagnostics();
			SucceededCount += Diagnostics.State == ETaskState::Succeeded;
			StaleCount += Diagnostics.TerminalReason == ETaskTerminalReason::StaleGeneration;
		}
		EXPECT_EQ(CallbackCount - StaleCallbackCount, SucceededCount);
		EXPECT_EQ(StaleCallbackCount, StaleCount);
	}

	TEST(FTaskTests, InvalidHandlesAreNoOp)
	{
		Tasks::FTask InvalidHandle;

		EXPECT_FALSE(InvalidHandle.IsValid());
		EXPECT_TRUE(InvalidHandle.IsComplete());
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

	TEST(FTaskTests, LaunchTaskReturnsInvalidHandleWhenSchedulerIsStopped)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;

		Tasks::FTask Handle = SubmitKernelTask("RejectedTask", []() {});

		EXPECT_FALSE(Handle.IsValid());
		EXPECT_TRUE(Handle.IsComplete());
	}
	TEST(FTaskDiagnosticsTests, TerminalPublicationBarrierPublishesResultDiagnosticsAndDependentsTogether)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FThreadEvent TaskStarted;
		FThreadEvent AllowTaskReturn;
		FThreadEvent RawTerminalReached;
		FThreadEvent ReleasePublication;
		std::atomic<uint64> HookTaskId = 0;
		std::atomic<bool> bDependentRan = false;
		FTaskTerminalPublicationTestHookGuard HookGuard([&](uint64 TaskId) {
			Private::SetTaskTerminalPublicationTestHook({});
			HookTaskId.store(TaskId, std::memory_order::release);
			RawTerminalReached.Trigger();
			ReleasePublication.Wait();
		});

		auto Producer = Tasks::LaunchTask("PublicationBarrierProducer", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
			return 42;
		});
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		Tasks::FTask Dependent = Durin::Tasks::FTask(Tasks::Then(Producer, Tasks::ETaskExecutor::Worker, {.DebugName = "PublicationBarrierDependent"}, [&](const int& Value) {
			EXPECT_EQ(42, Value);
			bDependentRan.store(true, std::memory_order::release);
		}));
		AllowTaskReturn.Trigger();
		ASSERT_TRUE(RawTerminalReached.WaitFor(1.0));
		ASSERT_EQ(Producer.GetTaskId(), HookTaskId.load(std::memory_order::acquire));

		const FTaskDiagnostics DuringPublication = Producer.GetDiagnostics();
		EXPECT_EQ(ETaskState::Running, Producer.GetState());
		EXPECT_EQ(ETaskState::Running, DuringPublication.State);
		EXPECT_EQ(0u, DuringPublication.FinishTimeNanoseconds);
		EXPECT_EQ(ETaskTerminalReason::None, DuringPublication.TerminalReason);
		EXPECT_TRUE(DuringPublication.Diagnostic.empty());
		EXPECT_FALSE(DuringPublication.bHasResultStorage);
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

	TEST(FTaskDiagnosticsTests, ResultBindingAfterTerminalPublicationPreservesLifetimeCounters)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Task = SubmitKernelTask("LateResultBinding", [] {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);
		auto Storage = std::make_shared<TTaskResultState<int>>();
		Storage->SetPending(42);
		Storage->Complete(ETaskState::Succeeded);
		Storage->BindProducer(Task);
		EXPECT_TRUE(Task.GetDiagnostics().bHasResultStorage);
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().RetainedTerminalResultCount);
		ShutdownTaskScheduler(true);
		Task = {};
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

} // namespace Durin
