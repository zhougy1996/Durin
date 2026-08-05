#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "RHIThread.h"
#include "Threading/ThreadEvent.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		class FRHIThreadTestGuard
		{
		public:
			explicit FRHIThreadTestGuard(FRHIThread& InThread)
				: Thread(InThread)
			{
			}

			~FRHIThreadTestGuard()
			{
				Thread.Stop();
			}

		private:
			FRHIThread& Thread;
		};
	}

	TEST(FRHIThreadTests, StartsNamedRoleAndPublishesSynchronousResult)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());

		std::string ObservedName;
		EThreadRole ObservedRole = EThreadRole::Unknown;
		bool bObservedAffinity = false;
		bool bObservedGlobalOwner = false;
		int Value = 0;
		FRHIThreadWork Work;
		Work.Execute = [&]() {
			ObservedName = GetCurrentThreadName();
			ObservedRole = GetCurrentThreadRole();
			bObservedAffinity = IsInRHIThread();
			bObservedGlobalOwner = GetCurrentThread() == GRHIThread;
			Value = 42;
			return FRHIThreadWorkResult::Success();
		};

		const FRHIThreadSynchronousResult Result = Thread.EnqueueSynchronous(Work);

		EXPECT_TRUE(Result.IsCompleted());
		EXPECT_EQ(42, Value);
		EXPECT_EQ("RHIThread", ObservedName);
		EXPECT_EQ(EThreadRole::RHIThread, ObservedRole);
		EXPECT_TRUE(bObservedAffinity);
		EXPECT_TRUE(bObservedGlobalOwner);
	}

	TEST(FRHIThreadTests, DispatchReturnsBeforeFIFOReplayAndExactSerialCompletes)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		FThreadEvent FirstStarted;
		FThreadEvent ReleaseFirst;
		std::mutex OrderMutex;
		std::vector<int> Order;

		FRHIThreadWork First;
		First.Execute = [&]() {
			FirstStarted.Trigger();
			ReleaseFirst.Wait();
			std::lock_guard Lock(OrderMutex);
			Order.push_back(1);
			return FRHIThreadWorkResult::Success();
		};
		const FRHIThreadSubmission FirstSubmission = Thread.Enqueue(First);
		ASSERT_TRUE(FirstSubmission.IsAccepted());
		ASSERT_TRUE(FirstStarted.WaitFor(1.0));

		FRHIThreadWork Second;
		Second.Execute = [&]() {
			std::lock_guard Lock(OrderMutex);
			Order.push_back(2);
			return FRHIThreadWorkResult::Success();
		};
		const FRHIThreadSubmission SecondSubmission = Thread.Enqueue(Second);
		ASSERT_TRUE(SecondSubmission.IsAccepted());
		EXPECT_FALSE(Thread.GetStats().CompletedSerial >= SecondSubmission.Serial);

		ReleaseFirst.Trigger();
		EXPECT_EQ(ERHIThreadWaitResult::Completed,
			Thread.WaitForSerial(SecondSubmission.Serial));
		std::lock_guard Lock(OrderMutex);
		EXPECT_EQ((std::vector<int>{1, 2}), Order);
	}

	TEST(FRHIThreadTests, BoundedCapacityBlocksProducerAndWakesAfterCompletion)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		FRHIThreadQueueLimits Limits;
		Limits.MaxEntries = 1;
		Limits.MaxBatches = 1;
		Limits.MaxPayloadBytes = 64;
		ASSERT_TRUE(Thread.Start(Limits));
		FThreadEvent FirstStarted;
		FThreadEvent ReleaseFirst;

		FRHIThreadWork First;
		First.BatchCount = 1;
		First.PayloadBytes = 64;
		First.Execute = [&]() {
			FirstStarted.Trigger();
			ReleaseFirst.Wait();
			return FRHIThreadWorkResult::Success();
		};
		ASSERT_TRUE(Thread.Enqueue(First).IsAccepted());
		if (!FirstStarted.WaitFor(1.0))
		{
			ReleaseFirst.Trigger();
			FAIL() << "first work item did not start";
		}

		std::atomic<bool> bSecondAccepted = false;
		FRHIThreadWork Second;
		Second.BatchCount = 1;
		Second.PayloadBytes = 64;
		Second.Execute = []() { return FRHIThreadWorkResult::Success(); };
		std::thread Producer([&]() {
			bSecondAccepted.store(
				Thread.Enqueue(Second).IsAccepted(), std::memory_order::release);
		});
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (Thread.GetStats().BackpressureWaitCount == 0
			&& std::chrono::steady_clock::now() < Deadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(1u, Thread.GetStats().BackpressureWaitCount);
		EXPECT_FALSE(bSecondAccepted.load(std::memory_order::acquire));

		ReleaseFirst.Trigger();
		Producer.join();
		EXPECT_TRUE(bSecondAccepted.load(std::memory_order::acquire));
		EXPECT_EQ(ERHIThreadWaitResult::Completed, Thread.Flush());
		const FRHIThreadStats Stats = Thread.GetStats();
		EXPECT_GT(Stats.BackpressureWaitNanoseconds, 0u);
		EXPECT_EQ(Stats.PeakOutstandingEntryCount, 1u);
		EXPECT_EQ(Stats.PeakOutstandingBatchCount, 1u);
		EXPECT_EQ(Stats.PeakOutstandingPayloadBytes, 64u);
	}

	TEST(FRHIThreadTests, DrainRejectsLateAndOversizedWorkWithoutMovingPayload)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		FRHIThreadQueueLimits Limits;
		Limits.MaxPayloadBytes = 8;
		ASSERT_TRUE(Thread.Start(Limits));

		FRHIThreadWork Oversized;
		Oversized.PayloadBytes = 9;
		Oversized.Execute = []() { return FRHIThreadWorkResult::Success(); };
		EXPECT_EQ(ERHIThreadEnqueueResult::Oversized,
			Thread.Enqueue(Oversized).Result);
		EXPECT_TRUE(static_cast<bool>(Oversized.Execute));

		Thread.BeginDrain();
		FRHIThreadWork Late;
		Late.Execute = []() { return FRHIThreadWorkResult::Success(); };
		EXPECT_FALSE(Thread.Enqueue(Late).IsAccepted());
		EXPECT_TRUE(static_cast<bool>(Late.Execute));
	}

	TEST(FRHIThreadTests,
		TerminalMarkerClosesAdmissionAndWakesBackpressuredProducer)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		FRHIThreadQueueLimits Limits;
		Limits.MaxEntries = 1;
		Limits.MaxBatches = 1;
		Limits.MaxPayloadBytes = 64;
		ASSERT_TRUE(Thread.Start(Limits));
		FThreadEvent FirstStarted;
		FThreadEvent ReleaseFirst;

		FRHIThreadWork First;
		First.BatchCount = 1;
		First.PayloadBytes = 64;
		First.Execute = [&]() {
			FirstStarted.Trigger();
			ReleaseFirst.Wait();
			return FRHIThreadWorkResult::Success();
		};
		ASSERT_TRUE(Thread.Enqueue(First).IsAccepted());
		ASSERT_TRUE(FirstStarted.WaitFor(1.0));

		ERHIThreadEnqueueResult BlockedResult =
			ERHIThreadEnqueueResult::Accepted;
		FRHIThreadWork Blocked;
		Blocked.BatchCount = 1;
		Blocked.PayloadBytes = 64;
		Blocked.Execute = []() { return FRHIThreadWorkResult::Success(); };
		std::thread Producer([&]() {
			BlockedResult = Thread.Enqueue(Blocked).Result;
		});
		const auto Deadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (Thread.GetStats().BackpressureWaitCount == 0
			&& std::chrono::steady_clock::now() < Deadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(1u, Thread.GetStats().BackpressureWaitCount);

		bool bTerminalExecuted = false;
		FRHIThreadWork Terminal;
		Terminal.Execute = [&]() {
			bTerminalExecuted = true;
			return FRHIThreadWorkResult::Success();
		};
		const FRHIThreadSubmission TerminalSubmission =
			Thread.EnqueueTerminal(Terminal);
		if (!TerminalSubmission.IsAccepted())
		{
			ReleaseFirst.Trigger();
			Producer.join();
			FAIL() << "terminal marker was rejected";
		}
		Producer.join();
		EXPECT_EQ(ERHIThreadEnqueueResult::Draining, BlockedResult);
		EXPECT_TRUE(static_cast<bool>(Blocked.Execute));

		ReleaseFirst.Trigger();
		EXPECT_EQ(ERHIThreadWaitResult::Completed,
			Thread.WaitForSerial(TerminalSubmission.Serial));
		EXPECT_TRUE(bTerminalExecuted);
		EXPECT_EQ(0u, Thread.GetStats().OutstandingEntryCount);
	}

	TEST(FRHIThreadTests, AcceptedAndRejectedPayloadsDestroyExactlyOnce)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		FRHIThreadQueueLimits Limits;
		Limits.MaxPayloadBytes = 8;
		ASSERT_TRUE(Thread.Start(Limits));
		std::atomic<uint32> AcceptedDestroyedCount = 0;
		std::atomic<uint32> RejectedDestroyedCount = 0;

		struct FTrackedCapture
		{
			explicit FTrackedCapture(std::atomic<uint32>& InCount)
				: Count(InCount)
			{
			}
			~FTrackedCapture() { Count.fetch_add(1, std::memory_order::acq_rel); }
			std::atomic<uint32>& Count;
		};

		FRHIThreadWork Accepted;
		Accepted.PayloadBytes = 8;
		Accepted.Execute = [Tracked = std::make_shared<FTrackedCapture>(AcceptedDestroyedCount)]() {
			return FRHIThreadWorkResult::Success();
		};
		EXPECT_TRUE(Thread.EnqueueSynchronous(Accepted).IsCompleted());
		EXPECT_EQ(1u, AcceptedDestroyedCount.load(std::memory_order::acquire));

		FRHIThreadWork Rejected;
		Rejected.PayloadBytes = 9;
		Rejected.Execute = [Tracked = std::make_shared<FTrackedCapture>(RejectedDestroyedCount)]() {
			return FRHIThreadWorkResult::Success();
		};
		EXPECT_EQ(ERHIThreadEnqueueResult::Oversized,
			Thread.Enqueue(Rejected).Result);
		EXPECT_EQ(0u, RejectedDestroyedCount.load(std::memory_order::acquire));
		Rejected.Execute = {};
		EXPECT_EQ(1u, RejectedDestroyedCount.load(std::memory_order::acquire));
	}

	TEST(FRHIThreadTests, RHIThreadSelfWaitIsRejected)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		ERHIThreadWaitResult ObservedWaitResult = ERHIThreadWaitResult::Completed;

		FRHIThreadWork Work;
		Work.Execute = [&]() {
			ObservedWaitResult = Thread.WaitForSerial(1);
			return FRHIThreadWorkResult::Success();
		};
		EXPECT_TRUE(Thread.EnqueueSynchronous(Work).IsCompleted());
		EXPECT_EQ(ERHIThreadWaitResult::SelfWait, ObservedWaitResult);
	}

	TEST(FRHIThreadTests, RHIThreadSelfEnqueueIsRejected)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		ERHIThreadEnqueueResult ObservedResult = ERHIThreadEnqueueResult::Accepted;
		bool bNestedWorkRetained = false;

		FRHIThreadWork Work;
		Work.Execute = [&]() {
			FRHIThreadWork Nested;
			Nested.Execute = []() { return FRHIThreadWorkResult::Success(); };
			ObservedResult = Thread.Enqueue(Nested).Result;
			bNestedWorkRetained = static_cast<bool>(Nested.Execute);
			return FRHIThreadWorkResult::Success();
		};
		EXPECT_TRUE(Thread.EnqueueSynchronous(Work).IsCompleted());
		EXPECT_EQ(ERHIThreadEnqueueResult::SelfEnqueue, ObservedResult);
		EXPECT_TRUE(bNestedWorkRetained);
	}

	TEST(FRHIThreadTests, FailureWakesWaitersRejectsQueuedWorkAndDestroysPayloads)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		FThreadEvent FailureStarted;
		FThreadEvent ReleaseFailure;
		std::atomic<uint32> DestroyedCount = 0;

		struct FTrackedCapture
		{
			explicit FTrackedCapture(std::atomic<uint32>& InCount)
				: Count(InCount)
			{
			}
			~FTrackedCapture() { Count.fetch_add(1, std::memory_order::acq_rel); }
			std::atomic<uint32>& Count;
		};

		FRHIThreadWork Failing;
		Failing.Execute = [&]() {
			FailureStarted.Trigger();
			ReleaseFailure.Wait();
			return FRHIThreadWorkResult::Failure("fake executor failure");
		};
		const FRHIThreadSubmission FailingSubmission = Thread.Enqueue(Failing);
		ASSERT_TRUE(FailingSubmission.IsAccepted());
		ASSERT_TRUE(FailureStarted.WaitFor(1.0));

		FRHIThreadWork Queued;
		Queued.Execute = [Tracked = std::make_shared<FTrackedCapture>(DestroyedCount)]() {
			return FRHIThreadWorkResult::Success();
		};
		const FRHIThreadSubmission QueuedSubmission = Thread.Enqueue(Queued);
		ASSERT_TRUE(QueuedSubmission.IsAccepted());
		ReleaseFailure.Trigger();

		EXPECT_EQ(ERHIThreadWaitResult::Failed,
			Thread.WaitForSerial(QueuedSubmission.Serial));
		const FRHIThreadStats Stats = Thread.GetStats();
		EXPECT_EQ(FailingSubmission.Serial, Stats.FailedSerial);
		EXPECT_EQ("fake executor failure", Stats.FailureDiagnostic);
		EXPECT_EQ(1u, Stats.RejectedWorkCount);
		EXPECT_EQ(0u, Stats.OutstandingEntryCount);
		EXPECT_EQ(1u, DestroyedCount.load(std::memory_order::acquire));
	}

	TEST(FRHIThreadTests, FailedLaunchRestoresStoppedState)
	{
		FRHIThread Thread;
		FRHIThreadQueueLimits Limits;
		Limits.ThreadStackSize = 1;

		EXPECT_FALSE(Thread.Start(Limits));
		EXPECT_EQ(ERHIThreadAdmissionState::Stopped,
			Thread.GetStats().AdmissionState);
		EXPECT_EQ(nullptr, GRHIThread);
	}
} // namespace Durin
