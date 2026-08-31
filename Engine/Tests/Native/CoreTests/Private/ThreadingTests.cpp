#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <iostream>

#include "Profiling/Profiling.h"
#include "Threading/QueuedThreadPool.h"
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

		struct FMoveOnlyVoidCallable
		{
			std::unique_ptr<int> Value = std::make_unique<int>(1);
			FMoveOnlyVoidCallable() = default;
			FMoveOnlyVoidCallable(FMoveOnlyVoidCallable&&) noexcept = default;
			auto operator=(FMoveOnlyVoidCallable&&) noexcept -> FMoveOnlyVoidCallable& = default;
			FMoveOnlyVoidCallable(const FMoveOnlyVoidCallable&) = delete;
			auto operator=(const FMoveOnlyVoidCallable&) -> FMoveOnlyVoidCallable& = delete;
			auto operator()() -> void {}
		};

		struct FMoveOnlyCancelableCallable : FMoveOnlyVoidCallable
		{
			auto operator()(const FTaskCancellationToken&) -> void {}
		};

		struct FMoveOnlyIntCallable : FMoveOnlyVoidCallable
		{
			auto operator()() -> int { return *Value; }
		};

		struct FWrongTaskCallable
		{
			auto operator()(int) -> void {}
		};

		struct FMoveOnlyTypedContinuation : FMoveOnlyVoidCallable
		{
			auto operator()(const int& Predecessor) -> int { return Predecessor + *Value; }
		};

		struct FMoveOnlyVoidOutcomeContinuation : FMoveOnlyVoidCallable
		{
			auto operator()(FTaskOutcome<void>) -> void {}
		};

		struct FMoveOnlyTypedOutcomeContinuation : FMoveOnlyVoidCallable
		{
			auto operator()(FTaskOutcome<int>) -> void {}
		};

		struct FMoveOnlyFanInContinuation : FMoveOnlyVoidCallable
		{
			auto operator()(const int& Left, const std::string& Right) -> int { return Left + static_cast<int>(Right.size()); }
		};

		struct FMoveOnlyFanInOutcomeContinuation : FMoveOnlyVoidCallable
		{
			auto operator()(TTaskAggregateOutcome<int, std::string>) -> void {}
		};

		struct FWrongFanInContinuation : FMoveOnlyVoidCallable
		{
			auto operator()(const int&) -> void {}
		};

		struct FFanInReferenceResultContinuation : FMoveOnlyVoidCallable
		{
			int Result = 0;
			auto operator()(const int&, const std::string&) -> int& { return Result; }
		};

		struct FReferenceResultCallable
		{
			int Value = 0;
			auto operator()() -> int& { return Value; }
		};

		struct FCompileMoveOnlyValue
		{
			explicit FCompileMoveOnlyValue(int InValue) : Value(std::make_unique<int>(InValue)) {}
			FCompileMoveOnlyValue(FCompileMoveOnlyValue&&) noexcept = default;
			auto operator=(FCompileMoveOnlyValue&&) noexcept -> FCompileMoveOnlyValue& = default;
			FCompileMoveOnlyValue(const FCompileMoveOnlyValue&) = delete;
			auto operator=(const FCompileMoveOnlyValue&) -> FCompileMoveOnlyValue& = delete;
			std::unique_ptr<int> Value;
		};

		struct FCompileUniqueProducer : FMoveOnlyVoidCallable
		{
			auto operator()() -> FCompileMoveOnlyValue { return FCompileMoveOnlyValue(*Value); }
		};

		struct FCompileUniqueSink : FMoveOnlyVoidCallable
		{
			auto operator()(FCompileMoveOnlyValue&&) -> void {}
		};

		struct FCompileUniqueOutcomeSink : FMoveOnlyVoidCallable
		{
			auto operator()(FUniqueTaskOutcome<FCompileMoveOnlyValue>&&) -> void {}
		};

		template<typename F>
		concept CCanLaunchVoidTask = requires(F&& Function) {
			{ LaunchTask("CompileFixture", std::forward<F>(Function)) } -> std::same_as<FTaskHandle>;
		};

		template<typename F>
		concept CCanLaunchCancelableVoidTask = requires(F&& Function) {
			{ LaunchCancelableTask("CompileFixture", std::forward<F>(Function)) } -> std::same_as<FTaskHandle>;
		};

		template<typename F>
		concept CCanLaunchExactIntTask = requires(F&& Function) {
			{ LaunchTask<int>("CompileFixture", std::forward<F>(Function)) } -> std::same_as<TTaskHandle<int>>;
		};

		template<typename F>
		concept CCanThenVoidTask = requires(FTaskHandle Handle, F&& Function) {
			Then(Handle, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanThenTypedTask = requires(TTaskHandle<int> Handle, F&& Function) {
			Then(Handle, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanThenVoidOutcomeTask = requires(FTaskHandle Handle, F&& Function) {
			ThenOutcome(Handle, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanThenTypedOutcomeTask = requires(TTaskHandle<int> Handle, F&& Function) {
			ThenOutcome(Handle, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanWhenAllTask = requires(std::tuple<TTaskHandle<int>, TTaskHandle<std::string>> Handles, F&& Function) {
			WhenAll(Handles, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanWhenAllOutcomeTask = requires(std::tuple<TTaskHandle<int>, TTaskHandle<std::string>> Handles, F&& Function) {
			WhenAllOutcome(Handles, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanWhenAllEmptyTask = requires(std::tuple<> Handles, F&& Function) {
			WhenAll(Handles, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanWhenAllVoidTask = requires(std::tuple<TTaskHandle<void>> Handles, F&& Function) {
			WhenAll(Handles, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanWhenAllUniqueTask = requires(std::tuple<TUniqueTaskHandle<FCompileMoveOnlyValue>> Handles, F&& Function) {
			WhenAll(Handles, "CompileFixture", std::forward<F>(Function));
		};

		template<typename F>
		concept CCanLaunchUniqueTask = requires(F&& Function) {
			{ LaunchUniqueTask<FCompileMoveOnlyValue>("CompileFixture", std::forward<F>(Function)) }
				-> std::same_as<TUniqueTaskHandle<FCompileMoveOnlyValue>>;
		};

		template<typename F>
		concept CCanConsumeUniqueTask = requires(TUniqueTaskHandle<FCompileMoveOnlyValue> Handle, F&& Function) {
			{ ConsumeThen(std::move(Handle), "CompileFixture", std::forward<F>(Function)) } -> std::same_as<FTaskHandle>;
		};

		template<typename F>
		concept CCanConsumeUniqueOutcomeTask = requires(TUniqueTaskHandle<FCompileMoveOnlyValue> Handle, F&& Function) {
			{ ConsumeThenOutcome(std::move(Handle), "CompileFixture", std::forward<F>(Function)) } -> std::same_as<FTaskHandle>;
		};

		template<typename F>
		concept CCanConsumeUniqueTaskLvalue = requires(TUniqueTaskHandle<FCompileMoveOnlyValue> Handle, F&& Function) {
			ConsumeThen(Handle, "CompileFixture", std::forward<F>(Function));
		};

		template<typename Handle>
		concept CHasSharedResultObserver = requires(const Handle& Value) { Value.GetResultShared(); };

		static_assert(std::is_move_constructible_v<Private::FMoveOnlyTaskFunction>);
		static_assert(std::is_nothrow_move_constructible_v<Private::FMoveOnlyTaskFunction>);
		static_assert(!std::is_copy_constructible_v<Private::FMoveOnlyTaskFunction>);
		static_assert(CCanLaunchVoidTask<FTaskFunction>);
		static_assert(CCanLaunchVoidTask<FMoveOnlyVoidCallable>);
		static_assert(CCanLaunchCancelableVoidTask<FCancelableTaskFunction>);
		static_assert(CCanLaunchCancelableVoidTask<FMoveOnlyCancelableCallable>);
		static_assert(CCanLaunchExactIntTask<FMoveOnlyIntCallable>);
		static_assert(CCanThenVoidTask<FMoveOnlyVoidCallable>);
		static_assert(CCanThenTypedTask<FMoveOnlyTypedContinuation>);
		static_assert(CCanThenVoidOutcomeTask<FMoveOnlyVoidOutcomeContinuation>);
		static_assert(CCanThenTypedOutcomeTask<FMoveOnlyTypedOutcomeContinuation>);
		static_assert(CCanWhenAllTask<FMoveOnlyFanInContinuation>);
		static_assert(CCanWhenAllOutcomeTask<FMoveOnlyFanInOutcomeContinuation>);
		static_assert(!CCanWhenAllTask<FWrongFanInContinuation>);
		static_assert(!CCanWhenAllTask<FFanInReferenceResultContinuation>);
		static_assert(!CCanWhenAllEmptyTask<FMoveOnlyVoidCallable>);
		static_assert(!CCanWhenAllVoidTask<FMoveOnlyVoidCallable>);
		static_assert(!CCanWhenAllUniqueTask<FCompileUniqueSink>);
		static_assert(!CCanLaunchVoidTask<FWrongTaskCallable>);
		static_assert(!CCanLaunchExactIntTask<FMoveOnlyVoidCallable>);
		static_assert(!CCanThenVoidTask<FReferenceResultCallable>);
		static_assert(std::is_move_constructible_v<TUniqueTaskHandle<FCompileMoveOnlyValue>>);
		static_assert(!std::is_copy_constructible_v<TUniqueTaskHandle<FCompileMoveOnlyValue>>);
		static_assert(!CHasSharedResultObserver<TUniqueTaskHandle<FCompileMoveOnlyValue>>);
		static_assert(CCanLaunchUniqueTask<FCompileUniqueProducer>);
		static_assert(CCanConsumeUniqueTask<FCompileUniqueSink>);
		static_assert(CCanConsumeUniqueOutcomeTask<FCompileUniqueOutcomeSink>);
		static_assert(!CCanConsumeUniqueTask<FCompileUniqueOutcomeSink>);
		static_assert(!CCanConsumeUniqueTaskLvalue<FCompileUniqueSink>);

		auto EnsureGameThreadForTaskTest() -> void
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
		}
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

		FTaskHandle Handle = LaunchTask("IdempotentInitialization", []() {});
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
		FTaskHandle Task = LaunchTask("ScopedPreCanceledToken", [&] {
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

	TEST(FTaskScopeTests, ExplicitAndInheritedSelectionCoversEveryTaskForm)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));

		FTaskScope ScopeA = CreateTaskScope();
		FTaskScope ScopeB = CreateTaskScope();
		ASSERT_TRUE(ScopeA.IsValid());
		ASSERT_TRUE(ScopeB.IsValid());
		const uint64 ScopeAId = ScopeA.GetDiagnostics().ScopeId;
		const uint64 ScopeBId = ScopeB.GetDiagnostics().ScopeId;
		FTaskLaunchOptions RootAOptions;
		RootAOptions.Scope = ScopeA.GetToken();
		FTaskLaunchOptions RootBOptions;
		RootBOptions.Scope = ScopeB.GetToken();

		auto RootA = LaunchTask<int>("ScopedRootA", []() { return 7; }, RootAOptions);
		auto RootB = LaunchTask<std::string>("ScopedRootB", []() { return std::string("scope-b"); }, RootBOptions);
		ASSERT_TRUE(RootA.IsValid());
		ASSERT_TRUE(RootB.IsValid());

		FTaskHandle InheritedChild;
		FTaskHandle MatchingExplicitChild;
		FTaskHandle ReparentedChild;
		FTaskHandle NestedParent = LaunchTask("ScopedNestedParent", [&]() {
			InheritedChild = LaunchTask("ScopedInheritedChild", []() {});
			FTaskLaunchOptions MatchingOptions;
			MatchingOptions.Scope = ScopeA.GetToken();
			MatchingExplicitChild = LaunchTask("ScopedMatchingChild", []() {}, MatchingOptions);
			FTaskLaunchOptions ReparentOptions;
			ReparentOptions.Scope = ScopeB.GetToken();
			ReparentedChild = LaunchTask("ScopedReparentedChild", []() {}, ReparentOptions);
		}, RootAOptions);
		ASSERT_TRUE(NestedParent.IsValid());

		FTaskHandle Continuation = Then(RootA, "ScopedContinuation", [](const int&) {});
		std::array<FTaskHandle, 1> AdditionalPrerequisites{RootB.GetTaskHandle()};
		FTaskContinuationOptions CrossScopePrerequisiteOptions;
		CrossScopePrerequisiteOptions.Prerequisites = AdditionalPrerequisites;
		FTaskHandle CrossScopePrerequisite = Then(RootA, "ScopedCrossScopePrerequisite", [](const int&) {}, CrossScopePrerequisiteOptions);
		FTaskContinuationOptions ReparentContinuationOptions;
		ReparentContinuationOptions.Scope = ScopeB.GetToken();
		FTaskHandle ReparentedContinuation = Then(RootA, "ScopedReparentedContinuation", [](const int&) {}, ReparentContinuationOptions);
		auto FanIn = WhenAll(std::make_tuple(RootA, RootB), "ScopedFanIn",
			[](const int& Number, const std::string& Text) { return Number + static_cast<int>(Text.size()); });

		auto UniqueProducer = LaunchUniqueTask<int>("ScopedUniqueProducer", []() { return 11; }, RootAOptions, sizeof(int));
		FTaskHandle UniqueSink = ConsumeThen(std::move(UniqueProducer), "ScopedUniqueSink", [](int&&) {});

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(NestedParent).TaskState);
		ASSERT_TRUE(InheritedChild.IsValid());
		ASSERT_TRUE(MatchingExplicitChild.IsValid());
		EXPECT_FALSE(ReparentedChild.IsValid());
		EXPECT_FALSE(ReparentedContinuation.IsValid());
		const uint64 AcceptedBeforeParallelFor = ScopeA.GetDiagnostics().AcceptedCount;
		FParallelForOptions ParallelOptions;
		ParallelOptions.MinBatchSize = 1;
		ParallelOptions.Scope = ScopeA.GetToken();
		const FParallelForResult ParallelResult = ParallelFor("ScopedParallelFor", 64, [](uint64) {}, ParallelOptions);
		ASSERT_EQ(ETaskState::Succeeded, ParallelResult.State);
		EXPECT_EQ(ParallelResult.ChunkCount - 1, ScopeA.GetDiagnostics().AcceptedCount - AcceptedBeforeParallelFor);

		for (const FTaskHandle& Task : std::array{
			RootA.GetTaskHandle(), RootB.GetTaskHandle(), InheritedChild, MatchingExplicitChild,
			Continuation, CrossScopePrerequisite, FanIn.GetTaskHandle(), UniqueSink})
		{
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		}

		EXPECT_EQ(ScopeAId, RootA.GetDiagnostics().ScopeId);
		EXPECT_EQ(ScopeBId, RootB.GetDiagnostics().ScopeId);
		EXPECT_EQ(ScopeAId, InheritedChild.GetDiagnostics().ScopeId);
		EXPECT_EQ(ScopeAId, MatchingExplicitChild.GetDiagnostics().ScopeId);
		EXPECT_EQ(ScopeAId, Continuation.GetDiagnostics().ScopeId);
		EXPECT_EQ(ScopeAId, CrossScopePrerequisite.GetDiagnostics().ScopeId);
		EXPECT_EQ(ScopeAId, FanIn.GetDiagnostics().ScopeId);
		EXPECT_EQ(ScopeAId, UniqueSink.GetDiagnostics().ScopeId);

		EXPECT_EQ(ETaskScopeCloseResult::Closed, ScopeA.Close(ETaskScopeCloseMode::Drain));
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, ScopeA.Wait());
		FTaskLaunchOptions ClosedOptions;
		ClosedOptions.Scope = ScopeA.GetToken();
		EXPECT_FALSE(LaunchTask("ScopedRejectedAfterClose", []() {}, ClosedOptions).IsValid());
		const FTaskScopeDiagnostics ScopeADiagnostics = ScopeA.GetDiagnostics();
		EXPECT_EQ(ETaskScopeState::QuiescentDrain, ScopeADiagnostics.State);
		EXPECT_EQ(0u, ScopeADiagnostics.CurrentActiveCount);
		EXPECT_EQ(ScopeADiagnostics.AcceptedCount,
			ScopeADiagnostics.SucceededCount + ScopeADiagnostics.FailedCount + ScopeADiagnostics.CanceledCount);
		EXPECT_EQ(1u, ScopeADiagnostics.RejectedCount);
		EXPECT_EQ(2u, ScopeB.GetDiagnostics().RejectedCount);
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
				Handles[ProducerIndex] = LaunchTask("ScopedAdmissionRace", []() {}, Options);
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
		FTaskHandle Task = LaunchCancelableTask("ScopedCancelFailure", [&](const FTaskCancellationToken& Token) {
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
		FTaskHandle Outer = LaunchTask("ScopedWorkerWaiter", [&]() {
			OuterStarted.Trigger();
			BeginWait.Wait();
			OwnWaitResult.store(OwningScope.WaitFor(0.001), std::memory_order::release);
			OtherWaitResult.store(WaitedScope.WaitFor(1.0), std::memory_order::release);
		}, OuterOptions);
		ASSERT_TRUE(OuterStarted.WaitFor(1.0));

		FTaskLaunchOptions WaitedOptions;
		WaitedOptions.Scope = WaitedScope.GetToken();
		FTaskHandle Helped = LaunchTask("ScopedWorkerHelped", []() {}, WaitedOptions);
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
		FTaskHandle Parent = LaunchCancelableTask("ScopedCancelRaceParent", [&](const FTaskCancellationToken&) {
			StartRace.arrive_and_wait();
			for (uint32 Index = 0; Index < 64; ++Index)
			{
				Children.emplace_back(LaunchTask("ScopedCancelRaceChild", []() {}));
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
		FTaskHandle Blocker = LaunchTask("ScopedDiagnosticBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		}, Options);
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		std::vector<FTaskHandle> Handles;
		for (uint32 Index = 0; Index < 80; ++Index)
		{
			Handles.emplace_back(Then(Blocker, "ScopedDiagnosticWaiting", []() {}));
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
		FTaskHandle Running = LaunchCancelableTask("ScopedShutdownRunning", [&](const FTaskCancellationToken& Token) {
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
		EXPECT_FALSE(LaunchTask("RejectedOldScope", []() {}, Options).IsValid());
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
		FTaskHandle Root = LaunchTask("ScopedDeferredRoot", []() {}, RootOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		FTaskContinuationOptions DeferredOptions;
		DeferredOptions.Target = ETaskTarget::GameThreadDeferred;
		DeferredOptions.EstimatedPayloadBytes = 1;
		FTaskHandle Deferred = Then(Root, "ScopedDeferredWaitTarget", []() {}, DeferredOptions);
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
		FTaskHandle Handle = LaunchTask("HandleCompletion", [&]() {
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
			Handles.push_back(LaunchTask("WaitAllTask", [&]() {
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

		FTaskHandle TargetHandle = LaunchTask("TargetTask", [&]() {
			bTargetTaskRan.store(true, std::memory_order::release);
		});
		ASSERT_TRUE(TargetHandle.IsValid());

		FTaskHandle BlockedHandle = LaunchTask("BlockedTask", [&]() {
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

		FTaskHandle ParentHandle = LaunchTask("ParentTask", [&]() {
			FTaskHandle ChildHandle = LaunchTask("ChildTask", [&]() {
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
		FTaskHandle Blocker = LaunchTask("IneligibleWaitBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

		FTaskHandle Parent;
		FTaskHandle DependentChild;
		std::atomic<ETaskWaitStatus> ObservedWaitStatus = ETaskWaitStatus::InvalidTask;
		std::atomic<ETaskState> ObservedWaitState = ETaskState::Invalid;
		Parent = LaunchTask("IneligibleWaitParent", [&]() {
			std::array<FTaskHandle, 1> Prerequisites{Parent};
			FTaskLaunchOptions Options;
			Options.Prerequisites = Prerequisites;
			DependentChild = LaunchTask("IneligibleWaitChild", []() {}, Options);
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

		FTaskHandle StandardFailure = LaunchTask("StandardFailure", []() {
			throw std::runtime_error("expected task failure");
		});
		FTaskHandle UnknownFailure = LaunchTask("UnknownFailure", []() {
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
		EXPECT_FALSE(LaunchTask("InvalidPrerequisiteTask", []() {}, InvalidOptions).IsValid());

		FTaskHandle EarlierLifetimeTask = LaunchTask("EarlierLifetimeTask", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(EarlierLifetimeTask).TaskState);
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

		EXPECT_EQ(ETaskState::Failed, WaitTask(FailedPrerequisite).TaskState);
		EXPECT_EQ(ETaskState::Canceled, WaitTask(FailedDependent).TaskState);
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
		FTaskHandle PreCanceledTask = LaunchTask("PreCanceledSourceTask", [&]() { bPreCanceledTaskRan.store(true, std::memory_order::release); }, PreCanceledOptions);
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
		FTaskHandle Task = LaunchCancelableTask("ExceptionAfterCancellation", [&](const FTaskCancellationToken& Token) {
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

		FTaskHandle CompletedTask = LaunchTask("CompletedBeforeCancellation", []() {});
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
		FTaskHandle BlockingTask = LaunchTask("SelfWaitBlocker", [&]() {
			BlockingTaskStarted.Trigger();
			ReleaseBlockingTask.Wait();
		});
		ASSERT_TRUE(BlockingTaskStarted.WaitFor(1.0));

		FTaskHandle SelfTask;
		std::atomic<ETaskWaitStatus> ObservedWaitStatus = ETaskWaitStatus::InvalidTask;
		std::atomic<ETaskState> ObservedWaitState = ETaskState::Invalid;
		SelfTask = LaunchTask("SelfWait", [&]() {
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
		FTaskHandle Parent = LaunchTask("DiagnosticParent", [&]() {
			AllowParentWork.Wait();
			Child = LaunchTask("DiagnosticChild", []() {});
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

		FTaskHandle Prerequisite = LaunchTask("DiagnosticPrerequisite", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Prerequisite).TaskState);
		std::array<FTaskHandle, 1> Prerequisites{Prerequisite};
		FTaskLaunchOptions DependentOptions;
		DependentOptions.Prerequisites = Prerequisites;
		FTaskHandle Dependent = LaunchTask("DiagnosticDependent", []() {}, DependentOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Dependent).TaskState);
		ASSERT_EQ(1u, Dependent.GetDiagnostics().PrerequisiteTaskIds.size());
		EXPECT_EQ(Prerequisite.GetTaskId(), Dependent.GetDiagnostics().PrerequisiteTaskIds[0]);

		FTaskHandle Failed = LaunchTask("DiagnosticFailure", []() {
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

		auto Producer = LaunchTask<int>("PublicationBarrierProducer", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
			return 42;
		});
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		FTaskHandle Dependent = Then(Producer, "PublicationBarrierDependent", [&](const int& Value) {
			EXPECT_EQ(42, Value);
			bDependentRan.store(true, std::memory_order::release);
		});
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

		FTaskHandle Running = LaunchTask("SnapshotContentionRunning", [&]() {
			TaskStarted.Trigger();
			AllowTaskReturn.Wait();
		});
		ASSERT_TRUE(TaskStarted.WaitFor(1.0));
		FTaskHandle Waiting = Then(Running, "SnapshotContentionWaiting", []() {});
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
		FTaskHandle Root = LaunchTask("SnapshotContentionRoot", []() {});
		FTaskHandle LateContinuation = Then(Running, "SnapshotContentionLateContinuation", []() {});
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

	TEST(FTaskDiagnosticsTests, LifetimeCountersRemainIsolatedAcrossStoppedAndRestartedSchedulers)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto PreviousLifetime = LaunchTask<int>("PreviousLifetime", []() { return 7; });
		auto ReleasedWhileStopped = LaunchTask<int>("ReleasedWhileStopped", []() { return 8; });
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(PreviousLifetime.GetTaskHandle()).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(ReleasedWhileStopped.GetTaskHandle()).TaskState);
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

	TEST(FTaskCapacityTests, SaturationCoversGraphFormsRollbackDeferredWorkAndCapacityReuse)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 8}));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());

		const FTaskAttribution Attribution = RegisterTaskAttribution("CapacityTests", "Saturation");
		FTaskLaunchOptions RootOptions;
		RootOptions.Attribution = Attribution;
		auto UniqueProducer = LaunchUniqueTask<int>("CapacityUniqueProducer", []() { return 17; }, RootOptions);
		auto FanInProducer = LaunchTask<int>("CapacityFanInProducer", []() { return 4; }, RootOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(UniqueProducer.GetTaskHandle()).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(FanInProducer.GetTaskHandle()).TaskState);

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		auto Blocker = LaunchTask<int>("CapacityBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
			return 3;
		}, RootOptions);
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		const FTaskHandle BlockerTask = Blocker.GetTaskHandle();
		FTaskLaunchOptions WaitingOptions = RootOptions;
		WaitingOptions.Prerequisites = std::span<const FTaskHandle>(&BlockerTask, 1);
		std::vector<FTaskHandle> WaitingTasks;
		for (uint32 Index = 0; Index < 7; ++Index)
		{
			WaitingTasks.emplace_back(LaunchTask("CapacityWaiting", []() {}, WaitingOptions));
			ASSERT_TRUE(WaitingTasks.back().IsValid());
		}

		const FTaskSchedulerDiagnostics Saturated = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(8u, Saturated.TaskReservationCapacity);
		EXPECT_EQ(8u, Saturated.CurrentTaskReservationCount);
		EXPECT_EQ(8u, Saturated.PeakTaskReservationCount);

		auto bRejectedCallableDestroyed = std::make_shared<std::atomic<bool>>(false);
		struct FReentrantRejectedCallable
		{
			std::shared_ptr<std::atomic<bool>> bDestroyed;
			std::unique_ptr<int> Ownership = std::make_unique<int>(1);
			FReentrantRejectedCallable(std::shared_ptr<std::atomic<bool>> InDestroyed)
				: bDestroyed(std::move(InDestroyed)) {}
			FReentrantRejectedCallable(FReentrantRejectedCallable&&) noexcept = default;
			FReentrantRejectedCallable(const FReentrantRejectedCallable&) = delete;
			~FReentrantRejectedCallable()
			{
				if (!Ownership) return;
				EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
				bDestroyed->store(true, std::memory_order::release);
			}
			auto operator()() -> void {}
		};
		EXPECT_FALSE(LaunchTask("CapacityRejectedRoot", FReentrantRejectedCallable(bRejectedCallableDestroyed), RootOptions).IsValid());
		EXPECT_TRUE(bRejectedCallableDestroyed->load(std::memory_order::acquire));

		FTaskContinuationOptions ContinuationOptions;
		ContinuationOptions.Attribution = Attribution;
		EXPECT_FALSE(Then(Blocker, "CapacityRejectedContinuation", [](const int&) {}, ContinuationOptions).IsValid());
		EXPECT_FALSE(WhenAll(std::make_tuple(Blocker, FanInProducer), "CapacityRejectedFanIn",
			[](const int&, const int&) {}, ContinuationOptions).IsValid());
		EXPECT_FALSE(ConsumeThen(std::move(UniqueProducer), "CapacityRejectedUnique",
			[](int&&) {}, ContinuationOptions).IsValid());

		FParallelForOptions ParallelOptions;
		ParallelOptions.MinBatchSize = 1;
		ParallelOptions.Attribution = Attribution;
		EXPECT_EQ(ETaskState::Canceled, ParallelFor("CapacityRejectedParallelFor", 4, [](uint64) {}, ParallelOptions).State);

		FTaskContinuationOptions DeferredOptions = ContinuationOptions;
		DeferredOptions.Target = ETaskTarget::GameThreadDeferred;
		DeferredOptions.EstimatedPayloadBytes = 1;
		EXPECT_FALSE(Then(Blocker, "CapacityRejectedDeferred", [](const int&) {}, DeferredOptions).IsValid());

		ASSERT_TRUE(CancelTask(WaitingTasks.back()));
		WaitingTasks.pop_back();
		EXPECT_EQ(7u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		FTaskHandle UniqueConsumer = ConsumeThen(std::move(UniqueProducer), "CapacityAcceptedUnique",
			[](int&& Value) { EXPECT_EQ(17, Value); }, ContinuationOptions);
		ASSERT_TRUE(UniqueConsumer.IsValid());
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);

		ReleaseBlocker.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(BlockerTask).TaskState);
		for (const FTaskHandle& Task : WaitingTasks) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(UniqueConsumer).TaskState);
		const auto ReservationReleaseDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().CurrentTaskReservationCount != 0
			&& std::chrono::steady_clock::now() < ReservationReleaseDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);

		FTaskHandle Deferred = Then(FanInProducer, "CapacityAcceptedDeferred", [](const int&) {}, DeferredOptions);
		ASSERT_TRUE(Deferred.IsValid());
		while (Deferred.GetState() == ETaskState::Waiting) std::this_thread::yield();
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		EXPECT_EQ(1u, PumpGameThreadDeferredWork({.bUnlimited = true}).ExecutedCallbacks);
		EXPECT_EQ(ETaskState::Succeeded, Deferred.GetState());

		const FTaskSchedulerDiagnostics Final = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(0u, Final.CurrentTaskReservationCount);
		EXPECT_EQ(8u, Final.PeakTaskReservationCount);
		EXPECT_GE(Final.CapacityRejectedTaskCount, 6u);
		auto CapacityCategory = std::ranges::find_if(Final.OwnerCategoryDiagnostics, [](const FTaskOwnerCategoryDiagnostics& Entry) {
			return Entry.Owner == "CapacityTests" && Entry.Category == "Saturation";
		});
		ASSERT_NE(Final.OwnerCategoryDiagnostics.end(), CapacityCategory);
		EXPECT_EQ(Final.CapacityRejectedTaskCount, CapacityCategory->CapacityExhaustedCount);
	}

	TEST(FTaskCapacityTests, ConcurrentProducersNeverOversubscribe)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 8}));
		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = LaunchTask("CapacityConcurrentBlocker", [&]() {
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
				FTaskHandle Task = LaunchTask("CapacityConcurrentProducer", []() {});
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
		FTaskHandle DrainRoot = LaunchTask("CapacityDrainRoot", [&]() {
			DrainStarted.Trigger();
			ReleaseDrain.Wait();
		});
		ASSERT_TRUE(DrainStarted.WaitFor(1.0));
		FTaskLaunchOptions DrainWaitingOptions;
		DrainWaitingOptions.Prerequisites = std::span<const FTaskHandle>(&DrainRoot, 1);
		std::vector<FTaskHandle> DrainTasks;
		for (uint32 Index = 0; Index < 7; ++Index)
		{
			DrainTasks.emplace_back(LaunchTask("CapacityDrainWaiting", []() {}, DrainWaitingOptions));
		}
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		ReleaseDrain.Trigger();
		ShutdownTaskScheduler(true);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().PeakTaskReservationCount);

		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 8}));
		FThreadEvent Started;
		FTaskHandle Running = LaunchCancelableTask("CapacityCancelRunning", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			while (!Token.IsCancellationRequested()) std::this_thread::yield();
		});
		ASSERT_TRUE(Started.WaitFor(1.0));
		FTaskLaunchOptions WaitingOptions;
		WaitingOptions.Prerequisites = std::span<const FTaskHandle>(&Running, 1);
		std::vector<FTaskHandle> Waiting;
		for (uint32 Index = 0; Index < 7; ++Index) Waiting.emplace_back(LaunchTask("CapacityCancelWaiting", []() {}, WaitingOptions));
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		ShutdownTaskScheduler(false);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		EXPECT_EQ(8u, GetTaskSchedulerDiagnostics().PeakTaskReservationCount);
		EXPECT_EQ(ETaskState::Canceled, Running.GetState());
		for (const FTaskHandle& Task : Waiting) EXPECT_EQ(ETaskState::Canceled, Task.GetState());
	}

	TEST(FTaskAttributionTests, AggregatesLifecycleRejectionsResultsAndConcurrentSnapshots)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		const FTaskAttribution MainAttribution = RegisterTaskAttribution("AggregateOwner", "Main");
		const FTaskAttribution OtherAttribution = RegisterTaskAttribution("AggregateOwner", "Other");
		auto Find = [](const FTaskSchedulerDiagnostics& Diagnostics, std::string_view Category) -> const FTaskOwnerCategoryDiagnostics& {
			const auto Iterator = std::ranges::find_if(Diagnostics.OwnerCategoryDiagnostics, [Category](const FTaskOwnerCategoryDiagnostics& Entry) {
				return Entry.Owner == "AggregateOwner" && Entry.Category == Category;
			});
			check(Iterator != Diagnostics.OwnerCategoryDiagnostics.end());
			return *Iterator;
		};
		auto Sum = [](const FTaskSchedulerDiagnostics& Diagnostics, auto Member) {
			uint64 Result = 0;
			for (const FTaskOwnerCategoryDiagnostics& Entry : Diagnostics.OwnerCategoryDiagnostics) Result += Entry.*Member;
			return Result;
		};
		auto HistogramSamples = [](const std::array<uint64, 32>& Histogram) {
			uint64 Result = 0;
			for (uint64 Bucket : Histogram) Result += Bucket;
			return Result;
		};

		FTaskLaunchOptions MainOptions;
		MainOptions.Attribution = MainAttribution;
		FTaskLaunchOptions OtherOptions;
		OtherOptions.Attribution = OtherAttribution;
		FThreadEvent RunningStarted;
		FThreadEvent ReleaseRunning;
		FTaskHandle Running = LaunchTask("AggregateRunning", [&]() {
			RunningStarted.Trigger();
			ReleaseRunning.Wait();
		}, MainOptions);
		ASSERT_TRUE(RunningStarted.WaitFor(1.0));
		std::array<FTaskHandle, 1> RunningPrerequisite{Running};
		FTaskLaunchOptions WaitingOptions = MainOptions;
		WaitingOptions.Prerequisites = RunningPrerequisite;
		FTaskHandle Waiting = LaunchTask("AggregateWaiting", []() {}, WaitingOptions);

		const FTaskSchedulerDiagnostics Live = GetTaskSchedulerDiagnostics();
		const FTaskOwnerCategoryDiagnostics& LiveMain = Find(Live, "Main");
		EXPECT_EQ(2u, LiveMain.AcceptedCount);
		EXPECT_EQ(1u, LiveMain.CurrentWaitingCount);
		EXPECT_EQ(1u, LiveMain.CurrentRunningCount);
		EXPECT_EQ(2u, LiveMain.CurrentNonterminalCount);
		EXPECT_GT(LiveMain.CurrentCallableBytes, 0u);
		EXPECT_EQ(2u, HistogramSamples(LiveMain.CallableBytesHistogram));
		EXPECT_EQ(2u, HistogramSamples(LiveMain.PayloadBytesHistogram));
		EXPECT_EQ(2u, HistogramSamples(LiveMain.ResultBytesHistogram));

		std::atomic<bool> bKeepSnapshotting = true;
		std::thread SnapshotThread([&]() {
			while (bKeepSnapshotting.load(std::memory_order::acquire))
			{
				const FTaskSchedulerDiagnostics Snapshot = GetTaskSchedulerDiagnostics();
				EXPECT_LE(Snapshot.OwnerCategoryDiagnostics.size(), 1'024u);
				EXPECT_EQ(Snapshot.OwnerCategoryDiagnostics.size(), std::ranges::count_if(
					Snapshot.OwnerCategoryDiagnostics,
					[](const FTaskOwnerCategoryDiagnostics& Entry) { return !Entry.Owner.empty() && !Entry.Category.empty(); }));
			}
		});

		std::vector<std::thread> Cancelers;
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			Cancelers.emplace_back([&]() { CancelTask(Running); });
		}
		for (std::thread& Canceler : Cancelers) Canceler.join();
		ReleaseRunning.Trigger();
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Running).TaskState);
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Waiting).TaskState);

		FTaskHandle Failed = LaunchTask("AggregateFailed", []() { throw std::runtime_error("aggregate failure"); }, OtherOptions);
		EXPECT_EQ(ETaskState::Failed, WaitTask(Failed).TaskState);
		FTaskHandle Succeeded = LaunchTask("AggregateSucceeded", []() {}, MainOptions);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Succeeded).TaskState);
		FTaskLaunchOptions InvalidOptions = MainOptions;
		std::array<FTaskHandle, 1> InvalidPrerequisite{};
		InvalidOptions.Prerequisites = InvalidPrerequisite;
		EXPECT_FALSE(LaunchTask("AggregateRejected", []() {}, InvalidOptions).IsValid());

		auto Producer = LaunchUniqueTask<Durin::FByteArray>(
			"AggregateUniqueProducer",
			[]() { return Durin::FByteArray(128, std::byte{7}); },
			MainOptions,
			128);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Producer.GetTaskHandle()).TaskState);
		const FTaskSchedulerDiagnostics Retained = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(128u, Retained.RetainedUniqueResultBytes);
		EXPECT_EQ(128u, Find(Retained, "Main").CurrentRetainedUniqueResultBytes);
		FTaskHandle Consumer = ConsumeThen(std::move(Producer), "AggregateUniqueConsumer", [](Durin::FByteArray&& Value) {
			EXPECT_EQ(128u, Value.size());
		});
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Consumer).TaskState);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);

		ASSERT_TRUE(InitializeGameThreadDeferredExecutor({
			.MaxQueuedEntries = 1,
			.MaxQueuedPayloadBytes = 8,
			.MaxPayloadBytesPerEntry = 8,
		}));
		FTaskHandle Root = LaunchTask("AggregateDeferredRoot", []() {}, MainOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		FTaskContinuationOptions DeferredOptions;
		DeferredOptions.Target = ETaskTarget::GameThreadDeferred;
		DeferredOptions.EstimatedPayloadBytes = 4;
		DeferredOptions.Attribution = MainAttribution;
		DeferredOptions.CoalescingKey = FTaskCoalescingKey{7, 8, 9};
		FTaskHandle Superseded = Then(Root, "AggregateSuperseded", []() {}, DeferredOptions);
		FTaskHandle Replacement = Then(Root, "AggregateReplacement", []() {}, DeferredOptions);
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Superseded).TaskState);
		FTaskContinuationOptions SaturatedOptions = DeferredOptions;
		SaturatedOptions.CoalescingKey.reset();
		FTaskHandle Saturated = Then(Root, "AggregateSaturated", []() {}, SaturatedOptions);
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Saturated).TaskState);
		EXPECT_EQ(ETaskTerminalReason::DispatchRejected, Saturated.GetDiagnostics().TerminalReason);
		PumpGameThreadDeferredWork({.bUnlimited = true});
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Replacement).TaskState);

		FTaskGenerationSource Generation;
		FTaskContinuationOptions StaleOptions = DeferredOptions;
		StaleOptions.CoalescingKey.reset();
		StaleOptions.GenerationToken = Generation.Capture();
		FTaskHandle Stale = Then(Root, "AggregateStale", []() {}, StaleOptions);
		Generation.Advance();
		PumpGameThreadDeferredWork({.bUnlimited = true});
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Stale).TaskState);

		FParallelForOptions ParallelOptions;
		ParallelOptions.Attribution = OtherAttribution;
		ParallelOptions.MinBatchSize = 1;
		EXPECT_EQ(ETaskState::Succeeded, ParallelFor("AggregateParallel", 4, [](uint64) {}, ParallelOptions).State);
		bKeepSnapshotting.store(false, std::memory_order::release);
		SnapshotThread.join();

		const FTaskSchedulerDiagnostics Final = GetTaskSchedulerDiagnostics();
		const FTaskOwnerCategoryDiagnostics& FinalMain = Find(Final, "Main");
		const FTaskOwnerCategoryDiagnostics& FinalOther = Find(Final, "Other");
		EXPECT_EQ(0u, Sum(Final, &FTaskOwnerCategoryDiagnostics::CurrentNonterminalCount));
		EXPECT_EQ(0u, Sum(Final, &FTaskOwnerCategoryDiagnostics::CurrentCallableBytes));
		EXPECT_EQ(0u, Sum(Final, &FTaskOwnerCategoryDiagnostics::CurrentPayloadBytes));
		EXPECT_EQ(0u, Sum(Final, &FTaskOwnerCategoryDiagnostics::CurrentResultBytes));
		EXPECT_EQ(Final.CompletedTaskCount, Sum(Final, &FTaskOwnerCategoryDiagnostics::SucceededCount)
			+ Sum(Final, &FTaskOwnerCategoryDiagnostics::FailedCount)
			+ Sum(Final, &FTaskOwnerCategoryDiagnostics::CanceledCount));
		EXPECT_EQ(Final.RejectedTaskCount, Sum(Final, &FTaskOwnerCategoryDiagnostics::RejectedCount));
		EXPECT_EQ(1u, FinalMain.SupersededCount);
		EXPECT_EQ(1u, FinalMain.StaleGenerationCount);
		EXPECT_GE(FinalMain.DispatchRejectedCount, 1u);
		EXPECT_EQ(1u, FinalOther.CallbackFailureCount);
		EXPECT_EQ(1u, FinalOther.ParallelForOperationCount);

		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		ASSERT_TRUE(InitializeTaskScheduler(1));
		const FTaskSchedulerDiagnostics Restarted = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(0u, Find(Restarted, "Main").AcceptedCount);
		EXPECT_EQ(0u, Find(Restarted, "Other").ParallelForOperationCount);
		ShutdownTaskScheduler(true);
	}

	TEST(FTaskAttributionTests, RegistersBoundedIdentityAndPropagatesAcrossTaskForms)
	{
		ShutdownTaskScheduler(false);
		Private::ResetTaskAttributionRegistryForTests();
		struct FAttributionRegistryResetGuard
		{
			~FAttributionRegistryResetGuard()
			{
				ShutdownTaskScheduler(false);
				Private::ResetTaskAttributionRegistryForTests();
			}
		} AttributionRegistryResetGuard;
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		const FTaskAttribution Unattributed;
		const FTaskAttribution OwnerA = RegisterTaskAttribution("AttributionOwnerA", "Prepare");
		const FTaskAttribution OwnerAAgain = RegisterTaskAttribution("AttributionOwnerA", "Prepare");
		const FTaskAttribution OwnerB = RegisterTaskAttribution("AttributionOwnerB", "Publish");
		EXPECT_EQ(OwnerA, OwnerAAgain);
		EXPECT_NE(Unattributed, OwnerA);
		EXPECT_NE(OwnerA, OwnerB);

		std::array<FTaskAttribution, 16> ConcurrentTokens{};
		std::vector<std::thread> RegistrationThreads;
		for (size_t Index = 0; Index < ConcurrentTokens.size(); ++Index)
		{
			RegistrationThreads.emplace_back([&, Index]() {
				ConcurrentTokens[Index] = RegisterTaskAttribution("ConcurrentOwner", "Duplicate");
			});
		}
		for (std::thread& Thread : RegistrationThreads) Thread.join();
		for (const FTaskAttribution Token : ConcurrentTokens) EXPECT_EQ(ConcurrentTokens.front(), Token);

		FTaskLaunchOptions OwnerAOptions;
		OwnerAOptions.Attribution = OwnerA;
		FTaskLaunchOptions OwnerBOptions;
		OwnerBOptions.Attribution = OwnerB;
		FTaskHandle Child;
		FTaskHandle RootA = LaunchTask("AttributedRootA", [&]() {
			Child = LaunchTask("AttributedChild", []() {});
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Child).TaskState);
		}, OwnerAOptions);
		FTaskHandle RootB = LaunchTask("AttributedRootB", []() {}, OwnerBOptions);
		FTaskHandle DefaultRoot = LaunchTask("UnattributedRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(RootA).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(RootB).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(DefaultRoot).TaskState);
		ASSERT_TRUE(Child.IsValid());

		std::array<FTaskHandle, 1> CrossOwnerPrerequisites{RootB};
		FTaskContinuationOptions CrossOwnerOptions;
		CrossOwnerOptions.Prerequisites = CrossOwnerPrerequisites;
		FTaskHandle InheritedContinuation = Then(RootA, "InheritedContinuation", []() {}, CrossOwnerOptions);
		FTaskContinuationOptions OverrideOptions;
		OverrideOptions.Attribution = OwnerB;
		FTaskHandle OverriddenContinuation = Then(RootA, "OverriddenContinuation", []() {}, OverrideOptions);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(InheritedContinuation).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(OverriddenContinuation).TaskState);

		auto UniqueProducer = LaunchUniqueTask<int>("AttributedUniqueProducer", []() { return 7; }, OwnerAOptions);
		FTaskHandle UniqueConsumer = ConsumeThen(std::move(UniqueProducer), "AttributedUniqueConsumer", [](int&& Value) {
			EXPECT_EQ(7, Value);
		});
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(UniqueConsumer).TaskState);

		auto ExpectAttribution = [](const FTaskDiagnostics& Diagnostics, std::string_view Owner, std::string_view Category) {
			EXPECT_EQ(Owner, Diagnostics.AttributionOwner);
			EXPECT_EQ(Category, Diagnostics.AttributionCategory);
			EXPECT_GT(Diagnostics.AttributionCategoryId, 1u);
		};
		ExpectAttribution(RootA.GetDiagnostics(), "AttributionOwnerA", "Prepare");
		ExpectAttribution(Child.GetDiagnostics(), "AttributionOwnerA", "Prepare");
		ExpectAttribution(InheritedContinuation.GetDiagnostics(), "AttributionOwnerA", "Prepare");
		ExpectAttribution(OverriddenContinuation.GetDiagnostics(), "AttributionOwnerB", "Publish");
		ExpectAttribution(UniqueConsumer.GetDiagnostics(), "AttributionOwnerA", "Prepare");
		EXPECT_EQ(0u, DefaultRoot.GetDiagnostics().AttributionOwnerId);
		EXPECT_EQ(0u, DefaultRoot.GetDiagnostics().AttributionCategoryId);
		EXPECT_EQ("Unattributed", DefaultRoot.GetDiagnostics().AttributionOwner);
		EXPECT_EQ("Unattributed", DefaultRoot.GetDiagnostics().AttributionCategory);

		FThreadEvent RunningStarted;
		FThreadEvent ReleaseRunning;
		struct FLargeAttributedCallable
		{
			FThreadEvent* Started = nullptr;
			FThreadEvent* Release = nullptr;
			std::array<std::byte, 128> Storage{};
			auto operator()() const -> void { Started->Trigger(); Release->Wait(); }
		};
		FTaskHandle Running = LaunchTask("AttributedRunning", FLargeAttributedCallable{&RunningStarted, &ReleaseRunning}, OwnerAOptions);
		ASSERT_TRUE(RunningStarted.WaitFor(1.0));
		const FTaskDiagnostics RunningDiagnostics = Running.GetDiagnostics();
		EXPECT_EQ(sizeof(FLargeAttributedCallable), RunningDiagnostics.CallableStorageBytes);
		EXPECT_GT(RunningDiagnostics.ExecutionNanoseconds, 0u);
		ReleaseRunning.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Running).TaskState);
		const FTaskDiagnostics FinishedDiagnostics = Running.GetDiagnostics();
		EXPECT_GE(FinishedDiagnostics.ExecutionNanoseconds, RunningDiagnostics.ExecutionNanoseconds);
		EXPECT_EQ(FinishedDiagnostics.FinishTimeNanoseconds - FinishedDiagnostics.StartTimeNanoseconds, FinishedDiagnostics.ExecutionNanoseconds);
		FTaskHandle Failed = LaunchTask("AttributedFailure", []() { throw std::runtime_error("attributed failure"); }, OwnerBOptions);
		ASSERT_EQ(ETaskState::Failed, WaitTask(Failed).TaskState);
		EXPECT_GT(Failed.GetDiagnostics().CallableStorageBytes, 0u);
		EXPECT_GT(Failed.GetDiagnostics().ExecutionNanoseconds, 0u);
		ExpectAttribution(Failed.GetDiagnostics(), "AttributionOwnerB", "Publish");
		FTaskCancellationSource PreCanceledSource;
		PreCanceledSource.RequestCancellation();
		FTaskLaunchOptions PreCanceledOptions = OwnerAOptions;
		PreCanceledOptions.CancellationToken = PreCanceledSource.GetToken();
		FTaskHandle PreCanceled = LaunchTask("AttributedPreCanceled", [Storage = std::array<std::byte, 128>{}]() {
			(void)Storage;
		}, PreCanceledOptions);
		ASSERT_EQ(ETaskState::Canceled, WaitTask(PreCanceled).TaskState);
		EXPECT_GT(PreCanceled.GetDiagnostics().CallableStorageBytes, 0u);
		EXPECT_EQ(0u, PreCanceled.GetDiagnostics().ExecutionNanoseconds);
		ExpectAttribution(PreCanceled.GetDiagnostics(), "AttributionOwnerA", "Prepare");

		FParallelForOptions ParallelOptions;
		ParallelOptions.MinBatchSize = 1;
		ParallelOptions.Attribution = OwnerB;
		FThreadEvent ParallelEntered;
		FThreadEvent ReleaseParallel;
		std::thread ParallelCaller([&]() {
			const FParallelForResult Result = ParallelFor("AttributedParallelFor", 2, [&](uint64) {
				ParallelEntered.Trigger();
				ReleaseParallel.Wait();
			}, ParallelOptions);
			EXPECT_EQ(ETaskState::Succeeded, Result.State);
		});
		ASSERT_TRUE(ParallelEntered.WaitFor(1.0));
		bool bObservedAttributedChunk = false;
		for (const FTaskDiagnostics& Diagnostics : GetTaskSchedulerDiagnostics().NonterminalTasks)
		{
			if (Diagnostics.DebugName == "AttributedParallelFor")
			{
				bObservedAttributedChunk = Diagnostics.AttributionOwner == "AttributionOwnerB"
					&& Diagnostics.AttributionCategory == "Publish";
			}
		}
		EXPECT_TRUE(bObservedAttributedChunk);
		ReleaseParallel.Trigger();
		ParallelCaller.join();

		const FTaskAttribution Overflow = RegisterTaskAttribution("", "Invalid");
		EXPECT_EQ(Overflow, RegisterTaskAttribution(std::string(64, 'x'), "Invalid"));
		EXPECT_EQ(Overflow, RegisterTaskAttribution(std::string("Bad\0Owner", 9), "Invalid"));
		EXPECT_EQ(Overflow, RegisterTaskAttribution(std::string("\xc0\x80", 2), "Invalid"));
		const size_t SlotsBeforeCapacityFill = GetTaskSchedulerDiagnostics().OwnerCategoryDiagnostics.size();
		for (size_t Index = SlotsBeforeCapacityFill; Index < 1'024; ++Index)
		{
			const FTaskAttribution Token = RegisterTaskAttribution("CapacityOwner", "Category" + std::to_string(Index));
			EXPECT_NE(Overflow, Token);
		}
		EXPECT_EQ(Overflow, RegisterTaskAttribution("CapacityOwner", "CapacityOverflow"));
		EXPECT_EQ(1'024u, GetTaskSchedulerDiagnostics().OwnerCategoryDiagnostics.size());

		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_EQ(1'024u, GetTaskSchedulerDiagnostics().OwnerCategoryDiagnostics.size());
		FTaskHandle Restarted = LaunchTask("AttributedAfterRestart", []() {}, OwnerAOptions);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Restarted).TaskState);
		ExpectAttribution(Restarted.GetDiagnostics(), "AttributionOwnerA", "Prepare");
		ShutdownTaskScheduler(true);
		const FTaskSchedulerDiagnostics StoppedDiagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_FALSE(StoppedDiagnostics.bRunning);
		EXPECT_EQ(1'024u, StoppedDiagnostics.OwnerCategoryDiagnostics.size());
		EXPECT_GE(StoppedDiagnostics.AttributionRegistrationOverflowCount, 5u);
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

	TEST(FTaskMoveOnlyCallableTests, MoveOnlyCapturesRunAcrossLaunchAndContinuationForms)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());

		std::atomic<uint32> Observation = 0;
		FTaskHandle VoidTask = LaunchTask("MoveOnlyVoid", [Value = std::make_unique<uint32>(1), &Observation]() {
			Observation.fetch_add(*Value, std::memory_order::acq_rel);
		});
		FTaskHandle CancelableTask = LaunchCancelableTask("MoveOnlyCancelable",
			[Value = std::make_unique<uint32>(2), &Observation](const FTaskCancellationToken& Token) {
				EXPECT_FALSE(Token.IsCancellationRequested());
				Observation.fetch_add(*Value, std::memory_order::acq_rel);
			});
		auto TypedTask = LaunchTask<uint32>("MoveOnlyTyped",
			[Value = std::make_unique<uint32>(4)]() { return *Value; });
		auto TypedContinuation = Then(TypedTask, "MoveOnlyTypedContinuation",
			[Value = std::make_unique<uint32>(8), &Observation](const uint32& Predecessor) {
				Observation.fetch_add(*Value + Predecessor, std::memory_order::acq_rel);
				return Predecessor * 2;
			});
		FTaskHandle OutcomeContinuation = ThenOutcome(VoidTask, "MoveOnlyOutcomeContinuation",
			[Value = std::make_unique<uint32>(16), &Observation](FTaskOutcome<void> Outcome) {
				EXPECT_EQ(ETaskState::Succeeded, Outcome.State);
				Observation.fetch_add(*Value, std::memory_order::acq_rel);
			});

		FTaskContinuationOptions DeferredOptions;
		DeferredOptions.Target = ETaskTarget::GameThreadDeferred;
		DeferredOptions.EstimatedPayloadBytes = 64;
		FTaskHandle Deferred = Then(TypedContinuation, "MoveOnlyDeferredContinuation",
			[Value = std::make_unique<uint32>(32), &Observation](const uint32& Result) {
				EXPECT_TRUE(IsInGameThread());
				Observation.fetch_add(*Value + Result, std::memory_order::acq_rel);
			}, DeferredOptions);

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(CancelableTask).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(OutcomeContinuation).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(TypedContinuation.GetTaskHandle()).TaskState);
		const auto DispatchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth == 0
			&& std::chrono::steady_clock::now() < DispatchDeadline)
		{
			std::this_thread::yield();
		}
		ASSERT_EQ(ETaskState::Queued, Deferred.GetState());
		ASSERT_EQ(1u, GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth);
		EXPECT_EQ(1u, PumpGameThreadDeferredWork().ExecutedCallbacks);
		EXPECT_EQ(ETaskState::Succeeded, Deferred.GetState());
		EXPECT_EQ(71u, Observation.load(std::memory_order::acquire));
	}

	TEST(FTaskMoveOnlyCallableTests, CancellationDestroysCaptureOutsideTaskStateLock)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = LaunchTask("MoveOnlyDestructionBlocker", [&]() {
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
		*CanceledHandle = LaunchTask("MoveOnlyReentrantDestruction",
			FReentrantDestruction(CanceledHandle, DestructionCount), WaitingOptions);
		ASSERT_TRUE(CanceledHandle->IsValid());
		ASSERT_TRUE(CancelTask(*CanceledHandle));
		EXPECT_EQ(1u, DestructionCount->load(std::memory_order::acquire));

		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
	}

	TEST(FUniqueTaskTests, WorkerSinkConsumesOneMoveOnlyValueAndClearsRetainedBytes)
	{
		struct FObservedValue
		{
			std::shared_ptr<std::atomic<uint32>> MoveCount;
			std::shared_ptr<std::atomic<uint32>> DestructionCount;
			std::unique_ptr<int> Value;

			FObservedValue(std::shared_ptr<std::atomic<uint32>> InMoveCount,
				std::shared_ptr<std::atomic<uint32>> InDestructionCount, int InValue)
				: MoveCount(std::move(InMoveCount)), DestructionCount(std::move(InDestructionCount)), Value(std::make_unique<int>(InValue))
			{
			}
			FObservedValue(FObservedValue&& Other) noexcept
				: MoveCount(std::move(Other.MoveCount)), DestructionCount(std::move(Other.DestructionCount)), Value(std::move(Other.Value))
			{
				MoveCount->fetch_add(1, std::memory_order::acq_rel);
			}
			FObservedValue(const FObservedValue&) = delete;
			~FObservedValue()
			{
				if (Value) DestructionCount->fetch_add(1, std::memory_order::acq_rel);
			}
		};

		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto MoveCount = std::make_shared<std::atomic<uint32>>(0);
		auto DestructionCount = std::make_shared<std::atomic<uint32>>(0);
		auto Producer = LaunchUniqueTask<FObservedValue>("UniqueWorkerProducer",
			[MoveCount, DestructionCount]() { return FObservedValue(MoveCount, DestructionCount, 42); }, {}, 128);
		ASSERT_TRUE(Producer.IsValid());
		const FTaskHandle ProducerTask = Producer.GetTaskHandle();
		std::atomic<uint32> Observed = 0;
		FTaskHandle Consumer = ConsumeThen(std::move(Producer), "UniqueWorkerConsumer",
			[&Observed](FObservedValue&& Result) { Observed.store(static_cast<uint32>(*Result.Value), std::memory_order::release); });
		EXPECT_FALSE(Producer.IsValid());
		ASSERT_TRUE(Consumer.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Consumer).TaskState);
		EXPECT_EQ(42u, Observed.load(std::memory_order::acquire));
		EXPECT_EQ(1u, MoveCount->load(std::memory_order::acquire));
		EXPECT_EQ(1u, DestructionCount->load(std::memory_order::acquire));
		EXPECT_EQ(128u, ProducerTask.GetDiagnostics().EstimatedResultBytes);
		EXPECT_EQ(0u, Consumer.GetDiagnostics().RetainedResultBytes);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);
	}

	TEST(FUniqueTaskTests, OutcomeSinkReceivesFailureWithoutAValue)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto Producer = LaunchUniqueTask<FCompileMoveOnlyValue>("UniqueFailedProducer", []() -> FCompileMoveOnlyValue {
			throw std::runtime_error("unique producer failed");
		});
		std::atomic<bool> bObserved = false;
		FTaskHandle Consumer = ConsumeThenOutcome(std::move(Producer), "UniqueFailureOutcome",
			[&bObserved](FUniqueTaskOutcome<FCompileMoveOnlyValue>&& Outcome) {
				EXPECT_EQ(ETaskState::Failed, Outcome.State);
				EXPECT_EQ(ETaskTerminalReason::CallbackFailure, Outcome.Reason);
				EXPECT_EQ("unique producer failed", Outcome.Diagnostic);
				EXPECT_FALSE(Outcome.Result.has_value());
				bObserved.store(true, std::memory_order::release);
			});
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Consumer).TaskState);
		EXPECT_TRUE(bObserved.load(std::memory_order::acquire));
	}

	TEST(FUniqueTaskTests, RejectedRegistrationPreservesHandleAndDuplicateClaimIsCounted)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FTaskHandle ForeignPrerequisite = LaunchTask("UniqueForeignPrerequisite", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(ForeignPrerequisite).TaskState);
		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent ReleaseProducer;
		auto Producer = LaunchUniqueTask<FCompileMoveOnlyValue>("UniqueTransactionalProducer", [&]() {
			ReleaseProducer.Wait();
			return FCompileMoveOnlyValue(9);
		});
		FTaskContinuationOptions InvalidOptions;
		InvalidOptions.Prerequisites = std::span<const FTaskHandle>(&ForeignPrerequisite, 1);
		FTaskHandle Rejected = ConsumeThen(std::move(Producer), "UniqueRejectedConsumer",
			[](FCompileMoveOnlyValue&&) {}, InvalidOptions);
		EXPECT_FALSE(Rejected.IsValid());
		EXPECT_TRUE(Producer.IsValid());

		FTaskHandle Consumer = ConsumeThen(std::move(Producer), "UniqueAcceptedConsumer",
			[](FCompileMoveOnlyValue&& Value) { EXPECT_EQ(9, *Value.Value); });
		ASSERT_TRUE(Consumer.IsValid());
		EXPECT_FALSE(Producer.IsValid());
		FTaskHandle Duplicate = ConsumeThen(std::move(Producer), "UniqueDuplicateConsumer",
			[](FCompileMoveOnlyValue&&) {});
		EXPECT_FALSE(Duplicate.IsValid());
		EXPECT_EQ(1u, GetTaskSchedulerDiagnostics().DuplicateUniqueConsumerClaimCount);

		ReleaseProducer.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Consumer).TaskState);
	}

	TEST(FUniqueTaskTests, GameThreadSinkChargesRetainedBytesAndOverflowPreservesHandle)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.MaxQueuedPayloadBytes = 256;
		Config.MaxPayloadBytesPerEntry = 256;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));

		auto Producer = LaunchUniqueTask<FCompileMoveOnlyValue>("UniqueDeferredProducer",
			[]() { return FCompileMoveOnlyValue(17); }, {}, 96);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Producer.GetTaskHandle()).TaskState);
		EXPECT_EQ(96u, Producer.GetDiagnostics().RetainedResultBytes);

		FTaskContinuationOptions OverflowOptions;
		OverflowOptions.Target = ETaskTarget::GameThreadDeferred;
		OverflowOptions.EstimatedPayloadBytes = std::numeric_limits<uint64>::max();
		EXPECT_FALSE(ConsumeThen(std::move(Producer), "UniqueDeferredOverflow",
			[](FCompileMoveOnlyValue&&) {}, OverflowOptions).IsValid());
		EXPECT_TRUE(Producer.IsValid());

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 32;
		std::atomic<bool> bRan = false;
		FTaskHandle Consumer = ConsumeThen(std::move(Producer), "UniqueDeferredConsumer",
			[&bRan](FCompileMoveOnlyValue&& Value) {
				EXPECT_TRUE(IsInGameThread());
				EXPECT_EQ(17, *Value.Value);
				bRan.store(true, std::memory_order::release);
			}, Options);
		while (Consumer.GetState() == ETaskState::Waiting) std::this_thread::yield();
		ASSERT_EQ(ETaskState::Queued, Consumer.GetState());
		EXPECT_EQ(128u, Consumer.GetDiagnostics().EstimatedPayloadBytes);
		EXPECT_EQ(128u, GetGameThreadDeferredWorkQueueDiagnostics().QueuedPayloadBytes);
		EXPECT_FALSE(bRan.load(std::memory_order::acquire));
		EXPECT_EQ(1u, PumpGameThreadDeferredWork().ExecutedCallbacks);
		EXPECT_TRUE(bRan.load(std::memory_order::acquire));
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);
	}

	TEST(FUniqueTaskTests, DroppedUnconsumedHandleDestroysPublishedValue)
	{
		struct FDestructionValue
		{
			std::shared_ptr<std::atomic<uint32>> Count;
			std::unique_ptr<int> Value = std::make_unique<int>(1);
			explicit FDestructionValue(std::shared_ptr<std::atomic<uint32>> InCount) : Count(std::move(InCount)) {}
			FDestructionValue(FDestructionValue&&) noexcept = default;
			FDestructionValue(const FDestructionValue&) = delete;
			~FDestructionValue() { if (Value) Count->fetch_add(1, std::memory_order::acq_rel); }
		};

		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Count = std::make_shared<std::atomic<uint32>>(0);
		auto Producer = LaunchUniqueTask<FDestructionValue>("UniqueDroppedProducer",
			[Count]() { return FDestructionValue(Count); }, {}, 64);
		FTaskHandle Erased = Producer.GetTaskHandle();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Erased).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(LaunchTask("UniqueDroppedFence", []() {})).TaskState);
		EXPECT_EQ(64u, Erased.GetDiagnostics().RetainedResultBytes);
		Producer = {};
		EXPECT_EQ(1u, Count->load(std::memory_order::acquire));
		EXPECT_TRUE(Erased.IsValid());
		EXPECT_EQ(0u, Erased.GetDiagnostics().RetainedResultBytes);
	}

	TEST(FUniqueTaskTests, CancellationBeforeInvocationDiscardsPublishedValue)
	{
		struct FDestructionValue
		{
			std::shared_ptr<std::atomic<uint32>> Count;
			std::unique_ptr<int> Value = std::make_unique<int>(1);
			explicit FDestructionValue(std::shared_ptr<std::atomic<uint32>> InCount) : Count(std::move(InCount)) {}
			FDestructionValue(FDestructionValue&&) noexcept = default;
			FDestructionValue(const FDestructionValue&) = delete;
			~FDestructionValue() { if (Value) Count->fetch_add(1, std::memory_order::acq_rel); }
		};

		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Count = std::make_shared<std::atomic<uint32>>(0);
		auto Producer = LaunchUniqueTask<FDestructionValue>("UniqueCanceledProducer",
			[Count]() { return FDestructionValue(Count); }, {}, 64);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Producer.GetTaskHandle()).TaskState);

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = LaunchTask("UniqueCanceledBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		std::atomic<bool> bRan = false;
		FTaskHandle Consumer = ConsumeThen(std::move(Producer), "UniqueCanceledConsumer",
			[&bRan](FDestructionValue&&) { bRan.store(true, std::memory_order::release); });
		ASSERT_TRUE(CancelTask(Consumer));
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Consumer).TaskState);
		EXPECT_FALSE(bRan.load(std::memory_order::acquire));
		EXPECT_EQ(1u, Count->load(std::memory_order::acquire));
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);
		ReleaseBlocker.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
	}

	TEST(FUniqueTaskTests, CallbackFailureDestroysConsumedValueAndFailsSink)
	{
		struct FDestructionValue
		{
			std::shared_ptr<std::atomic<uint32>> Count;
			std::unique_ptr<int> Value = std::make_unique<int>(1);
			explicit FDestructionValue(std::shared_ptr<std::atomic<uint32>> InCount) : Count(std::move(InCount)) {}
			FDestructionValue(FDestructionValue&&) noexcept = default;
			FDestructionValue(const FDestructionValue&) = delete;
			~FDestructionValue() { if (Value) Count->fetch_add(1, std::memory_order::acq_rel); }
		};

		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Count = std::make_shared<std::atomic<uint32>>(0);
		auto Producer = LaunchUniqueTask<FDestructionValue>("UniqueFailingSinkProducer",
			[Count]() { return FDestructionValue(Count); });
		FTaskHandle Consumer = ConsumeThen(std::move(Producer), "UniqueFailingSink",
			[](FDestructionValue&&) { throw std::runtime_error("unique sink failed"); });
		EXPECT_EQ(ETaskState::Failed, WaitTask(Consumer).TaskState);
		EXPECT_EQ(ETaskTerminalReason::CallbackFailure, Consumer.GetDiagnostics().TerminalReason);
		EXPECT_EQ("unique sink failed", Consumer.GetDiagnostic());
		EXPECT_EQ(1u, Count->load(std::memory_order::acquire));
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);
	}

	TEST(FUniqueTaskTests, DrainAndCancelShutdownLeaveUniqueGraphTerminalAndEmpty)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());

		std::atomic<bool> bDrainedSinkRan = false;
		auto DrainProducer = LaunchUniqueTask<FCompileMoveOnlyValue>("UniqueDrainProducer",
			[]() { return FCompileMoveOnlyValue(3); });
		FTaskContinuationOptions DeferredOptions;
		DeferredOptions.Target = ETaskTarget::GameThreadDeferred;
		DeferredOptions.EstimatedPayloadBytes = 16;
		FTaskHandle DrainConsumer = ConsumeThen(std::move(DrainProducer), "UniqueDrainConsumer",
			[&bDrainedSinkRan](FCompileMoveOnlyValue&& Value) {
				EXPECT_EQ(3, *Value.Value);
				bDrainedSinkRan.store(true, std::memory_order::release);
			}, DeferredOptions);
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		EXPECT_TRUE(bDrainedSinkRan.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskState::Succeeded, DrainConsumer.GetState());
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);

		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent ProducerStarted;
		auto Count = std::make_shared<std::atomic<uint32>>(0);
		struct FCancelValue
		{
			std::shared_ptr<std::atomic<uint32>> Count;
			std::unique_ptr<int> Value = std::make_unique<int>(1);
			explicit FCancelValue(std::shared_ptr<std::atomic<uint32>> InCount) : Count(std::move(InCount)) {}
			FCancelValue(FCancelValue&&) noexcept = default;
			FCancelValue(const FCancelValue&) = delete;
			~FCancelValue() { if (Value) Count->fetch_add(1, std::memory_order::acq_rel); }
		};
		auto CancelProducer = LaunchUniqueCancelableTask<FCancelValue>("UniqueCancelShutdownProducer",
			[&ProducerStarted, Count](const FTaskCancellationToken& Token) {
				ProducerStarted.Trigger();
				while (!Token.IsCancellationRequested()) std::this_thread::yield();
				return FCancelValue(Count);
			});
		ASSERT_TRUE(ProducerStarted.WaitFor(1.0));
		ShutdownTaskScheduler(false);
		EXPECT_EQ(ETaskState::Canceled, CancelProducer.GetState());
		EXPECT_EQ(1u, Count->load(std::memory_order::acquire));
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);
	}

	TEST(FUniqueTaskTests, DeferredRetainedBytesSaturateWithoutHiddenStorage)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.MaxQueuedEntries = 8;
		Config.MaxQueuedPayloadBytes = 256;
		Config.MaxPayloadBytesPerEntry = 128;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));

		std::vector<TUniqueTaskHandle<FCompileMoveOnlyValue>> Producers;
		for (int Value = 0; Value < 5; ++Value)
		{
			Producers.emplace_back(LaunchUniqueTask<FCompileMoveOnlyValue>(
				"UniqueSaturationProducer", [Value]() { return FCompileMoveOnlyValue(Value); }, {}, 64));
		}
		for (const auto& Producer : Producers)
			ASSERT_EQ(ETaskState::Succeeded, WaitTask(Producer.GetTaskHandle()).TaskState);

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		std::atomic<uint32> Runs = 0;
		std::vector<FTaskHandle> Consumers;
		for (auto& Producer : Producers)
		{
			Consumers.emplace_back(ConsumeThen(std::move(Producer), "UniqueSaturationConsumer",
				[&Runs](FCompileMoveOnlyValue&&) { Runs.fetch_add(1, std::memory_order::acq_rel); }, Options));
		}
		for (size_t Index = 0; Index < 4; ++Index)
			ASSERT_EQ(ETaskState::Queued, Consumers[Index].GetState());
		EXPECT_EQ(ETaskState::Canceled, Consumers[4].GetState());
		EXPECT_EQ(ETaskTerminalReason::DispatchRejected, Consumers[4].GetDiagnostics().TerminalReason);
		EXPECT_EQ(256u, GetGameThreadDeferredWorkQueueDiagnostics().QueuedPayloadBytes);
		EXPECT_EQ(256u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);

		EXPECT_EQ(4u, PumpGameThreadDeferredWork().ExecutedCallbacks);
		EXPECT_EQ(4u, Runs.load(std::memory_order::acquire));
		EXPECT_EQ(0u, GetGameThreadDeferredWorkQueueDiagnostics().QueuedPayloadBytes);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);
	}

	TEST(FTaskContinuationTests, MoveOnlyTypedResultSupportsImmutableFanOutAndTypedChains)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		auto Root = LaunchTask<std::unique_ptr<int>>("TypedRoot", []() {
			return std::make_unique<int>(21);
		});
		auto Left = Then(Root, "TypedLeft", [](const std::unique_ptr<int>& Value) {
			return *Value * 2;
		});
		auto Right = Then(Root, "TypedRight", [](const std::unique_ptr<int>& Value) {
			return std::to_string(*Value);
		});
		auto Tail = Then(Left, "TypedTail", [](const int& Value) {
			EXPECT_EQ(42, Value);
		});

		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Tail).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Right.GetTaskHandle()).TaskState);
		auto RootResult = Root.GetResultShared();
		auto SecondRootOwner = Root.GetResultShared();
		auto LeftResult = Left.GetResultShared();
		auto RightResult = Right.GetResultShared();
		ASSERT_NE(RootResult, nullptr);
		EXPECT_EQ(RootResult.get(), SecondRootOwner.get());
		ASSERT_NE(LeftResult, nullptr);
		EXPECT_EQ(42, *LeftResult);
		ASSERT_NE(RightResult, nullptr);
		EXPECT_EQ("21", *RightResult);
	}

	TEST(FTaskContinuationTests, VoidAndTypedTasksComposeInBothDirections)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FTaskHandle Root = LaunchTask("VoidRoot", []() {});
		auto Typed = Then(Root, "VoidToTyped", []() { return 7; });
		FTaskHandle VoidTail = Then(Typed, "TypedToVoid", [](const int& Value) {
			EXPECT_EQ(7, Value);
		});

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(VoidTail).TaskState);
		ASSERT_NE(Typed.GetResultShared(), nullptr);
		EXPECT_EQ(7, *Typed.GetResultShared());
	}

	TEST(FTaskContinuationTests, TypedResultLivesUntilItsLastHandleOwnerIsReleased)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		std::weak_ptr<const int> WeakResult;
		{
			auto Task = LaunchTask<int>("TypedLifetime", []() { return 13; });
			ASSERT_EQ(ETaskState::Succeeded, WaitTask(Task.GetTaskHandle()).TaskState);
			auto Result = Task.GetResultShared();
			ASSERT_NE(Result, nullptr);
			WeakResult = Result;
			Result.reset();
			EXPECT_FALSE(WeakResult.expired());
			EXPECT_TRUE(Task.GetDiagnostics().bHasResultStorage);
			EXPECT_GE(GetTaskSchedulerDiagnostics().RetainedTerminalResultCount, 1u);
			// Terminal publication precedes the worker wrapper leaving its stack. A
			// later single-worker task proves that transient executor ownership ended.
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(LaunchTask("TypedLifetimeFence", []() {})).TaskState);
		}
		EXPECT_TRUE(WeakResult.expired());
	}

	TEST(FTaskContinuationTests, CompletionEdgeReceivesOwnedFailureOutcome)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto Failed = LaunchTask<int>("TypedFailure", []() -> int {
			throw std::runtime_error("typed failure");
		});
		auto Cleanup = ThenOutcome(Failed, "FailureCleanup", [](FTaskOutcome<int> Outcome) {
			EXPECT_EQ(ETaskState::Failed, Outcome.State);
			EXPECT_EQ(ETaskTerminalReason::CallbackFailure, Outcome.Reason);
			EXPECT_EQ("typed failure", Outcome.Diagnostic);
			EXPECT_EQ(nullptr, Outcome.Result);
			return Outcome.Diagnostic;
		});

		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Cleanup.GetTaskHandle()).TaskState);
		ASSERT_NE(Cleanup.GetResultShared(), nullptr);
		EXPECT_EQ("typed failure", *Cleanup.GetResultShared());
	}

	TEST(FTaskFanInTests, HeterogeneousResultsPreserveOrderDuplicatesAndLifetime)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FThreadEvent ReleaseInputs;
		auto Number = LaunchTask<int>("FanInNumber", [&]() {
			ReleaseInputs.Wait();
			return 17;
		});
		auto Text = LaunchTask<std::string>("FanInText", [&]() {
			ReleaseInputs.Wait();
			return std::string("typed");
		});
		auto Joined = WhenAll(std::make_tuple(Number, Text), "HeterogeneousFanIn",
			[Capture = std::make_unique<int>(2)](const int& Left, const std::string& Right) {
				return Right + ':' + std::to_string(Left * *Capture);
			});
		auto Repeated = WhenAll(std::make_tuple(Number, Number), "RepeatedFanIn",
			[](const int& Left, const int& Right) { return Left + Right; });
		auto AllSucceeded = WhenAllOutcome(std::make_tuple(Number, Text), "SuccessfulOutcomeFanIn",
			[](TTaskAggregateOutcome<int, std::string> Outcome) {
				EXPECT_EQ(ETaskState::Succeeded, Outcome.State);
				EXPECT_EQ(0u, Outcome.BlockingTaskId);
				EXPECT_EQ(ETaskTerminalReason::None, Outcome.Reason);
				EXPECT_NE(nullptr, std::get<0>(Outcome.Outcomes).Result);
				EXPECT_NE(nullptr, std::get<1>(Outcome.Outcomes).Result);
			});

		Number = {};
		Text = {};
		ReleaseInputs.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Joined.GetTaskHandle()).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Repeated.GetTaskHandle()).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(AllSucceeded).TaskState);
		ASSERT_NE(Joined.GetResultShared(), nullptr);
		EXPECT_EQ("typed:34", *Joined.GetResultShared());
		ASSERT_NE(Repeated.GetResultShared(), nullptr);
		EXPECT_EQ(34, *Repeated.GetResultShared());
		EXPECT_EQ(1u, Repeated.GetDiagnostics().PrerequisiteTaskIds.size());
	}

	TEST(FTaskFanInTests, SuccessAndOutcomeEdgesUseDeterministicTerminalRules)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		auto FirstFailure = LaunchTask<int>("FanInFirstFailure", []() -> int {
			throw std::runtime_error("first typed failure");
		});
		auto SecondFailure = LaunchTask<std::string>("FanInSecondFailure", []() -> std::string {
			throw std::runtime_error("second typed failure");
		});
		FTaskCancellationSource CanceledSource;
		CanceledSource.RequestCancellation();
		FTaskLaunchOptions CanceledOptions;
		CanceledOptions.CancellationToken = CanceledSource.GetToken();
		auto Canceled = LaunchTask<double>("FanInCanceled", []() { return 2.0; }, CanceledOptions);

		std::atomic<bool> bSuccessCallbackRan = false;
		FTaskHandle SuccessEdge = WhenAll(std::make_tuple(SecondFailure, Canceled, FirstFailure),
			"FailedSuccessFanIn", [&](const std::string&, const double&, const int&) {
				bSuccessCallbackRan.store(true, std::memory_order::release);
			});
		auto OutcomeEdge = WhenAllOutcome(std::make_tuple(SecondFailure, Canceled, FirstFailure),
			"TerminalOutcomeFanIn", [](TTaskAggregateOutcome<std::string, double, int> Outcome) {
				EXPECT_EQ(ETaskState::Failed, Outcome.State);
				EXPECT_EQ(ETaskTerminalReason::CallbackFailure, Outcome.Reason);
				EXPECT_EQ(std::get<2>(Outcome.Outcomes).Task.GetTaskId(), Outcome.BlockingTaskId);
				EXPECT_EQ("first typed failure", Outcome.Diagnostic);
				EXPECT_EQ(ETaskState::Failed, std::get<0>(Outcome.Outcomes).State);
				EXPECT_EQ(nullptr, std::get<0>(Outcome.Outcomes).Result);
				EXPECT_EQ(ETaskState::Canceled, std::get<1>(Outcome.Outcomes).State);
				EXPECT_EQ(nullptr, std::get<1>(Outcome.Outcomes).Result);
				EXPECT_EQ(ETaskState::Failed, std::get<2>(Outcome.Outcomes).State);
				return Outcome.BlockingTaskId;
			});

		EXPECT_EQ(ETaskState::Canceled, WaitTask(SuccessEdge).TaskState);
		EXPECT_FALSE(bSuccessCallbackRan.load(std::memory_order::acquire));
		EXPECT_EQ(ETaskTerminalReason::DependencyFailed, SuccessEdge.GetDiagnostics().TerminalReason);
		EXPECT_EQ(FirstFailure.GetTaskId(), SuccessEdge.GetDiagnostics().DirectBlockingTaskId);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(OutcomeEdge.GetTaskHandle()).TaskState);
		ASSERT_NE(OutcomeEdge.GetResultShared(), nullptr);
		EXPECT_EQ(FirstFailure.GetTaskId(), *OutcomeEdge.GetResultShared());
	}

	TEST(FTaskFanInTests, InvalidAdmissionDestroysMoveOnlyCallbackWithoutInvocation)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto Valid = LaunchTask<int>("ValidFanInInput", []() { return 4; });
		TTaskHandle<std::string> Invalid;
		auto Capture = std::make_shared<int>(1);
		std::weak_ptr<int> WeakCapture = Capture;
		std::atomic<bool> bRan = false;
		FTaskHandle Rejected = WhenAll(std::make_tuple(Valid, Invalid), "InvalidFanIn",
			[Capture = std::move(Capture), &bRan](const int&, const std::string&) {
				bRan.store(true, std::memory_order::release);
			});

		EXPECT_FALSE(Rejected.IsValid());
		EXPECT_FALSE(bRan.load(std::memory_order::acquire));
		EXPECT_TRUE(WeakCapture.expired());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Valid.GetTaskHandle()).TaskState);

		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Current = LaunchTask<std::string>("CurrentFanInLifetime", []() { return std::string("current"); });
		EXPECT_FALSE(WhenAll(std::make_tuple(Valid, Current), "MixedLifetimeFanIn",
			[](const int&, const std::string&) {}).IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Current.GetTaskHandle()).TaskState);
	}

	TEST(FTaskContinuationTests, SuccessFanInWaitsForEveryTerminalAndSelectsStableBlocker)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		FThreadEvent ReleaseSlowFailure;
		FTaskHandle FirstFailure = LaunchTask("FirstFanInFailure", []() {
			throw std::runtime_error("first");
		});
		FTaskHandle SlowFailure = LaunchTask("SlowFanInFailure", [&]() {
			ReleaseSlowFailure.Wait();
			throw std::runtime_error("slow");
		});
		auto Primary = LaunchTask<int>("FanInPrimary", []() { return 9; });
		std::array<FTaskHandle, 2> Additional{SlowFailure, FirstFailure};
		FTaskContinuationOptions Options;
		Options.Prerequisites = Additional;
		std::atomic<bool> bRan = false;
		auto Join = Then(Primary, "TypedFanIn", [&](const int&) {
			bRan.store(true, std::memory_order::release);
		}, Options);

		ASSERT_EQ(ETaskState::Failed, WaitTask(FirstFailure).TaskState);
		EXPECT_EQ(ETaskState::Waiting, Join.GetState());
		ReleaseSlowFailure.Trigger();
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Join).TaskState);
		EXPECT_FALSE(bRan.load(std::memory_order::acquire));
		const FTaskDiagnostics Diagnostics = Join.GetDiagnostics();
		EXPECT_EQ(ETaskTerminalReason::DependencyFailed, Diagnostics.TerminalReason);
		EXPECT_EQ(std::min(FirstFailure.GetTaskId(), SlowFailure.GetTaskId()), Diagnostics.DirectBlockingTaskId);
		ASSERT_EQ(3u, Diagnostics.PrerequisiteTaskIds.size());
		EXPECT_TRUE(std::ranges::all_of(Diagnostics.PrerequisiteDependencyKinds,
			[](ETaskDependencyKind Kind) { return Kind == ETaskDependencyKind::Success; }));
	}

	TEST(FTaskContinuationTests, RunningCancellationNeverPublishesPendingTypedResult)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent Started;
		FThreadEvent ReturnValue;
		auto Task = LaunchCancelableTask<std::unique_ptr<int>>(
			"CanceledTypedResult",
			[&](const FTaskCancellationToken&) {
				Started.Trigger();
				ReturnValue.Wait();
				return std::make_unique<int>(5);
			}
		);
		ASSERT_TRUE(Started.WaitFor(1.0));
		EXPECT_TRUE(CancelTask(Task.GetTaskHandle()));
		ReturnValue.Trigger();

		EXPECT_EQ(ETaskState::Canceled, WaitTask(Task.GetTaskHandle()).TaskState);
		EXPECT_EQ(nullptr, Task.GetResultShared());
		EXPECT_EQ(ETaskTerminalReason::CancellationRequested, Task.GetDiagnostics().TerminalReason);
	}

	TEST(FTaskContinuationTests, RegistrationRaceReleasesTypedContinuationExactlyOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));

		for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
		{
			std::barrier Race(3);
			auto Root = LaunchTask<int>("TypedRegistrationRaceRoot", [&]() {
				Race.arrive_and_wait();
				return 1;
			});
			std::atomic<uint32> Runs = 0;
			TTaskHandle<int> Continuation;
			std::thread Submitter([&]() {
				Race.arrive_and_wait();
				Continuation = Then(Root, "TypedRegistrationRaceContinuation", [&](const int& Value) {
					Runs.fetch_add(static_cast<uint32>(Value), std::memory_order::acq_rel);
					return Value + 1;
				});
			});
			Race.arrive_and_wait();
			Submitter.join();

			ASSERT_TRUE(Continuation.IsValid());
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Continuation.GetTaskHandle()).TaskState);
			EXPECT_EQ(1u, Runs.load(std::memory_order::acquire));
			ASSERT_NE(Continuation.GetResultShared(), nullptr);
			EXPECT_EQ(2, *Continuation.GetResultShared());
		}
	}

	TEST(FTaskContinuationTests, ForeignLifetimeIsRejectedAndMissingTargetTerminalizesDispatch)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		auto EarlierLifetime = LaunchTask<int>("EarlierTypedLifetime", []() { return 3; });
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(EarlierLifetime.GetTaskHandle()).TaskState);
		ShutdownTaskScheduler(true);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		EXPECT_FALSE(Then(EarlierLifetime, "ForeignTypedContinuation", [](const int&) {}).IsValid());
		auto Current = LaunchTask<int>("CurrentTypedLifetime", []() { return 4; });
		FTaskContinuationOptions DeferredOptions;
		DeferredOptions.Target = ETaskTarget::GameThreadDeferred;
		DeferredOptions.EstimatedPayloadBytes = 1;
		FTaskHandle Unavailable = Then(Current, "UnavailableDeferredTarget", [](const int&) {}, DeferredOptions);
		ASSERT_TRUE(Unavailable.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Current.GetTaskHandle()).TaskState);
		const auto RejectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!Unavailable.IsComplete() && std::chrono::steady_clock::now() < RejectionDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(ETaskState::Canceled, Unavailable.GetState());
		EXPECT_EQ(ETaskTerminalReason::DispatchRejected, Unavailable.GetDiagnostics().TerminalReason);
	}

	TEST(FTaskContinuationTests, DrainAndDiscardShutdownTerminalizeContinuationChains)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent DrainStarted;
		FThreadEvent ReleaseDrain;
		auto DrainRoot = LaunchTask<int>("DrainTypedRoot", [&]() {
			DrainStarted.Trigger();
			ReleaseDrain.Wait();
			return 8;
		});
		auto DrainTail = Then(DrainRoot, "DrainTypedTail", [](const int& Value) { return Value + 1; });
		ASSERT_TRUE(DrainStarted.WaitFor(1.0));
		std::thread DrainThread([]() { ShutdownTaskScheduler(true); });
		while (IsTaskSchedulerRunning()) std::this_thread::yield();
		ReleaseDrain.Trigger();
		DrainThread.join();
		EXPECT_EQ(ETaskState::Succeeded, DrainRoot.GetState());
		EXPECT_EQ(ETaskState::Succeeded, DrainTail.GetState());
		ASSERT_NE(DrainTail.GetResultShared(), nullptr);
		EXPECT_EQ(9, *DrainTail.GetResultShared());

		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent DiscardStarted;
		FThreadEvent ReleaseDiscard;
		auto DiscardRoot = LaunchCancelableTask<int>("DiscardTypedRoot", [&](const FTaskCancellationToken& Token) {
			DiscardStarted.Trigger();
			ReleaseDiscard.Wait();
			while (!Token.IsCancellationRequested()) std::this_thread::yield();
			return 10;
		});
		auto DiscardTail = Then(DiscardRoot, "DiscardTypedTail", [](const int& Value) { return Value + 1; });
		ASSERT_TRUE(DiscardStarted.WaitFor(1.0));
		std::thread DiscardThread([]() { ShutdownTaskScheduler(false); });
		while (IsTaskSchedulerRunning()) std::this_thread::yield();
		ReleaseDiscard.Trigger();
		DiscardThread.join();
		EXPECT_EQ(ETaskState::Canceled, DiscardRoot.GetState());
		EXPECT_EQ(ETaskState::Canceled, DiscardTail.GetState());
		EXPECT_EQ(nullptr, DiscardRoot.GetResultShared());
		EXPECT_EQ(nullptr, DiscardTail.GetResultShared());
	}

	TEST(FGameThreadDeferredTaskTests, WorkerContinuationRunsOnlyFromGameThreadPump)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());

		auto Root = LaunchTask<int>("DeferredRoot", []() { return 12; });
		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 32;
		std::atomic<bool> bRan = false;
		FTaskHandle Deferred = Then(Root, "DeferredTail", [&](const int& Value) {
			EXPECT_TRUE(IsInGameThread());
			EXPECT_EQ(12, Value);
			bRan.store(true, std::memory_order::release);
		}, Options);

		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root.GetTaskHandle()).TaskState);
		const auto DispatchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (Deferred.GetState() == ETaskState::Waiting && std::chrono::steady_clock::now() < DispatchDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(ETaskState::Queued, Deferred.GetState());
		const FTaskWaitResult DeferredWaitResult = WaitTask(Deferred);
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, DeferredWaitResult.WaitStatus);
		EXPECT_EQ(ETaskState::Queued, DeferredWaitResult.TaskState);
		EXPECT_FALSE(bRan.load(std::memory_order::acquire));
		EXPECT_EQ(1u, GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth);

		const FGameThreadDeferredPumpResult PumpResult = PumpGameThreadDeferredWork();
		EXPECT_EQ(1u, PumpResult.ExecutedCallbacks);
		EXPECT_EQ(ETaskState::Succeeded, Deferred.GetState());
		EXPECT_TRUE(bRan.load(std::memory_order::acquire));
	}

	TEST(FGameThreadDeferredTaskTests, TypedFanInRunsOnlyFromGameThreadPump)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());

		auto Number = LaunchTask<int>("DeferredFanInNumber", []() { return 5; });
		auto Text = LaunchTask<std::string>("DeferredFanInText", []() { return std::string("five"); });
		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 32;
		std::atomic<bool> bRan = false;
		FTaskHandle Deferred = WhenAll(std::make_tuple(Number, Text), "DeferredTypedFanIn",
			[&](const int& Value, const std::string& Label) {
				EXPECT_TRUE(IsInGameThread());
				EXPECT_EQ(5, Value);
				EXPECT_EQ("five", Label);
				bRan.store(true, std::memory_order::release);
			}, Options);

		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Number.GetTaskHandle()).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Text.GetTaskHandle()).TaskState);
		const auto DispatchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (Deferred.GetState() == ETaskState::Waiting && std::chrono::steady_clock::now() < DispatchDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(ETaskState::Queued, Deferred.GetState());
		const FTaskWaitResult DeferredWaitResult = WaitTask(Deferred);
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, DeferredWaitResult.WaitStatus);
		EXPECT_EQ(ETaskState::Queued, DeferredWaitResult.TaskState);
		EXPECT_FALSE(bRan.load(std::memory_order::acquire));
		EXPECT_EQ(1u, PumpGameThreadDeferredWork().ExecutedCallbacks);
		EXPECT_EQ(ETaskState::Succeeded, Deferred.GetState());
		EXPECT_TRUE(bRan.load(std::memory_order::acquire));
	}

	TEST(FGameThreadDeferredTaskTests, PriorityFifoAndItemBudgetAreDeterministic)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FTaskHandle Root = LaunchTask("DeferredPriorityRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		std::vector<int> Order;
		auto Enqueue = [&](int Value, ETaskPriority Priority) {
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.Priority = Priority;
			Options.EstimatedPayloadBytes = 1;
			return Then(Root, "DeferredPriorityEntry", [&, Value]() { Order.push_back(Value); }, Options);
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
		FTaskHandle Root = LaunchTask("DeferredLimitRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		auto Enqueue = [&](uint64 Bytes) {
			FTaskContinuationOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = Bytes;
			return Then(Root, "DeferredLimitEntry", []() {}, Options);
		};
		FTaskHandle First = Enqueue(5);
		FTaskHandle Second = Enqueue(5);
		FTaskHandle CountRejected = Enqueue(1);
		FTaskHandle EntryRejected = Enqueue(9);
		FTaskHandle ZeroRejected = Enqueue(0);

		EXPECT_EQ(ETaskState::Queued, First.GetState());
		EXPECT_EQ(ETaskState::Queued, Second.GetState());
		EXPECT_EQ(ETaskState::Canceled, CountRejected.GetState());
		EXPECT_EQ(ETaskState::Canceled, EntryRejected.GetState());
		EXPECT_EQ(ETaskState::Canceled, ZeroRejected.GetState());
		EXPECT_TRUE(CancelTask(First));
		FTaskHandle Replacement = Enqueue(5);
		EXPECT_EQ(ETaskState::Queued, Replacement.GetState());
		const auto Diagnostics = GetGameThreadDeferredWorkQueueDiagnostics();
		EXPECT_EQ(2u, Diagnostics.QueueDepth);
		EXPECT_EQ(10u, Diagnostics.QueuedPayloadBytes);
		EXPECT_EQ(3u, Diagnostics.RejectedCount);
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
		FTaskHandle Root = LaunchTask("DeferredTimeBudgetRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		FTaskHandle Slow = Then(Root, "DeferredSlowCallback", []() {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}, Options);
		FTaskHandle Later = Then(Root, "DeferredLaterCallback", []() {}, Options);
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
		FTaskHandle Root = LaunchTask("DeferredPolicyRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskContinuationOptions CoalescedOptions;
		CoalescedOptions.Target = ETaskTarget::GameThreadDeferred;
		CoalescedOptions.EstimatedPayloadBytes = 4;
		CoalescedOptions.CoalescingKey = FTaskCoalescingKey{11, 22, 33};
		FTaskHandle Superseded = Then(Root, "DeferredSuperseded", []() {}, CoalescedOptions);
		std::atomic<bool> bReplacementRan = false;
		FTaskHandle Replacement = Then(Root, "DeferredReplacement", [&]() { bReplacementRan = true; }, CoalescedOptions);
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
		FTaskHandle Stale = Then(Root, "DeferredStale", []() {}, StaleOptions);
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
		FTaskHandle Root = LaunchTask("DeferredFailureRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		FTaskHandle Failed = Then(Root, "DeferredFailure", []() {
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
		FTaskHandle DrainRoot = LaunchTask("CrossDrainRoot", [&]() {
			DrainRootStarted.Trigger();
			ReleaseDrainRoot.Wait();
		});
		FTaskContinuationOptions Options;
		Options.Target = ETaskTarget::GameThreadDeferred;
		Options.EstimatedPayloadBytes = 1;
		std::atomic<bool> bDrainRan = false;
		FTaskHandle DrainTail = Then(DrainRoot, "CrossDrainTail", [&]() {
			bDrainRan = true;
			EXPECT_FALSE(LaunchTask("RejectedShutdownRoot", []() {}).IsValid());
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
		FTaskHandle CancelRoot = LaunchTask("CrossCancelRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(CancelRoot).TaskState);
		FTaskHandle CancelTail = Then(CancelRoot, "CrossCancelTail", []() {}, Options);
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
		FTaskHandle Root = LaunchTask("DeferredMeasurementRoot", []() {});
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
			Handles.emplace_back(Then(Root, "DeferredMeasurementCallback", [&]() {
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

		FTaskHandle Handle = LaunchTask("RejectedTask", []() {});

		EXPECT_FALSE(Handle.IsValid());
		EXPECT_FALSE(Handle.IsComplete());
	}
} // namespace Durin
