#include "CoreMinimal.h"
#include <gtest/gtest.h>
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/TaskConcurrencyLimiter.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	namespace
	{
		auto EnsureGameThreadForTaskTest() -> void
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
		}
		class FEngineThreadPoolTestGuard
		{
		public:
			FEngineThreadPoolTestGuard() { EnsureGameThreadForTaskTest(); }
			~FEngineThreadPoolTestGuard() { ShutdownTaskScheduler(false); }
		};
	}

	TEST(FTaskConcurrencyLimiterTests, ConcurrentProducersRespectLimitAndAllWorkCompletes)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(6));
		Tasks::FTaskConcurrencyLimiter Limiter(3);
		FThreadEvent Saturated, Release;
		std::atomic<int> Active = 0, Peak = 0, Runs = 0;
		std::array<std::atomic<bool>, 3> Slots{};
		std::array<std::thread, 4> Producers;
		for (auto& Producer : Producers) Producer = std::thread([&] {
			for (int Index = 0; Index < 16; ++Index) Limiter.Push("Limited", [&](uint32 Slot) {
				ASSERT_LT(Slot, Slots.size());
				EXPECT_FALSE(Slots[Slot].exchange(true));
				const int Count = ++Active;
				int Previous = Peak.load();
				while (Count > Previous && !Peak.compare_exchange_weak(Previous, Count)) {}
				if (Count == 3) Saturated.Trigger();
				(void)Release.WaitFor(5);
				++Runs;
				--Active;
				EXPECT_TRUE(Slots[Slot].exchange(false));
			});
		});
		for (auto& Producer : Producers) Producer.join();
		EXPECT_TRUE(Saturated.WaitFor(2));
		EXPECT_FALSE(Limiter.Wait(0.001));
		EXPECT_EQ(3, Peak.load());
		Release.Trigger();
		EXPECT_TRUE(Limiter.Wait(5));
		EXPECT_EQ(64, Runs.load());
		EXPECT_EQ(3, Peak.load());
		EXPECT_EQ(0, Active.load());
		EXPECT_TRUE(Limiter.Wait(0));
	}

	TEST(FTaskConcurrencyLimiterTests, FreeSlotsProgressWhileEarlierTaskIsBlocked)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(3));
		Tasks::FTaskConcurrencyLimiter Limiter(2);
		FThreadEvent Started, Release, Tail;
		Limiter.Push("Long", [&](uint32) { Started.Trigger(); (void)Release.WaitFor(5); });
		EXPECT_TRUE(Started.WaitFor(2));
		for (int Index = 0; Index < 32; ++Index) Limiter.Push("Short", [](uint32) {});
		Limiter.Push("Tail", [&](uint32) { Tail.Trigger(); });
		EXPECT_TRUE(Tail.WaitFor(2));
		Release.Trigger();
		EXPECT_TRUE(Limiter.Wait(5));
	}

	TEST(FTaskConcurrencyLimiterTests, BodyCompletionReleasesSlotBeforeNestedCompletion)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskConcurrencyLimiter Limiter(1);
		Tasks::FTaskEvent Nested("LimiterNested", false);
		FThreadEvent BodyFinished;
		std::atomic<int> Destroyed = 0, Runs = 0;
		auto Capture = std::shared_ptr<int>(new int(1), [&](int* Value) { delete Value; ++Destroyed; });
		Limiter.Push("Parent", [Capture = std::move(Capture), &Nested, &BodyFinished](uint32) {
			Tasks::AddNested(Nested);
			BodyFinished.Trigger();
		});
		EXPECT_TRUE(BodyFinished.WaitFor(2));
		Limiter.Push("AfterBody", [&](uint32) { ++Runs; });
		EXPECT_TRUE(Limiter.Wait(2));
		EXPECT_EQ(1, Runs.load());
		EXPECT_FALSE(Nested.IsCompleted());
		EXPECT_GT(GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount, 0u);
		Nested.Trigger();
		ShutdownTaskScheduler(true);
		EXPECT_EQ(1, Destroyed.load());
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
	}

	TEST(FTaskConcurrencyLimiterTests, SlotAndWaitCompleteBeforeCaptureDestruction)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskConcurrencyLimiter Limiter(1);
		FThreadEvent BodyStarted, ReleaseBody, Destroying, ReleaseCapture;
		std::atomic<int> Destroyed = 0, Runs = 0;
		auto Capture = std::shared_ptr<int>(new int(1), [&](int* Value) {
			Destroying.Trigger();
			(void)ReleaseCapture.WaitFor(5);
			delete Value;
			++Destroyed;
		});
		Limiter.Push("RetiringCapture", [Capture = std::move(Capture), &BodyStarted, &ReleaseBody](uint32) {
			BodyStarted.Trigger();
			(void)ReleaseBody.WaitFor(5);
		});
		EXPECT_TRUE(BodyStarted.WaitFor(2));
		Limiter.Push("NextBody", [&](uint32) { ++Runs; });
		ReleaseBody.Trigger();
		EXPECT_TRUE(Destroying.WaitFor(2));
		EXPECT_TRUE(Limiter.Wait(0.1));
		EXPECT_EQ(1, Runs.load());
		EXPECT_EQ(0, Destroyed.load());
		ReleaseCapture.Trigger();
		ShutdownTaskScheduler(true);
		EXPECT_EQ(1, Destroyed.load());
	}

	TEST(FTaskConcurrencyLimiterTests, EarlyDestructionExceptionsAndCancellationPreserveAcceptedWork)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		FThreadEvent Started, Release;
		FTaskCancellationSource Cancellation;
		std::atomic<int> Runs = 0, Destroyed = 0;
		{
			Tasks::FTaskConcurrencyLimiter Limiter(1);
			Limiter.Push("Blocker", [&](uint32) { Started.Trigger(); (void)Release.WaitFor(5); });
			EXPECT_TRUE(Started.WaitFor(2));
			Limiter.Push("Throws", [](uint32) { throw std::runtime_error("limiter failure"); });
			auto Capture = std::shared_ptr<int>(new int(1), [&](int* Value) { delete Value; ++Destroyed; });
			Limiter.Push("Canceled", [Capture = std::move(Capture), &Runs](uint32, const FTaskCancellationToken& Token) {
				EXPECT_TRUE(Token.IsCancellationRequested()); ++Runs;
			}, Cancellation.GetToken());
			Limiter.Push("Tail", [&](uint32) { ++Runs; });
			Cancellation.RequestCancellation();
		}
		Release.Trigger();
		ShutdownTaskScheduler(true);
		EXPECT_EQ(2, Runs.load());
		EXPECT_EQ(1, Destroyed.load());
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount);
	}

	TEST(FTaskConcurrencyLimiterTests, WorkerWaitRetractsOnlyLimitedWorkAndSelfWaitIsRejected)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Done;
		auto Parent = Tasks::LaunchTask("LimiterWaiter", [&] {
			Tasks::FTaskConcurrencyLimiter Limiter(1);
			int Runs = 0;
			Limiter.Push("SelfWait", [&](uint32) {
				EXPECT_FALSE(Limiter.Wait());
				++Runs;
			});
			Limiter.Push("Next", [&](uint32) { ++Runs; });
			EXPECT_TRUE(Limiter.Wait(2));
			EXPECT_EQ(2, Runs);
			Done.Trigger();
		});
		EXPECT_TRUE(Done.WaitFor(5));
		EXPECT_EQ(ETaskState::Succeeded, Parent.Wait().TaskState);
	}

	TEST(FTaskConcurrencyLimiterTests, WorkerWaitProgressesEverySlotBeforeBlocking)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Event("CrossSlotCompletion", false);
		FThreadEvent Done;
		auto Parent = Tasks::LaunchTask("CrossSlotWaiter", [&] {
			Tasks::FTaskConcurrencyLimiter Limiter(2);
			Limiter.Push("NestedWaiter", [&](uint32) { Tasks::AddNested(Event); });
			Limiter.Push("HandOffOtherSlot", [](uint32) {});
			Limiter.Push("SignalOtherSlot", [&](uint32) { Event.Trigger(); });
			EXPECT_TRUE(Limiter.Wait(0.1));
			Event.Trigger(); // Keep a failing regression bounded and drainable.
			Done.Trigger();
		});
		EXPECT_TRUE(Done.WaitFor(5));
		EXPECT_EQ(ETaskState::Succeeded, Parent.Wait().TaskState);
		ShutdownTaskScheduler(true);
	}

	TEST(FTaskConcurrencyLimiterTests, WaitDrainsCurrentWorkAfterAnEarlierSuccessfulWait)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskConcurrencyLimiter Limiter(1);
		EXPECT_TRUE(Limiter.Wait(0));
		Limiter.Push("FirstBatch", [](uint32) {});
		EXPECT_TRUE(Limiter.Wait(2));
		FThreadEvent Started, Release;
		Limiter.Push("LaterBatch", [&](uint32) { Started.Trigger(); (void)Release.WaitFor(5); });
		EXPECT_TRUE(Started.WaitFor(2));
		EXPECT_FALSE(Limiter.Wait(0));
		EXPECT_FALSE(Limiter.Wait(0.001));
		Release.Trigger();
		EXPECT_TRUE(Limiter.Wait(2));
		ShutdownTaskScheduler(true);
	}

	TEST(FTaskConcurrencyLimiterTests, ConcurrentWaitersObserveBodyCompletion)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskConcurrencyLimiter Limiter(1);
		FThreadEvent BodyStarted, ReleaseBody;
		std::atomic<int> Runs = 0, Entered = 0;
		Limiter.Push("SharedWait", [Capture = std::make_unique<int>(7), &BodyStarted, &ReleaseBody, &Runs](uint32) {
			EXPECT_EQ(7, *Capture);
			BodyStarted.Trigger();
			(void)ReleaseBody.WaitFor(5);
			++Runs;
		});
		EXPECT_TRUE(BodyStarted.WaitFor(2));
		std::array<std::thread, 2> Waiters;
		for (auto& Waiter : Waiters) Waiter = std::thread([&] {
			++Entered;
			EXPECT_TRUE(Limiter.Wait(2));
		});
		while (Entered.load() != 2) std::this_thread::yield();
		ReleaseBody.Trigger();
		for (auto& Waiter : Waiters) Waiter.join();
		EXPECT_EQ(1, Runs.load());
		EXPECT_TRUE(Limiter.Wait(0));
	}

	TEST(FTaskConcurrencyLimiterTests, CancelShutdownDrainsQueuedTasksAndReleasesLimiterState)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskConcurrencyLimiter Limiter(1);
		FThreadEvent Started;
		std::atomic<int> Runs = 0;
		Limiter.Push("ObserveShutdown", [&](uint32, const FTaskCancellationToken& Token) {
			Started.Trigger();
			while (!Token.IsCancellationRequested()) std::this_thread::yield();
		});
		EXPECT_TRUE(Started.WaitFor(2));
		for (int Index = 0; Index < 64; ++Index) Limiter.Push("QueuedAtShutdown", [&](uint32) { ++Runs; });
		ShutdownTaskScheduler(false);
		EXPECT_TRUE(Limiter.Wait(1));
		EXPECT_EQ(64, Runs.load());
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
	}

}
