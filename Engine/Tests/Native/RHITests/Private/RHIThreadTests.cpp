#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHIThread.h"
#include "Threading/Runnable.h"
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

		class FGameThreadRegistrationGuard
		{
		public:
			FGameThreadRegistrationGuard()
				: PreviousId(GGameThreadId), bPreviouslyInitialized(GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
			~FGameThreadRegistrationGuard()
			{
				GGameThreadId = PreviousId;
				GIsGameThreadIdInitialized = bPreviouslyInitialized;
			}
		private:
			uint32 PreviousId;
			bool bPreviouslyInitialized;
		};

		class FExecutionThreadProbe final : public FRunnable
		{
		public:
			auto Run() -> uint32 override
			{
				if (bWaitForPublication) Published.Wait();
				bAllowed = IsInRHIExecutionThread();
				bRHIRole = IsInRHIThread();
				if (bAllowed) CheckRHIExecutionThread();
				return 0;
			}
			bool bAllowed = false;
			bool bWaitForPublication = false;
			FThreadEvent Published;
			bool bRHIRole = false;
		};
	}

	TEST(FRHIThreadTests, InlineExecutionUsesRenderingThreadOrStoppedRenderingGameThread)
	{
		ASSERT_EQ(GRHIThread, nullptr);
		FGameThreadRegistrationGuard Registration;
		EXPECT_TRUE(IsInRHIExecutionThread());
		CheckRHIExecutionThread();
		EXPECT_FALSE(IsInRHIThread());
		GIsGameThreadIdInitialized = false;
		EXPECT_FALSE(IsInRHIExecutionThread());
		GIsGameThreadIdInitialized = true;

		for (const auto Role : {EThreadRole::Unknown, EThreadRole::GameThread,
			EThreadRole::RenderingThread, EThreadRole::RHIThread,
			EThreadRole::WorkerThread, EThreadRole::IOThread})
		{
			FExecutionThreadProbe Probe;
			Probe.bWaitForPublication = Role == EThreadRole::RenderingThread;
			std::unique_ptr<FRunnableThread> Thread(FRunnableThread::Create(
				&Probe, "ExecutionProbe", 0, EThreadPriority::Normal, Role));
			ASSERT_TRUE(Thread);
			if (Probe.bWaitForPublication)
			{
				GRenderingThread = Thread.get();
				EXPECT_FALSE(IsInRHIExecutionThread());
				Probe.Published.Trigger();
			}
			Thread->WaitForCompletion();
			if (Probe.bWaitForPublication) GRenderingThread = nullptr;
			EXPECT_EQ(Probe.bAllowed, Role == EThreadRole::RenderingThread);
			EXPECT_TRUE(IsInRHIExecutionThread());
		}
	}

	TEST(FRHIThreadTests, DedicatedExecutionRequiresExactOwnerAndRestoresGameFallbackAfterStop)
	{
		FGameThreadRegistrationGuard Registration;
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		EXPECT_FALSE(IsInRHIExecutionThread());
		bool bOwnerAllowed = false;
		FRHIThreadWork Work;
		Work.Execute = [&] {
			bOwnerAllowed = IsInRHIExecutionThread();
			CheckRHIExecutionThread();
		};
		ASSERT_TRUE(Thread.EnqueueSynchronous(Work).IsCompleted());
		EXPECT_TRUE(bOwnerAllowed);
		for (const auto Role : {EThreadRole::Unknown, EThreadRole::RenderingThread,
			EThreadRole::RHIThread, EThreadRole::WorkerThread, EThreadRole::IOThread})
		{
			FExecutionThreadProbe Probe;
			std::unique_ptr<FRunnableThread> Other(FRunnableThread::Create(
				&Probe, "NonOwnerProbe", 0, EThreadPriority::Normal, Role));
			ASSERT_TRUE(Other);
			Other->WaitForCompletion();
			EXPECT_FALSE(Probe.bAllowed);
			EXPECT_EQ(Probe.bRHIRole, Role == EThreadRole::RHIThread);
		}
		Thread.Stop();
		EXPECT_TRUE(IsInRHIExecutionThread());
	}

	TEST(FRHIThreadDeathTest, UnhandledWorkExceptionsTerminate)
	{
		for (const bool bUnknown : {false, true})
		{
			EXPECT_DEATH(([] (bool bUnknown) {
				FRHIThread Thread;
				if (!Thread.Start()) std::exit(0);
				FRHIThreadWork Work;
				Work.Execute = [bUnknown] {
					if (bUnknown) throw 42;
					throw std::runtime_error("unhandled RHI work failure");
				};
				Thread.EnqueueSynchronous(Work);
			}(bUnknown)), "");
		}
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
		};
		const FRHIThreadSubmission FirstSubmission = Thread.Enqueue(First);
		ASSERT_TRUE(FirstSubmission.IsAccepted());
		ASSERT_TRUE(FirstStarted.WaitFor(1.0));

		FRHIThreadWork Second;
		Second.Execute = [&]() {
			std::lock_guard Lock(OrderMutex);
			Order.push_back(2);
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

	TEST(FRHIThreadTests, ThreeFrameLimitAllowsOverlapAndBlocksOnlyAtCapacity)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		FThreadEvent Started, Release;
		struct FReleaseOnExit { FThreadEvent& Event; ~FReleaseOnExit() { Event.Trigger(); } } ReleaseGuard{Release};
		FRHIThreadWork First;
		First.FrameCount = 1;
		First.Execute = [&] { Started.Trigger(); Release.Wait(); };
		ASSERT_TRUE(Thread.Enqueue(First).IsAccepted());
		ASSERT_TRUE(Started.WaitFor(1.0));
		for (int Index = 0; Index < 2; ++Index)
		{
			FRHIThreadWork Next;
			Next.FrameCount = 1;
			Next.Execute = [] {};
			EXPECT_TRUE(Thread.Enqueue(Next).IsAccepted());
		}
		EXPECT_EQ(Thread.GetStats().OutstandingFrameCount, 3u);
		EXPECT_EQ(Thread.GetStats().BackpressureWaitCount, 0u);
		std::atomic<bool> Accepted = false;
		std::thread Producer([&] {
			FRHIThreadWork Fourth;
			Fourth.FrameCount = 1;
			Fourth.Execute = [] {};
			Accepted = Thread.Enqueue(Fourth).IsAccepted();
		});
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!Thread.GetStats().FramePressureWaitCount && std::chrono::steady_clock::now() < Deadline)
			std::this_thread::yield();
		EXPECT_EQ(Thread.GetStats().FramePressureWaitCount, 1u);
		EXPECT_FALSE(Accepted.load());
		Release.Trigger();
		Producer.join();
		EXPECT_TRUE(Accepted.load());
		EXPECT_EQ(Thread.Flush(), ERHIThreadWaitResult::Completed);
		EXPECT_EQ(Thread.GetStats().OutstandingFrameCount, 0u);
		EXPECT_EQ(Thread.GetStats().PeakOutstandingFrameCount, 3u);
		EXPECT_GT(Thread.GetStats().FramePressureWaitNanoseconds, 0u);
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
		Second.Execute = []() {};
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

	TEST(FRHIThreadTests, OversizedPayloadRunsAloneAndBlocksLaterWork)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		FRHIThreadQueueLimits Limits;
		Limits.MaxEntries = 2;
		Limits.MaxBatches = 2;
		Limits.MaxPayloadBytes = 64;
		ASSERT_TRUE(Thread.Start(Limits));
		FThreadEvent FirstStarted;
		FThreadEvent ReleaseFirst;

		FRHIThreadWork First;
		First.BatchCount = 1;
		First.PayloadBytes = 128;
		First.Execute = [&]() {
			FirstStarted.Trigger();
			ReleaseFirst.Wait();
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
		Second.Execute = []() {};
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
		EXPECT_EQ(Stats.PeakOutstandingPayloadBytes, 128u);
	}

	TEST(FRHIThreadTests, DrainRejectsLateAndOversizedWorkWithoutMovingPayload)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		FRHIThreadQueueLimits Limits;
		Limits.MaxPayloadBytes = 8;
		ASSERT_TRUE(Thread.Start(Limits));

		FRHIThreadWork Oversized;
		Oversized.BatchCount = Limits.MaxBatches + 1;
		Oversized.Execute = []() {};
		EXPECT_EQ(ERHIThreadEnqueueResult::Oversized,
			Thread.Enqueue(Oversized).Result);
		EXPECT_TRUE(static_cast<bool>(Oversized.Execute));

		Thread.BeginDrain();
		FRHIThreadWork Late;
		Late.Execute = []() {};
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
		};
		ASSERT_TRUE(Thread.Enqueue(First).IsAccepted());
		ASSERT_TRUE(FirstStarted.WaitFor(1.0));

		ERHIThreadEnqueueResult BlockedResult =
			ERHIThreadEnqueueResult::Accepted;
		FRHIThreadWork Blocked;
		Blocked.BatchCount = 1;
		Blocked.PayloadBytes = 64;
		Blocked.Execute = []() {};
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
		};
		EXPECT_TRUE(Thread.EnqueueSynchronous(Accepted).IsCompleted());
		EXPECT_EQ(1u, AcceptedDestroyedCount.load(std::memory_order::acquire));

		FRHIThreadWork Rejected;
		Rejected.BatchCount = Limits.MaxBatches + 1;
		Rejected.Execute = [Tracked = std::make_shared<FTrackedCapture>(RejectedDestroyedCount)]() {
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
			Nested.Execute = []() {};
			ObservedResult = Thread.Enqueue(Nested).Result;
			bNestedWorkRetained = static_cast<bool>(Nested.Execute);
		};
		EXPECT_TRUE(Thread.EnqueueSynchronous(Work).IsCompleted());
		EXPECT_EQ(ERHIThreadEnqueueResult::SelfEnqueue, ObservedResult);
		EXPECT_TRUE(bNestedWorkRetained);
	}

	TEST(FRHIThreadTests, ExternalFailureWakesWaitersRejectsQueuedWorkAndDestroysPayloads)
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
		Failing.FrameCount = 1;
		Failing.Execute = [&]() {
			FailureStarted.Trigger();
			ReleaseFailure.Wait();
			Thread.ReportExternalFailure();
		};
		const FRHIThreadSubmission FailingSubmission = Thread.Enqueue(Failing);
		ASSERT_TRUE(FailingSubmission.IsAccepted());
		ASSERT_TRUE(FailureStarted.WaitFor(1.0));

		FRHIThreadWork Queued;
		Queued.Execute = [Tracked = std::make_shared<FTrackedCapture>(DestroyedCount)]() {
		};
		Queued.FrameCount = 1;
		const FRHIThreadSubmission QueuedSubmission = Thread.Enqueue(Queued);
		ASSERT_TRUE(QueuedSubmission.IsAccepted());
		ReleaseFailure.Trigger();

		EXPECT_EQ(ERHIThreadWaitResult::Failed,
			Thread.WaitForSerial(QueuedSubmission.Serial));
		Thread.Stop();
		const FRHIThreadStats Stats = Thread.GetStats();
		EXPECT_EQ(FailingSubmission.Serial, Stats.FailedSerial);
		EXPECT_EQ(1u, Stats.RejectedWorkCount);
		EXPECT_EQ(0u, Stats.OutstandingEntryCount);
		EXPECT_EQ(0u, Stats.OutstandingFrameCount);
		EXPECT_EQ(1u, DestroyedCount.load(std::memory_order::acquire));
	}

	TEST(FRHIThreadTests, ExternalFailurePreservesFailedSerialOnExecutionAndRejection)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		FRHIThreadWork Work;
		Work.Execute = [&Thread]() {
			Thread.ReportExternalFailure();
		};
		const auto Failed = Thread.EnqueueSynchronous(Work);
		EXPECT_EQ(ERHIThreadWaitResult::Failed, Failed.WaitResult);
		const auto FailedSerial = Thread.GetStats().FailedSerial;
		EXPECT_EQ(Failed.Submission.Serial, FailedSerial);
		Work.Execute = []() {};
		const auto Rejected = Thread.EnqueueSynchronous(Work);
		EXPECT_EQ(ERHIThreadEnqueueResult::Failed, Rejected.Submission.Result);
		EXPECT_EQ(FailedSerial, Thread.GetStats().FailedSerial);
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

	TEST(FRHIThreadTests, ExternalFailureWakesWaitersBeforeRunningWorkReturns)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		FThreadEvent Started, Release;
		FRHIThreadWork Running;
		Running.FrameCount = 1;
		Running.Execute = [&] { Started.Trigger(); Release.Wait(); };
		const auto First = Thread.Enqueue(Running);
		EXPECT_TRUE(Started.WaitFor(1.0));
		std::atomic<bool> Replayed = false;
		FRHIThreadWork Queued;
		Queued.BatchCount = 1;
		Queued.PayloadBytes = 512;
		Queued.FrameCount = 1;
		Queued.Execute = [&] { Replayed = true; };
		const auto Second = Thread.Enqueue(Queued);
		Thread.ReportExternalFailure();
		EXPECT_EQ(Thread.WaitForSerial(First.Serial), ERHIThreadWaitResult::Failed);
		EXPECT_EQ(Thread.WaitForSerial(Second.Serial), ERHIThreadWaitResult::Failed);
		EXPECT_EQ(Thread.GetStats().OutstandingEntryCount, 1u);
		EXPECT_EQ(Thread.GetStats().OutstandingFrameCount, 1u);
		EXPECT_EQ(Thread.GetStats().OutstandingPayloadBytes, 0u);
		Release.Trigger();
		Thread.Stop();
		EXPECT_EQ(Thread.GetStats().CompletedSerial, 0u);
		EXPECT_EQ(Thread.GetStats().OutstandingEntryCount, 0u);
		EXPECT_EQ(Thread.GetStats().OutstandingFrameCount, 0u);
		EXPECT_FALSE(Replayed.load());
	}

	TEST(FRHIThreadTests, ExternalFailureBeforeAdmissionRejectsNewWork)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		Thread.ReportExternalFailure();
		FRHIThreadWork Work;
		Work.Execute = [] {};
		EXPECT_EQ(Thread.Enqueue(Work).Result, ERHIThreadEnqueueResult::Failed);
		EXPECT_TRUE(Work.Execute);
	}
} // namespace Durin
