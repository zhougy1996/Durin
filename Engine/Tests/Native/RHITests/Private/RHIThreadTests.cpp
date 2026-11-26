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
		Thread.EnqueueSynchronous(std::move(Work));
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
				Thread.EnqueueSynchronous(std::move(Work));
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

		Thread.EnqueueSynchronous(std::move(Work));
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
		const uint64 FirstSubmission = Thread.Enqueue(std::move(First));
		ASSERT_TRUE(bool(FirstSubmission));
		ASSERT_TRUE(FirstStarted.WaitFor(1.0));

		FRHIThreadWork Second;
		Second.Execute = [&]() {
			std::lock_guard Lock(OrderMutex);
			Order.push_back(2);
		};
		const uint64 SecondSubmission = Thread.Enqueue(std::move(Second));
		ASSERT_TRUE(bool(SecondSubmission));
		EXPECT_FALSE(Thread.GetStats().CompletedSerial >= SecondSubmission);

		ReleaseFirst.Trigger();
		Thread.WaitForSerial(SecondSubmission);
		std::lock_guard Lock(OrderMutex);
		EXPECT_EQ((std::vector<int>{1, 2}), Order);
	}

	TEST(FRHIThreadTests, QueuePeaksIncludeRunningAndQueuedWork)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		FThreadEvent Started, Release;
		struct FReleaseOnExit { FThreadEvent& Event; ~FReleaseOnExit() { Event.Trigger(); } } ReleaseGuard{Release};
		FRHIThreadWork First;
		First.Execute = [&] { Started.Trigger(); EXPECT_TRUE(Release.WaitFor(5.0)); };
		ASSERT_TRUE(Thread.Enqueue(std::move(First)));
		ASSERT_TRUE(Started.WaitFor(1.0));
		constexpr uint64 PayloadPerEntry = 5ull * 1024 * 1024;
		for (int Index = 0; Index < 8; ++Index)
		{
			FRHIThreadWork Ordinary;
			Ordinary.BatchCount = 3;
			Ordinary.PayloadBytes = PayloadPerEntry;
			Ordinary.Execute = [] {};
			Thread.Enqueue(std::move(Ordinary));
		}
		const auto Stats = Thread.GetStats();
		EXPECT_EQ(Stats.PeakOutstandingEntryCount, 9u);
		EXPECT_EQ(Stats.PeakOutstandingBatchCount, 24u);
		EXPECT_EQ(Stats.PeakOutstandingPayloadBytes, 8 * PayloadPerEntry);
		Release.Trigger();
		Thread.Flush();
		EXPECT_EQ(Thread.GetStats().OutstandingEntryCount, 0u);
	}

	TEST(FRHIThreadTests, SubmissionAfterDrainIsFatal)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		Thread.BeginDrain();
		FRHIThreadWork Late;
		Late.Execute = []() {};
		EXPECT_DEATH(Thread.Enqueue(std::move(Late)), "RHI thread is closed");
	}

	TEST(FRHIThreadTests, SynchronousSubmissionRetiresCaptureExactlyOnce)
	{
		FRHIThread Thread;
		FRHIThreadTestGuard Guard(Thread);
		ASSERT_TRUE(Thread.Start());
		std::atomic<uint32> AcceptedDestroyedCount = 0;

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
		Thread.EnqueueSynchronous(std::move(Accepted));
		EXPECT_EQ(1u, AcceptedDestroyedCount.load(std::memory_order::acquire));
	}

	TEST(FRHIThreadTests, RunningWorkRetainsSerialCapacityAndOwnershipThroughDrain)
	{
		FRHIThread Thread;
		ASSERT_TRUE(Thread.Start());
		FThreadEvent Entered, Release;
		uint32 Calls = 0;
		auto Owner = std::make_shared<int>(1);
		std::weak_ptr<int> Lifetime = Owner;
		FRHIThreadWork Work;
		Work.BatchCount = 1;
		Work.Execute = [&, Owner] {
			++Calls;
			Entered.Trigger();
			Release.Wait();
		};
		const auto Accepted = Thread.Enqueue(std::move(Work));
		EXPECT_TRUE(bool(Accepted));
		Owner.reset();
		EXPECT_TRUE(Entered.WaitFor(5.0));
		Thread.BeginDrain();
		EXPECT_EQ(Thread.GetStats().CompletedSerial, 0u);
		EXPECT_EQ(Thread.GetStats().OutstandingBatchCount, 1u);
		EXPECT_FALSE(Lifetime.expired());
		Release.Trigger();
		Thread.WaitForSerial(Accepted);
		EXPECT_EQ(Thread.GetStats().CompletedSerial, Accepted);
		EXPECT_TRUE(Lifetime.expired());
		Thread.Stop();
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Thread.GetStats().OutstandingEntryCount, 0u);
		EXPECT_EQ(Thread.GetStats().OutstandingBatchCount, 0u);
	}

	TEST(FRHIThreadTests, RHIThreadSelfWaitIsFatal)
	{
		EXPECT_DEATH(([] {
			FRHIThread Thread;
			const bool Started = Thread.Start(); require(Started);
			FRHIThreadWork Work;
			Work.Execute = [&] { Thread.WaitForSerial(1); };
			Thread.EnqueueSynchronous(std::move(Work));
		}()), "SelfWait");
	}

	TEST(FRHIThreadTests, RHIThreadSelfEnqueueIsFatal)
	{
		EXPECT_DEATH(([] {
			FRHIThread Thread;
			const bool Started = Thread.Start(); require(Started);
			FRHIThreadWork Work;
			Work.Execute = [&] { FRHIThreadWork Nested; Nested.Execute = [] {}; Thread.Enqueue(std::move(Nested)); };
			Thread.EnqueueSynchronous(std::move(Work));
		}()), "SelfEnqueue");
	}

	TEST(FRHIThreadTests, FailedLaunchRestoresStoppedState)
	{
		FRHIThread Thread;

		EXPECT_FALSE(Thread.Start(1));
		EXPECT_EQ(ERHIThreadAdmissionState::Stopped,
			Thread.GetStats().AdmissionState);
		EXPECT_EQ(nullptr, GRHIThread);
	}

	TEST(FRHIThreadTests, StoppedThreadCannotRestart)
	{
		FRHIThread Thread;
		ASSERT_TRUE(Thread.Start());
		FRHIThreadWork Work;
		Work.Execute = [] {};
		const uint64 Serial = Thread.Enqueue(std::move(Work));
		ASSERT_NE(Serial, 0u);
		Thread.Stop();
		EXPECT_FALSE(Thread.Start());
		EXPECT_EQ(Thread.GetStats().CompletedSerial, Serial);
		Thread.WaitForSerial(Serial);
	}

	TEST(FRHIThreadTests, StoppedQueueRejectsSynchronousSubmissionAndUnsubmittedSerial)
	{
		FRHIThread Thread;
		ASSERT_TRUE(Thread.Start());
		Thread.Stop();
		FRHIThreadWork Work;
		Work.Execute = [] {};
		EXPECT_DEATH(Thread.EnqueueSynchronous(std::move(Work)), "RHI thread is closed");
		EXPECT_DEATH(Thread.WaitForSerial(1), "Wait for unsubmitted serial");
		Thread.WaitForSerial(0);
	}
} // namespace Durin
