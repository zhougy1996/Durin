#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <iostream>

#include "Profiling/Profiling.h"
#include "Threading/QueuedThreadPool.h"
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
	static_assert(std::is_default_constructible_v<FTaskScope>);
	static_assert(std::is_move_constructible_v<FTaskScope>);
	static_assert(std::is_move_assignable_v<FTaskScope>);
	static_assert(!std::is_copy_constructible_v<FTaskScope>);
	static_assert(!std::is_copy_assignable_v<FTaskScope>);
	static_assert(std::is_default_constructible_v<FTaskScopeToken>);
	static_assert(std::is_copy_constructible_v<FTaskScopeToken>);
	static_assert(std::is_copy_assignable_v<FTaskScopeToken>);
	static_assert(std::is_move_constructible_v<FTaskScopeToken>);
	static_assert(std::is_move_assignable_v<FTaskScopeToken>);
	static_assert(std::equality_comparable<FTaskScopeToken>);
	static_assert(noexcept(Profiling::TaskEnqueued(1, 3, 2, 2, 0)));
	static_assert(noexcept(Profiling::TaskExecution(1, 3, 2, 2, 0)));
	static_assert(noexcept(Profiling::TaskTerminal(1, 3, 2, 2, 0, 0)));
	static_assert(noexcept(Profiling::TaskAggregatePlots(2, 2, 0, 0, 0, 0, 0, 0, 0)));

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
				ObservedResult = WaitTask(Target);
				ReturnedEvent.Trigger();
				return 0;
			}

			FTaskHandle Target;
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

		// Kernel fixtures deliberately observe recoverable admission and erased lifetime state.
		// Ordinary result/acceptance behavior is exercised by TaskCompositionTests.
		template<typename F>
		auto SubmitCancelableKernelTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle
		{
			auto Admission = Private::TryLaunchCancelableTaskWithCompletion(Name, std::forward<F>(Function), {}, Options);
			return Admission.HasValue() ? std::move(Admission).TakeValue() : FTaskHandle{};
		}
		template<typename F>
		auto SubmitKernelTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle
		{
			if constexpr (std::same_as<std::decay_t<F>, FTaskFunction>)
				if (!Function) return SubmitCancelableKernelTask(Name, Private::FMoveOnlyTaskFunction{}, Options);
			return SubmitCancelableKernelTask(Name,
				[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { std::invoke(Function); }, Options);
		}
		template<typename F>
		auto SubmitKernelContinuation(const FTaskHandle& Input, const char* Name, F&& Function,
			const FTaskContinuationOptions& Options = {}) -> FTaskHandle
		{
			auto Admission = Private::TryLaunchContinuationTask(Input, Name,
				[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { std::invoke(Function); },
				{}, Options, ETaskDependencyKind::Success);
			return Admission.HasValue() ? std::move(Admission).TakeValue() : FTaskHandle{};
		}

		auto EnsureGameThreadForTaskTest() -> void
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
		}
	} // namespace

	TEST(FQueuedThreadPoolTests, AllocationRollbackReleasesOwnerAndCallableOutsideLock)
	{
		FQueuedThreadPool Pool;
		ASSERT_TRUE(Pool.Create(1));
		bool Destroyed = false;
		auto Capture = std::shared_ptr<int>(new int(7), [&](int* Value) {
			EXPECT_EQ(0u, Pool.GetOwnerTagOutstandingCount(7));
			Destroyed = true;
			delete Value;
		});
		Private::SetQueuedWorkAllocationFailureForTests(true);
		EXPECT_THROW(Pool.Enqueue("FailedQueueAllocation", [Capture = std::move(Capture)] {}, {}, 7), std::bad_alloc);
		Private::SetQueuedWorkAllocationFailureForTests(false);
		EXPECT_TRUE(Destroyed);
		EXPECT_EQ(0u, Pool.GetNumQueuedTasks());
		EXPECT_TRUE(Pool.Enqueue("RetryQueueAllocation", [] {}, {}, 7));
		EXPECT_TRUE(Pool.WaitForOwnerTagIdle(7, 1.0));
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
		FTaskHandle Handle = SubmitKernelTask("EnginePoolTask", [&]() {
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

		FTaskHandle Handle = SubmitKernelTask("IdempotentInitialization", []() {});
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
		std::array<FTaskHandle, ProducerCount> Handles;
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
		EXPECT_FALSE(SubmitKernelTask("RejectedAfterClose", []() {}).IsValid());
	}

	TEST(FTaskScopeTests, DefaultsTraitsAndEmptyCloseRemainExplicit)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;

		FTaskScope InvalidScope;
		EXPECT_FALSE(InvalidScope.IsValid());
		EXPECT_EQ(FTaskScopeToken{}, InvalidScope.GetToken());
		EXPECT_EQ(ETaskScopeCloseResult::Invalid, InvalidScope.Close(ETaskScopeCloseMode::Drain));
		EXPECT_EQ(ETaskScopeWaitResult::Invalid, InvalidScope.Wait());
		EXPECT_EQ(ETaskScopeState::Invalid, InvalidScope.GetDiagnostics().State);
		EXPECT_FALSE(CreateTaskScope().IsValid());
		EXPECT_EQ(FTaskScopeToken{}, FTaskLaunchOptions{}.Scope);
		EXPECT_EQ(FTaskScopeToken{}, FTaskContinuationOptions{}.Scope);
		EXPECT_EQ(FTaskScopeToken{}, FParallelForOptions{}.Scope);

		ASSERT_TRUE(InitializeTaskScheduler(2));
		FTaskScope First = CreateTaskScope();
		FTaskScope Second = CreateTaskScope();
		ASSERT_TRUE(First.IsValid());
		ASSERT_TRUE(Second.IsValid());
		EXPECT_NE(First.GetToken(), Second.GetToken());
		EXPECT_NE(0u, First.GetDiagnostics().ScopeId);
		EXPECT_NE(First.GetDiagnostics().ScopeId, Second.GetDiagnostics().ScopeId);

		EXPECT_EQ(ETaskScopeCloseResult::Closed, First.Close(ETaskScopeCloseMode::Drain));
		EXPECT_EQ(ETaskScopeState::QuiescentDrain, First.GetDiagnostics().State);
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, First.Wait());
		EXPECT_EQ(ETaskScopeCloseResult::AlreadyClosed, First.Close(ETaskScopeCloseMode::Cancel));
	}

	TEST(FTaskScopeTests, PreCanceledTokenReconcilesScopedAdmission)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FTaskScope Scope = CreateTaskScope();
		ASSERT_TRUE(Scope.IsValid());
		FTaskCancellationSource Source;
		Source.RequestCancellation();
		FTaskLaunchOptions Options;
		Options.Scope = Scope.GetToken();
		Options.CancellationToken = Source.GetToken();
		std::atomic<bool> bRan = false;
		FTaskHandle Task = SubmitKernelTask("ScopedPreCanceledToken", [&] {
			bRan.store(true, std::memory_order_release);
		}, Options);

		ASSERT_TRUE(Task.IsValid());
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Task).TaskState);
		EXPECT_FALSE(bRan.load(std::memory_order_acquire));
		EXPECT_EQ(ETaskScopeCloseResult::Closed,
			Scope.Close(ETaskScopeCloseMode::Drain));
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, Scope.Wait());
		const FTaskScopeDiagnostics Diagnostics = Scope.GetDiagnostics();
		EXPECT_EQ(1u, Diagnostics.AcceptedCount);
		EXPECT_EQ(1u, Diagnostics.CanceledCount);
		EXPECT_EQ(0u, Diagnostics.CurrentActiveCount);
	}


	TEST(FTaskScopeTests, ConcurrentCloseLinearizesEveryAdmission)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));
		FTaskScope Scope = CreateTaskScope();
		ASSERT_TRUE(Scope.IsValid());

		constexpr uint32 ProducerCount = 32;
		std::barrier StartBarrier(ProducerCount + 1);
		std::array<FTaskHandle, ProducerCount> Handles;
		std::vector<std::thread> Producers;
		Producers.reserve(ProducerCount);
		const FTaskScopeToken Token = Scope.GetToken();
		for (uint32 ProducerIndex = 0; ProducerIndex < ProducerCount; ++ProducerIndex)
		{
			Producers.emplace_back([&, ProducerIndex, Token]() {
				FTaskLaunchOptions Options;
				Options.Scope = Token;
				StartBarrier.arrive_and_wait();
				Handles[ProducerIndex] = SubmitKernelTask("ScopedAdmissionRace", []() {}, Options);
			});
		}

		StartBarrier.arrive_and_wait();
		EXPECT_EQ(ETaskScopeCloseResult::Closed, Scope.Close(ETaskScopeCloseMode::Drain));
		for (std::thread& Producer : Producers) Producer.join();

		uint64 AcceptedCount = 0;
		for (const FTaskHandle& Handle : Handles)
		{
			if (!Handle.IsValid()) continue;
			++AcceptedCount;
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Handle).TaskState);
		}
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, Scope.Wait());
		const FTaskScopeDiagnostics Diagnostics = Scope.GetDiagnostics();
		EXPECT_EQ(AcceptedCount, Diagnostics.AcceptedCount);
		EXPECT_EQ(ProducerCount - AcceptedCount, Diagnostics.RejectedCount);
		EXPECT_EQ(AcceptedCount, Diagnostics.SucceededCount);
		EXPECT_EQ(0u, Diagnostics.CurrentActiveCount);
	}

	TEST(FTaskScopeTests, CancelWaitAndTerminalPublicationPreserveFailurePrecedence)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FTaskScope Scope = CreateTaskScope();
		FTaskLaunchOptions Options;
		Options.Scope = Scope.GetToken();

		FThreadEvent Started;
		FThreadEvent ReleaseCallable;
		FThreadEvent RawTerminalReached;
		FThreadEvent ReleasePublication;
		std::atomic<bool> bObservedCancellation = false;
		FTaskTerminalPublicationTestHookGuard HookGuard([&](uint64) {
			Private::SetTaskTerminalPublicationTestHook({});
			RawTerminalReached.Trigger();
			ReleasePublication.Wait();
		});
		FTaskHandle Task = SubmitCancelableKernelTask("ScopedCancelFailure", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			ReleaseCallable.Wait();
			bObservedCancellation.store(Token.IsCancellationRequested(), std::memory_order::release);
			throw std::runtime_error("scope cancellation lost to failure");
		}, Options);
		ASSERT_TRUE(Started.WaitFor(1.0));

		EXPECT_EQ(ETaskScopeCloseResult::Closed, Scope.Close(ETaskScopeCloseMode::Drain));
		EXPECT_EQ(ETaskScopeWaitResult::TimedOut, Scope.WaitFor(0.001));
		EXPECT_EQ(ETaskScopeCloseResult::EscalatedToCancel, Scope.Close(ETaskScopeCloseMode::Cancel));
		EXPECT_EQ(ETaskScopeCloseResult::AlreadyClosed, Scope.Close(ETaskScopeCloseMode::Cancel));
		ReleaseCallable.Trigger();
		ASSERT_TRUE(RawTerminalReached.WaitFor(1.0));
		EXPECT_EQ(1u, Scope.GetDiagnostics().CurrentActiveCount);
		EXPECT_EQ(ETaskScopeWaitResult::TimedOut, Scope.WaitFor(0.001));
		ReleasePublication.Trigger();

		EXPECT_EQ(ETaskState::Failed, WaitTask(Task).TaskState);
		EXPECT_TRUE(bObservedCancellation.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, Scope.Wait());
		const FTaskScopeDiagnostics Diagnostics = Scope.GetDiagnostics();
		EXPECT_EQ(1u, Diagnostics.AcceptedCount);
		EXPECT_EQ(1u, Diagnostics.FailedCount);
		EXPECT_EQ(0u, Diagnostics.CanceledCount);
		EXPECT_EQ(0u, Diagnostics.CurrentActiveCount);
	}

	TEST(FTaskScopeTests, WorkerHelpsAnotherClosedScopeWhileOwnScopeWaitIsRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FTaskScope WaitedScope = CreateTaskScope();
		FTaskScope OwningScope = CreateTaskScope();
		FThreadEvent OuterStarted;
		FThreadEvent BeginWait;
		std::atomic<ETaskScopeWaitResult> OtherWaitResult = ETaskScopeWaitResult::Invalid;
		std::atomic<ETaskScopeWaitResult> OwnWaitResult = ETaskScopeWaitResult::Invalid;
		FTaskLaunchOptions OuterOptions;
		OuterOptions.Scope = OwningScope.GetToken();
		FTaskHandle Outer = SubmitKernelTask("ScopedWorkerWaiter", [&]() {
			OuterStarted.Trigger();
			BeginWait.Wait();
			OwnWaitResult.store(OwningScope.WaitFor(0.001), std::memory_order::release);
			OtherWaitResult.store(WaitedScope.WaitFor(1.0), std::memory_order::release);
		}, OuterOptions);
		ASSERT_TRUE(OuterStarted.WaitFor(1.0));

		FTaskLaunchOptions WaitedOptions;
		WaitedOptions.Scope = WaitedScope.GetToken();
		FTaskHandle Helped = SubmitKernelTask("ScopedWorkerHelped", []() {}, WaitedOptions);
		ASSERT_TRUE(Helped.IsValid());
		EXPECT_EQ(ETaskScopeCloseResult::Closed, WaitedScope.Close(ETaskScopeCloseMode::Drain));
		EXPECT_EQ(ETaskScopeCloseResult::Closed, OwningScope.Close(ETaskScopeCloseMode::Drain));
		BeginWait.Trigger();

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Outer).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, Helped.GetState());
		EXPECT_EQ(ETaskScopeWaitResult::UnsupportedThread, OwnWaitResult.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, OtherWaitResult.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, OwningScope.Wait());
	}

	TEST(FTaskScopeTests, ChildLaunchAndCancelCloseRaceReconcilesEveryDescendant)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		FTaskScope Scope = CreateTaskScope();
		FTaskLaunchOptions Options;
		Options.Scope = Scope.GetToken();
		std::barrier StartRace(2);
		std::vector<FTaskHandle> Children;
		FTaskHandle Parent = SubmitCancelableKernelTask("ScopedCancelRaceParent", [&](const FTaskCancellationToken&) {
			StartRace.arrive_and_wait();
			for (uint32 Index = 0; Index < 64; ++Index)
			{
				Children.emplace_back(SubmitKernelTask("ScopedCancelRaceChild", []() {}));
			}
		}, Options);
		ASSERT_TRUE(Parent.IsValid());
		StartRace.arrive_and_wait();
		EXPECT_EQ(ETaskScopeCloseResult::Closed, Scope.Close(ETaskScopeCloseMode::Cancel));
		auto IsTerminal = [](ETaskState State) {
			return State == ETaskState::Succeeded || State == ETaskState::Failed || State == ETaskState::Canceled;
		};
		EXPECT_TRUE(IsTerminal(WaitTask(Parent).TaskState));
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, Scope.Wait());

		uint64 AcceptedChildren = 0;
		for (const FTaskHandle& Child : Children)
		{
			if (!Child.IsValid()) continue;
			++AcceptedChildren;
			EXPECT_TRUE(IsTerminal(Child.GetState()));
		}
		const FTaskScopeDiagnostics Diagnostics = Scope.GetDiagnostics();
		EXPECT_EQ(1u + AcceptedChildren, Diagnostics.AcceptedCount);
		EXPECT_EQ(64u - AcceptedChildren, Diagnostics.RejectedCount);
		EXPECT_EQ(Diagnostics.AcceptedCount, Diagnostics.SucceededCount + Diagnostics.FailedCount + Diagnostics.CanceledCount);
		EXPECT_EQ(0u, Diagnostics.CurrentActiveCount);
	}

	TEST(FTaskScopeTests, DiagnosticsStayBoundedAcrossConcurrentTerminalRelease)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FTaskScope Scope = CreateTaskScope();
		FTaskLaunchOptions Options;
		Options.Scope = Scope.GetToken();
		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = SubmitKernelTask("ScopedDiagnosticBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		}, Options);
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		std::vector<FTaskHandle> Handles;
		for (uint32 Index = 0; Index < 80; ++Index)
		{
			Handles.emplace_back(SubmitKernelContinuation(Blocker, "ScopedDiagnosticWaiting", []() {}));
		}

		const FTaskScopeDiagnostics BeforeClose = Scope.GetDiagnostics();
		ASSERT_EQ(81u, BeforeClose.CurrentActiveCount);
		EXPECT_EQ(64u, BeforeClose.NonterminalTasks.size());
		EXPECT_EQ(17u, BeforeClose.NonterminalSnapshotTruncationCount);
		EXPECT_TRUE(std::ranges::is_sorted(BeforeClose.NonterminalTasks, {}, &FTaskDiagnostics::TaskId));
		std::atomic<bool> bSnapshotsReconciled = true;
		std::thread SnapshotThread([&]() {
			for (uint32 Index = 0; Index < 100; ++Index)
			{
				const FTaskScopeDiagnostics Snapshot = Scope.GetDiagnostics();
				if (Snapshot.NonterminalTasks.size() > 64
					|| Snapshot.AcceptedCount != Snapshot.SucceededCount + Snapshot.FailedCount + Snapshot.CanceledCount + Snapshot.CurrentActiveCount)
				{
					bSnapshotsReconciled.store(false, std::memory_order::release);
				}
			}
		});
		EXPECT_EQ(ETaskScopeCloseResult::Closed, Scope.Close(ETaskScopeCloseMode::Cancel));
		Handles.clear();
		ReleaseBlocker.Trigger();
		SnapshotThread.join();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, Scope.Wait());
		EXPECT_TRUE(bSnapshotsReconciled.load(std::memory_order::acquire));
		const FTaskScopeDiagnostics Final = Scope.GetDiagnostics();
		EXPECT_EQ(81u, Final.AcceptedCount);
		EXPECT_EQ(Final.AcceptedCount, Final.SucceededCount + Final.FailedCount + Final.CanceledCount);
		EXPECT_EQ(0u, Final.CurrentActiveCount);
		EXPECT_TRUE(Final.NonterminalTasks.empty());
	}

	TEST(FTaskScopeTests, SchedulerTracksAbandonmentRejectionsAndShutdownClosure)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		{
			FTaskScope Abandoned = CreateTaskScope();
			const FTaskSchedulerDiagnostics Live = GetTaskSchedulerDiagnostics();
			EXPECT_EQ(1u, Live.LiveScopeCount);
			EXPECT_EQ(1u, Live.OpenScopeCount);
			EXPECT_EQ(1u, Live.NonquiescentScopeCount);
		}
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().AbandonedOpenScopeCount);

		FTaskScope Scope = CreateTaskScope();
		FTaskLaunchOptions Options;
		Options.Scope = Scope.GetToken();
		FThreadEvent Started;
		FTaskHandle Running = SubmitCancelableKernelTask("ScopedShutdownRunning", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			while (!Token.IsCancellationRequested()) std::this_thread::yield();
		}, Options);
		ASSERT_TRUE(Started.WaitFor(1.0));
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().NonquiescentScopeCount);
		ShutdownTaskScheduler(false);

		EXPECT_EQ(ETaskState::Canceled, Running.GetState());
		EXPECT_EQ(ETaskScopeState::QuiescentCancel, Scope.GetDiagnostics().State);
		const FTaskSchedulerDiagnostics Shutdown = GetTaskSchedulerDiagnostics();
		EXPECT_FALSE(Shutdown.bRunning);
		EXPECT_EQ(1u, Shutdown.LiveScopeCount);
		EXPECT_EQ(0u, Shutdown.OpenScopeCount);
		EXPECT_EQ(0u, Shutdown.NonquiescentScopeCount);
		EXPECT_EQ(1u, Shutdown.AbandonedOpenScopeCount);

		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_FALSE(SubmitKernelTask("RejectedOldScope", []() {}, Options).IsValid());
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().ScopeRejectedTaskCount);
	}

	TEST(FTaskScopeTests, GameThreadRejectsWaitForScopedDeferredWork)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FTaskScope Scope = CreateTaskScope();
		FTaskLaunchOptions RootOptions;
		RootOptions.Scope = Scope.GetToken();
		FTaskHandle Root = SubmitKernelTask("ScopedDeferredRoot", []() {}, RootOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		FTaskContinuationOptions DeferredOptions;
		DeferredOptions.Target = ETaskTarget::GameThreadDeferred;
		DeferredOptions.EstimatedPayloadBytes = 1;
		FTaskHandle Deferred = SubmitKernelContinuation(Root, "ScopedDeferredWaitTarget", []() {}, DeferredOptions);
		ASSERT_EQ(ETaskState::Queued, Deferred.GetState());
		EXPECT_EQ(ETaskScopeCloseResult::Closed, Scope.Close(ETaskScopeCloseMode::Drain));
		EXPECT_EQ(ETaskScopeWaitResult::UnsupportedThread, Scope.WaitFor(0.001));
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		EXPECT_EQ(ETaskState::Succeeded, Deferred.GetState());
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, Scope.Wait());
	}

	TEST(FTaskTests, LaunchTaskReturnsValidHandleAndCompletes)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		std::atomic<bool> bTaskRan = false;
		FTaskHandle Handle = SubmitKernelTask("HandleCompletion", [&]() {
			bTaskRan.store(true, std::memory_order::release);
		});

		ASSERT_TRUE(Handle.IsValid());
		EXPECT_STREQ("HandleCompletion", Handle.GetDebugName());

		WaitTask(Handle).TaskState;

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
			Handles.push_back(SubmitKernelTask("WaitAllTask", [&]() {
				CompletedTaskCount.fetch_add(1, std::memory_order::acq_rel);
			}));
			ASSERT_TRUE(Handles.back().IsValid());
		}

		const std::vector<FTaskWaitResult> Outcomes = WaitAll(std::span<const FTaskHandle>(Handles.data(), Handles.size()));

		EXPECT_EQ(TaskCount, CompletedTaskCount.load(std::memory_order::acquire));
		ASSERT_EQ(TaskCount, Outcomes.size());
		for (const FTaskWaitResult& Outcome : Outcomes)
		{
			EXPECT_EQ(ETaskWaitStatus::Completed, Outcome.WaitStatus);
			EXPECT_EQ(ETaskState::Succeeded, Outcome.TaskState);
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

		FTaskHandle TargetHandle = SubmitKernelTask("TargetTask", [&]() {
			bTargetTaskRan.store(true, std::memory_order::release);
		});
		ASSERT_TRUE(TargetHandle.IsValid());

		FTaskHandle BlockedHandle = SubmitKernelTask("BlockedTask", [&]() {
			BlockedTaskStarted.Trigger();
			ReleaseBlockedTask.Wait();
		});
		ASSERT_TRUE(BlockedHandle.IsValid());

		WaitTask(TargetHandle).TaskState;

		EXPECT_TRUE(TargetHandle.IsComplete());
		EXPECT_TRUE(bTargetTaskRan.load(std::memory_order::acquire));
		ASSERT_TRUE(BlockedTaskStarted.WaitFor(1.0));
		EXPECT_FALSE(BlockedHandle.IsComplete());

		ReleaseBlockedTask.Trigger();
		WaitTask(BlockedHandle).TaskState;
	}

	TEST(FTaskTests, WorkerWaitHelpsNestedTaskOnSingleWorkerPool)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::atomic<bool> bChildTaskRan = false;
		std::atomic<bool> bChildHandleWasValid = false;
		std::atomic<bool> bParentTaskFinished = false;

		FTaskHandle ParentHandle = SubmitKernelTask("ParentTask", [&]() {
			FTaskHandle ChildHandle = SubmitKernelTask("ChildTask", [&]() {
				bChildTaskRan.store(true, std::memory_order::release);
			});

			bChildHandleWasValid.store(ChildHandle.IsValid(), std::memory_order::release);
			WaitTask(ChildHandle).TaskState;
			bParentTaskFinished.store(true, std::memory_order::release);
		});
		ASSERT_TRUE(ParentHandle.IsValid());

		WaitTask(ParentHandle).TaskState;

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
		FTaskHandle Blocker = SubmitKernelTask("IneligibleWaitBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		FTaskHandle Parent;
		FTaskHandle DependentChild;
		std::atomic<ETaskWaitStatus> ObservedWaitStatus = ETaskWaitStatus::InvalidTask;
		std::atomic<ETaskState> ObservedWaitState = ETaskState::Invalid;
		Parent = SubmitKernelTask("IneligibleWaitParent", [&]() {
			std::array<FTaskHandle, 1> Prerequisites{Parent};
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

		FTaskHandle StandardFailure = SubmitKernelTask("StandardFailure", []() {
			throw std::runtime_error("expected task failure");
		});
		FTaskHandle UnknownFailure = SubmitKernelTask("UnknownFailure", []() {
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
		FTaskHandle RunningTask = SubmitKernelTask("RunningDuringDiscard", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle QueuedTask = SubmitKernelTask("QueuedDuringDiscard", []() {});
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
		FTaskHandle RunningTask = SubmitKernelTask("RunningDuringDrain", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle QueuedTask = SubmitKernelTask("QueuedDuringDrain", []() {});

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
		FTaskHandle BlockingTask = SubmitKernelTask("DiscardRaceBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		std::atomic<uint32> ExecutionCount = 0;
		FTaskHandle RacingTask = SubmitKernelTask("DiscardRaceTarget", [&]() {
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
		FTaskHandle Root = SubmitKernelTask("DependencyRoot", [&]() {
			ReleaseRoot.Wait();
			ExecutionMask.fetch_or(1u, std::memory_order::acq_rel);
		});

		std::array<FTaskHandle, 1> RootPrerequisite{Root};
		FTaskLaunchOptions RootOptions;
		RootOptions.Prerequisites = RootPrerequisite;
		FTaskHandle Left = SubmitKernelTask("DependencyLeft", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 1u) == 0)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(2u, std::memory_order::acq_rel); }, RootOptions);
		FTaskHandle Right = SubmitKernelTask("DependencyRight", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 1u) == 0)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(4u, std::memory_order::acq_rel); }, RootOptions);

		std::array<FTaskHandle, 2> JoinPrerequisites{Left, Right};
		FTaskLaunchOptions JoinOptions;
		JoinOptions.Prerequisites = JoinPrerequisites;
		FTaskHandle Join = SubmitKernelTask("DependencyJoin", [&]() {
			if ((ExecutionMask.load(std::memory_order::acquire) & 7u) != 7u)
			{
				bOrderValid.store(false, std::memory_order::release);
			}
			ExecutionMask.fetch_or(8u, std::memory_order::acq_rel); }, JoinOptions);

		std::array<FTaskHandle, 1> JoinPrerequisite{Join};
		FTaskLaunchOptions TailOptions;
		TailOptions.Prerequisites = JoinPrerequisite;
		FTaskHandle Tail = SubmitKernelTask("DependencyTail", [&]() {
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
			FTaskHandle Prerequisite = SubmitKernelTask("RegistrationRacePrerequisite", [&]() {
				RaceBarrier.arrive_and_wait();
			});
			FTaskHandle Dependent;
			std::thread Submitter([&]() {
				RaceBarrier.arrive_and_wait();
				std::array<FTaskHandle, 1> Prerequisites{Prerequisite};
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
		FTaskHandle First = SubmitKernelTask("SimultaneousFirst", [&]() {
			CompletionBarrier.arrive_and_wait();
		});
		FTaskHandle Second = SubmitKernelTask("SimultaneousSecond", [&]() {
			CompletionBarrier.arrive_and_wait();
		});
		std::array<FTaskHandle, 2> Prerequisites{First, Second};
		FTaskLaunchOptions Options;
		Options.Prerequisites = Prerequisites;
		std::atomic<uint32> ExecutionCount = 0;
		FTaskHandle Dependent = SubmitKernelTask("SimultaneousFanIn", [&]() { ExecutionCount.fetch_add(1, std::memory_order::acq_rel); }, Options);
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

		FTaskHandle InvalidPrerequisite;
		std::array<FTaskHandle, 1> InvalidPrerequisites{InvalidPrerequisite};
		FTaskLaunchOptions InvalidOptions;
		InvalidOptions.Prerequisites = InvalidPrerequisites;
		EXPECT_FALSE(SubmitKernelTask("InvalidPrerequisiteTask", []() {}, InvalidOptions).IsValid());

		FTaskHandle EarlierLifetimeTask = SubmitKernelTask("EarlierLifetimeTask", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(EarlierLifetimeTask).TaskState);
		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::array<FTaskHandle, 1> ForeignPrerequisites{EarlierLifetimeTask};
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
		FTaskHandle FailedPrerequisite = SubmitKernelTask("FailedPrerequisite", []() {
			throw std::runtime_error("dependency failure");
		});
		std::array<FTaskHandle, 1> FailedPrerequisites{FailedPrerequisite};
		FTaskLaunchOptions FailedOptions;
		FailedOptions.Prerequisites = FailedPrerequisites;
		FTaskHandle FailedDependent = SubmitKernelTask("FailedDependent", [&]() { bFailedDependentRan.store(true, std::memory_order::release); }, FailedOptions);

		EXPECT_EQ(ETaskState::Failed, WaitTask(FailedPrerequisite).TaskState);
		EXPECT_EQ(ETaskState::Canceled, WaitTask(FailedDependent).TaskState);
		EXPECT_FALSE(bFailedDependentRan.load(std::memory_order::acquire));
		EXPECT_NE(std::string::npos, FailedDependent.GetDiagnostic().find(std::to_string(FailedPrerequisite.GetTaskId())));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = SubmitKernelTask("CancellationPropagationBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		FTaskHandle CanceledPrerequisite = SubmitKernelTask("CanceledPrerequisite", []() {});
		std::array<FTaskHandle, 1> CanceledPrerequisites{CanceledPrerequisite};
		FTaskLaunchOptions CanceledOptions;
		CanceledOptions.Prerequisites = CanceledPrerequisites;
		std::atomic<bool> bCanceledDependentRan = false;
		FTaskHandle CanceledDependent = SubmitKernelTask("CanceledDependent", [&]() { bCanceledDependentRan.store(true, std::memory_order::release); }, CanceledOptions);

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
		FTaskHandle Running = SubmitCancelableKernelTask("SharedCancellationRunning", [&](const FTaskCancellationToken& Token) {
			RunningStarted.Trigger();
			ReleaseRunning.Wait();
			bRunningObservedCancellation.store(Token.IsCancellationRequested(), std::memory_order::release); }, SharedOptions);
		ASSERT_TRUE(RunningStarted.WaitFor(1.0));

		std::atomic<bool> bQueuedRan = false;
		FTaskHandle Queued = SubmitKernelTask("SharedCancellationQueued", [&]() { bQueuedRan.store(true, std::memory_order::release); }, SharedOptions);
		std::array<FTaskHandle, 1> QueuedPrerequisite{Queued};
		FTaskLaunchOptions WaitingOptions = SharedOptions;
		WaitingOptions.Prerequisites = QueuedPrerequisite;
		std::atomic<bool> bWaitingRan = false;
		FTaskHandle Waiting = SubmitKernelTask("SharedCancellationWaiting", [&]() { bWaitingRan.store(true, std::memory_order::release); }, WaitingOptions);

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
		FTaskHandle PreCanceledTask = SubmitKernelTask("PreCanceledSourceTask", [&]() { bPreCanceledTaskRan.store(true, std::memory_order::release); }, PreCanceledOptions);
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
		FTaskHandle Task = SubmitCancelableKernelTask("ExceptionAfterCancellation", [&](const FTaskCancellationToken& Token) {
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

		FTaskHandle CompletedTask = SubmitKernelTask("CompletedBeforeCancellation", []() {});
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
		FTaskHandle Task = SubmitKernelTask("MultipleWaiters", [&]() {
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
		FTaskHandle DrainRoot = SubmitKernelTask("DrainGraphRoot", [&]() {
			DrainRootStarted.Trigger();
			ReleaseDrainRoot.Wait();
		});
		ASSERT_TRUE(DrainRootStarted.WaitFor(1.0));
		std::array<FTaskHandle, 1> DrainPrerequisite{DrainRoot};
		FTaskLaunchOptions DrainOptions;
		DrainOptions.Prerequisites = DrainPrerequisite;
		FTaskHandle DrainDependent = SubmitKernelTask("DrainGraphDependent", []() {}, DrainOptions);
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
		FTaskHandle DiscardRoot = SubmitKernelTask("DiscardGraphRoot", [&]() {
			DiscardRootStarted.Trigger();
			ReleaseDiscardRoot.Wait();
		});
		ASSERT_TRUE(DiscardRootStarted.WaitFor(1.0));
		std::array<FTaskHandle, 1> DiscardPrerequisite{DiscardRoot};
		FTaskLaunchOptions DiscardOptions;
		DiscardOptions.Prerequisites = DiscardPrerequisite;
		FTaskHandle DiscardDependent = SubmitKernelTask("DiscardGraphDependent", []() {}, DiscardOptions);
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
		FTaskHandle BlockingTask = SubmitKernelTask("SelfWaitBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));

		FTaskHandle SelfTask;
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

	TEST(FTaskTests, RenderingThreadWaitIsRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockingTaskStarted;
		FThreadEvent ReleaseBlockingTask;
		FTaskHandle BlockingTask = SubmitKernelTask("RenderingWaitBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle QueuedTask = SubmitKernelTask("RenderingWaitTarget", []() {});

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
		FTaskHandle BlockingTask = SubmitKernelTask("ShutdownBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));
		FTaskHandle RetainedTask = SubmitKernelTask("RetainedAcrossRestart", []() {});
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
		FTaskHandle NewTask = SubmitKernelTask("SequentialFixtureRestart", []() {});
		ASSERT_TRUE(NewTask.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(NewTask).TaskState);
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

	TEST(FTaskDiagnosticsTests, ReportsRelationshipsTimingCountersLongWaitsAndRetainedHandles)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = SubmitKernelTask("DiagnosticBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		FTaskHandle Queued = SubmitKernelTask("DiagnosticQueued", []() {});

		FTaskHandle InvalidPrerequisite;
		std::array<FTaskHandle, 1> InvalidPrerequisites{InvalidPrerequisite};
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
			WaitTask(Blocker).TaskState;
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
		FTaskHandle Child;
		FTaskHandle Parent = SubmitKernelTask("DiagnosticParent", [&]() {
			AllowParentWork.Wait();
			Child = SubmitKernelTask("DiagnosticChild", []() {});
			WaitTask(Child).TaskState;
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

		FTaskHandle Prerequisite = SubmitKernelTask("DiagnosticPrerequisite", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Prerequisite).TaskState);
		std::array<FTaskHandle, 1> Prerequisites{Prerequisite};
		FTaskLaunchOptions DependentOptions;
		DependentOptions.Prerequisites = Prerequisites;
		FTaskHandle Dependent = SubmitKernelTask("DiagnosticDependent", []() {}, DependentOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Dependent).TaskState);
		ASSERT_EQ(1u, Dependent.GetDiagnostics().PrerequisiteTaskIds.size());
		EXPECT_EQ(Prerequisite.GetTaskId(), Dependent.GetDiagnostics().PrerequisiteTaskIds[0]);

		FTaskHandle Failed = SubmitKernelTask("DiagnosticFailure", []() {
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

		FTaskHandle Running = SubmitKernelTask("SnapshotContentionRunning", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
		});
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		FTaskHandle Waiting = SubmitKernelContinuation(Running, "SnapshotContentionWaiting", []() {});
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
		FTaskHandle Root = SubmitKernelTask("SnapshotContentionRoot", []() {});
		FTaskHandle LateContinuation = SubmitKernelContinuation(Running, "SnapshotContentionLateContinuation", []() {});
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

	TEST(FTaskAdmissionTests, ReportsLifetimePrerequisiteAndScopeRejection)
	{
		using ECode = Tasks::ETaskAdmissionErrorCode;
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		auto Spawn = [](const FTaskLaunchOptions& Options = {}) {
			return Private::TryLaunchCancelableTaskWithCompletion("AdmissionRoot",
				[](const FTaskCancellationToken&) {}, {}, Options);
		};
		auto Closed = Spawn();
		ASSERT_FALSE(Closed.HasValue());
		EXPECT_EQ(ECode::LifetimeClosed, Closed.GetError().Code);
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Empty = Private::TryLaunchCancelableTaskWithCompletion("Empty", {}, {}, {});
		ASSERT_FALSE(Empty.HasValue());
		EXPECT_EQ(ECode::InvalidCallable, Empty.GetError().Code);
		FTaskHandle Invalid;
		FTaskLaunchOptions Options;
		Options.Prerequisites = std::span<const FTaskHandle>(&Invalid, 1);
		auto BadPrerequisite = Spawn(Options);
		ASSERT_FALSE(BadPrerequisite.HasValue());
		EXPECT_EQ(ECode::InvalidPrerequisite, BadPrerequisite.GetError().Code);
		EXPECT_EQ(0u, BadPrerequisite.GetError().RelatedTaskId);

		auto Accepted = Spawn();
		ASSERT_TRUE(Accepted.HasValue());
		auto Previous = std::move(Accepted).TakeValue();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Previous).TaskState);
		ShutdownTaskScheduler(false);
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Options.Prerequisites = std::span<const FTaskHandle>(&Previous, 1);
		auto OldLifetime = Spawn(Options);
		ASSERT_FALSE(OldLifetime.HasValue());
		EXPECT_EQ(ECode::InvalidPrerequisite, OldLifetime.GetError().Code);
		EXPECT_EQ(Previous.GetTaskId(), OldLifetime.GetError().RelatedTaskId);

		auto Scope = CreateTaskScope();
		Options = {};
		Options.Scope = Scope.GetToken();
		Scope.Close(ETaskScopeCloseMode::Drain);
		auto ClosedGroup = Spawn(Options);
		ASSERT_FALSE(ClosedGroup.HasValue());
		EXPECT_EQ(ECode::GroupClosed, ClosedGroup.GetError().Code);
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
		auto Continue = [&](const FTaskHandle& Predecessor, const FTaskContinuationOptions& Options) {
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
		auto Scope = CreateTaskScope();
		Options.Scope = Scope.GetToken();
		auto Reparent = Continue(Root, Options);
		ASSERT_FALSE(Reparent.HasValue());
		EXPECT_EQ(ECode::GroupClosed, Reparent.GetError().Code);
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
		FTaskHandle Blocker = SubmitKernelTask("CapacityConcurrentBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		std::mutex AcceptedMutex;
		std::vector<FTaskHandle> Accepted;
		std::vector<std::thread> Producers;
		for (uint32 Index = 0; Index < 32; ++Index)
		{
			Producers.emplace_back([&]() {
				FTaskHandle Task = SubmitKernelTask("CapacityConcurrentProducer", []() {});
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
		for (const FTaskHandle& Task : Accepted) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
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
		FTaskHandle DrainRoot = SubmitKernelTask("CapacityDrainRoot", [&]() {
			DrainStarted.Trigger();
			ReleaseDrain.Wait();
		});
		ASSERT_TRUE(DrainStarted.WaitFor(1.0));
		FTaskLaunchOptions DrainWaitingOptions;
		DrainWaitingOptions.Prerequisites = std::span<const FTaskHandle>(&DrainRoot, 1);
		std::vector<FTaskHandle> DrainTasks;
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
		FTaskHandle Running = SubmitCancelableKernelTask("CapacityCancelRunning", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			while (!Token.IsCancellationRequested()) std::this_thread::yield();
		});
		ASSERT_TRUE(Started.WaitFor(1.0));
		FTaskLaunchOptions WaitingOptions;
		WaitingOptions.Prerequisites = std::span<const FTaskHandle>(&Running, 1);
		std::vector<FTaskHandle> Waiting;
		for (uint32 Index = 0; Index < 7; ++Index) Waiting.emplace_back(SubmitKernelTask("CapacityCancelWaiting", []() {}, WaitingOptions));
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		ShutdownTaskScheduler(false);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().PeakTaskReservationCount);
		EXPECT_EQ(ETaskState::Canceled, Running.GetState());
		for (const FTaskHandle& Task : Waiting) EXPECT_EQ(ETaskState::Canceled, Task.GetState());
	}



	TEST(FTaskMoveOnlyCallableTests, ErasureMovesInlineAndHeapTargetsAndDestroysExactlyOnce)
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

		using FMoveOnlyVoidFunction = Private::TMoveOnlyFunction<void()>;
		FMoveOnlyVoidFunction Empty;
		EXPECT_FALSE(static_cast<bool>(Empty));
		EXPECT_EQ(0u, Empty.GetStorageBytes());

		auto InlineValue = std::make_unique<int>(7);
		int* InlineAddress = InlineValue.get();
		FMoveOnlyVoidFunction Inline([Value = std::move(InlineValue), &InlineAddress]() {
			EXPECT_EQ(InlineAddress, Value.get());
		});
		FMoveOnlyVoidFunction MovedInline(std::move(Inline));
		EXPECT_FALSE(static_cast<bool>(Inline));
		EXPECT_EQ(0u, Inline.GetStorageBytes());
		EXPECT_EQ(sizeof(void*) * 3, MovedInline.GetStorageBytes());
		ASSERT_TRUE(static_cast<bool>(MovedInline));
		MovedInline();

		auto DestructionCount = std::make_shared<std::atomic<uint32>>(0);
		{
			FMoveOnlyVoidFunction Heap{FTrackedCallable(DestructionCount)};
			FMoveOnlyVoidFunction MovedHeap(std::move(Heap));
			EXPECT_FALSE(static_cast<bool>(Heap));
			EXPECT_EQ(0u, Heap.GetStorageBytes());
			EXPECT_EQ(sizeof(FTrackedCallable), MovedHeap.GetStorageBytes());
			MovedHeap();
		}
		EXPECT_EQ(1u, DestructionCount->load(std::memory_order::acquire));

		FMoveOnlyVoidFunction Throwing([]() { throw std::runtime_error("move-only failure"); });
		EXPECT_THROW(Throwing(), std::runtime_error);
	}


	TEST(FTaskMoveOnlyCallableTests, CancellationDestroysCaptureOutsideTaskStateLock)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = SubmitKernelTask("MoveOnlyDestructionBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		auto CanceledHandle = std::make_shared<FTaskHandle>();
		auto DestructionCount = std::make_shared<std::atomic<uint32>>(0);
		struct FReentrantDestruction
		{
			std::shared_ptr<FTaskHandle> Handle;
			std::shared_ptr<std::atomic<uint32>> Count;
			std::unique_ptr<int> Ownership = std::make_unique<int>(1);

			FReentrantDestruction(std::shared_ptr<FTaskHandle> InHandle, std::shared_ptr<std::atomic<uint32>> InCount)
				: Handle(std::move(InHandle)), Count(std::move(InCount))
			{
			}
			FReentrantDestruction(FReentrantDestruction&&) noexcept = default;
			FReentrantDestruction(const FReentrantDestruction&) = delete;
			~FReentrantDestruction()
			{
				if (!Ownership) return;
				EXPECT_EQ(ETaskState::Canceled, Handle->GetState());
				Count->fetch_add(1, std::memory_order::acq_rel);
			}
			auto operator()() -> void {}
		};

		FTaskLaunchOptions WaitingOptions;
		WaitingOptions.Prerequisites = std::span<const FTaskHandle>(&Blocker, 1);
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
		FTaskHandle Root = SubmitKernelTask("DeferredPriorityRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		std::vector<int> Order;
		auto Enqueue = [&](int Value, ETaskPriority Priority) {
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.Priority = Priority;
			Options.EstimatedPayloadBytes = 1;
			return SubmitKernelContinuation(Root, "DeferredPriorityEntry", [&, Value]() { Order.push_back(Value); }, Options);
		};
		FTaskHandle Low = Enqueue(1, ETaskPriority::Low);
		FTaskHandle HighFirst = Enqueue(2, ETaskPriority::High);
		FTaskHandle Normal = Enqueue(3, ETaskPriority::Normal);
		FTaskHandle HighSecond = Enqueue(4, ETaskPriority::High);

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
		FTaskHandle Root = SubmitKernelTask("DeferredLimitRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		auto Enqueue = [&](uint64 Bytes) {
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = Bytes;
			return SubmitKernelContinuation(Root, "DeferredLimitEntry", []() {}, Options);
		};
		FTaskHandle First = Enqueue(5);
		FTaskHandle Second = Enqueue(5);
		FTaskHandle CountRejected = Enqueue(1);
		FTaskHandle EntryRejected = Enqueue(9);
		FTaskHandle ZeroRejected = Enqueue(0);

		EXPECT_EQ(ETaskState::Queued, First.GetState());
		EXPECT_EQ(ETaskState::Queued, Second.GetState());
		EXPECT_EQ(ETaskState::Canceled, CountRejected.GetState());
		EXPECT_FALSE(EntryRejected.IsValid());
		EXPECT_FALSE(ZeroRejected.IsValid());
		EXPECT_TRUE(CancelTask(First));
		FTaskHandle Replacement = Enqueue(5);
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
		FTaskHandle Root = SubmitKernelTask("DeferredTimeBudgetRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		FTaskHandle Slow = SubmitKernelContinuation(Root, "DeferredSlowCallback", []() {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}, Options);
		FTaskHandle Later = SubmitKernelContinuation(Root, "DeferredLaterCallback", []() {}, Options);
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
		FTaskHandle Root = SubmitKernelTask("DeferredPolicyRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskContinuationOptions CoalescedOptions;
		CoalescedOptions.Target = ETaskTarget::GameThreadDeferred;
		CoalescedOptions.EstimatedPayloadBytes = 4;
		CoalescedOptions.CoalescingKey = FTaskCoalescingKey{11, 22, 33};
		FTaskHandle Superseded = SubmitKernelContinuation(Root, "DeferredSuperseded", []() {}, CoalescedOptions);
		std::atomic<bool> bReplacementRan = false;
		FTaskHandle Replacement = SubmitKernelContinuation(Root, "DeferredReplacement", [&]() { bReplacementRan = true; }, CoalescedOptions);
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
		FTaskHandle Stale = SubmitKernelContinuation(Root, "DeferredStale", []() {}, StaleOptions);
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
		FTaskHandle Root = SubmitKernelTask("DeferredFailureRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		FTaskHandle Failed = SubmitKernelContinuation(Root, "DeferredFailure", []() {
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
		FTaskHandle DrainRoot = SubmitKernelTask("CrossDrainRoot", [&]() {
			DrainRootStarted.Trigger();
			ReleaseDrainRoot.Wait();
		});
		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		std::atomic<bool> bDrainRan = false;
		FTaskHandle DrainTail = SubmitKernelContinuation(DrainRoot, "CrossDrainTail", [&]() {
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
		FTaskHandle CancelRoot = SubmitKernelTask("CrossCancelRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(CancelRoot).TaskState);
		FTaskHandle CancelTail = SubmitKernelContinuation(CancelRoot, "CrossCancelTail", []() {}, Options);
		ASSERT_EQ(ETaskState::Queued, CancelTail.GetState());
		ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		EXPECT_EQ(ETaskState::Canceled, CancelTail.GetState());
		EXPECT_EQ(ETaskTerminalReason::ShutdownCanceled, CancelTail.GetDiagnostics().TerminalReason);
	}

	TEST(FGameThreadDeferredTaskTests, RepresentativeWorkloadMeasuresAdmissionPumpResidencyAndStaleDrops)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.MaxQueuedEntries = 256;
		Config.MaxQueuedPayloadBytes = 256 * 64;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));
		FTaskHandle Root = SubmitKernelTask("DeferredMeasurementRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		constexpr uint32 CallbackCount = 256;
		constexpr uint32 StaleCallbackCount = 32;
		constexpr uint64 DeclaredCaptureBytes = 64;
		FTaskGenerationSource StaleGeneration;
		const FTaskGenerationToken StaleToken = StaleGeneration.Capture();
		std::atomic<uint32> ExecutedCount = 0;
		std::vector<FTaskHandle> Handles;
		Handles.reserve(CallbackCount);
		const auto AdmissionStart = std::chrono::steady_clock::now();
		for (uint32 Index = 0; Index < CallbackCount; ++Index)
		{
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = DeclaredCaptureBytes;
			if (Index < StaleCallbackCount) Options.GenerationToken = StaleToken;
			Handles.emplace_back(SubmitKernelContinuation(Root, "DeferredMeasurementCallback", [&]() {
				ExecutedCount.fetch_add(1, std::memory_order::acq_rel);
			}, Options));
		}
		const uint64 AdmissionNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - AdmissionStart).count());
		StaleGeneration.Advance();

		const FGameThreadDeferredWorkQueueDiagnostics BeforePump = GetGameThreadDeferredWorkQueueDiagnostics();
		ASSERT_EQ(CallbackCount, BeforePump.QueueDepth);
		ASSERT_EQ(CallbackCount * DeclaredCaptureBytes, BeforePump.QueuedPayloadBytes);
		const FGameThreadDeferredPumpResult Pump = PumpGameThreadDeferredWork({.bUnlimited = true});
		ASSERT_EQ(CallbackCount - StaleCallbackCount, Pump.ExecutedCallbacks);
		EXPECT_EQ(CallbackCount - StaleCallbackCount, ExecutedCount.load(std::memory_order::acquire));

		uint64 TotalResidencyNanoseconds = 0;
		uint64 MaxResidencyNanoseconds = 0;
		uint32 SucceededCount = 0;
		uint32 StaleCount = 0;
		for (const FTaskHandle& Handle : Handles)
		{
			const FTaskDiagnostics Diagnostics = Handle.GetDiagnostics();
			TotalResidencyNanoseconds += Diagnostics.QueueResidencyNanoseconds;
			MaxResidencyNanoseconds = std::max(MaxResidencyNanoseconds, Diagnostics.QueueResidencyNanoseconds);
			SucceededCount += Diagnostics.State == ETaskState::Succeeded;
			StaleCount += Diagnostics.TerminalReason == ETaskTerminalReason::StaleGeneration;
		}
		EXPECT_EQ(CallbackCount - StaleCallbackCount, SucceededCount);
		EXPECT_EQ(StaleCallbackCount, StaleCount);
		EXPECT_LT(Pump.ElapsedNanoseconds, 1'000'000'000u);
		std::cout << "[ QUALIFICATION ] game_thread_deferred callbacks=" << CallbackCount
			<< " declared_capture_bytes=" << CallbackCount * DeclaredCaptureBytes
			<< " admission_ns=" << AdmissionNanoseconds
			<< " pump_ns=" << Pump.ElapsedNanoseconds
			<< " average_residency_ns=" << TotalResidencyNanoseconds / CallbackCount
			<< " max_residency_ns=" << MaxResidencyNanoseconds
			<< " stale_drop_count=" << StaleCount
			<< " stale_drop_ppm=" << (static_cast<uint64>(StaleCount) * 1'000'000 / CallbackCount)
			<< '\n';
	}

	TEST(FTaskTests, InvalidHandlesAreNoOp)
	{
		FTaskHandle InvalidHandle;

		EXPECT_FALSE(InvalidHandle.IsValid());
		EXPECT_FALSE(InvalidHandle.IsComplete());
		EXPECT_EQ(ETaskState::Invalid, InvalidHandle.GetState());
		EXPECT_STREQ("", InvalidHandle.GetDebugName());

		const FTaskWaitResult WaitResult = WaitTask(InvalidHandle);
		EXPECT_EQ(ETaskWaitStatus::InvalidTask, WaitResult.WaitStatus);
		EXPECT_EQ(ETaskState::Invalid, WaitResult.TaskState);
		const std::vector<FTaskWaitResult> WaitResults = WaitAll(std::span<const FTaskHandle>(&InvalidHandle, 1));
		ASSERT_EQ(1u, WaitResults.size());
		EXPECT_EQ(ETaskWaitStatus::InvalidTask, WaitResults[0].WaitStatus);
		EXPECT_EQ(ETaskState::Invalid, WaitResults[0].TaskState);
	}

	TEST(FTaskTests, LaunchTaskReturnsInvalidHandleWhenSchedulerIsStopped)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;

		FTaskHandle Handle = SubmitKernelTask("RejectedTask", []() {});

		EXPECT_FALSE(Handle.IsValid());
		EXPECT_FALSE(Handle.IsComplete());
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

		auto Producer = Tasks::Share(Tasks::LaunchTask("PublicationBarrierProducer", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
			return 42;
		}));
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		FTaskHandle Dependent = Tasks::Then(Producer, Tasks::ETaskExecutor::Worker, {.DebugName = "PublicationBarrierDependent"}, [&](const int& Value) {
			EXPECT_EQ(42, Value);
			bDependentRan.store(true, std::memory_order::release);
		}).GetCompletion().GetTaskHandle();
		AllowTaskReturn.Trigger();
		ASSERT_TRUE(RawTerminalReached.WaitFor(1.0));
		ASSERT_EQ(Producer.GetCompletion().GetTaskHandle().GetTaskId(), HookTaskId.load(std::memory_order::acquire));

		const FTaskDiagnostics DuringPublication = Producer.GetDiagnostics();
		EXPECT_EQ(ETaskState::Running, Producer.GetState());
		EXPECT_EQ(ETaskState::Running, DuringPublication.State);
		EXPECT_EQ(0u, DuringPublication.FinishTimeNanoseconds);
		EXPECT_EQ(ETaskTerminalReason::None, DuringPublication.TerminalReason);
		EXPECT_TRUE(DuringPublication.Diagnostic.empty());
		EXPECT_FALSE(DuringPublication.bHasResultStorage);
		EXPECT_EQ(0u, DuringPublication.RetainedResultBytes);
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

	TEST(FTaskDiagnosticsTests, LifetimeCountersRemainIsolatedAcrossStoppedAndRestartedSchedulers)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto PreviousLifetime = Tasks::Share(Tasks::LaunchTask("PreviousLifetime", []() { return 7; }));
		auto ReleasedWhileStopped = Tasks::Share(Tasks::LaunchTask("ReleasedWhileStopped", []() { return 8; }));
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(PreviousLifetime.GetCompletion().GetTaskHandle()).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(ReleasedWhileStopped.GetCompletion().GetTaskHandle()).TaskState);
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
