#include <gtest/gtest.h>

#include "Threading/Runnable.h"
#include "Threading/RunnableThread.h"
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
}
