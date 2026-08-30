#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHICommandList.h"
#include "RHIThread.h"
#include "RenderResource.h"
#include "SceneViewState.h"
#include "RenderingThread.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	namespace
	{
		struct FRenderResourceEvents
		{
			int InitCount = 0;
			int ReleaseCount = 0;
			bool bCallbacksOnRenderingThread = true;
			bool bDestroyed = false;
			bool bDestroyedOnRenderingThread = false;
		};

		class FTestRenderResource final : public FRenderResource
		{
		public:
			explicit FTestRenderResource(
				std::shared_ptr<FRenderResourceEvents> InEvents)
				: Events(std::move(InEvents))
			{
			}

			~FTestRenderResource() override
			{
				Events->bDestroyed = true;
				Events->bDestroyedOnRenderingThread = IsInRenderingThread();
			}

			auto InitRHI(FRHICommandListBase&) -> void override
			{
				++Events->InitCount;
				Events->bCallbacksOnRenderingThread &= IsInRenderingThread();
			}

			auto ReleaseRHI() -> void override
			{
				++Events->ReleaseCount;
				Events->bCallbacksOnRenderingThread &= IsInRenderingThread();
			}

			auto GetFriendlyName() const -> std::string override
			{
				return "FTestRenderResource";
			}

		private:
			std::shared_ptr<FRenderResourceEvents> Events;
		};

		class FTestRHIResource final : public FRHIResource
		{
		public:
			explicit FTestRHIResource(std::atomic<bool>& InDestroyed)
				: FRHIResource(ERHIResourceType::Buffer)
				, Destroyed(InDestroyed)
			{
			}

			~FTestRHIResource() override
			{
				Destroyed.store(true, std::memory_order_release);
			}

		private:
			std::atomic<bool>& Destroyed;
		};

		class FRenderResourceLifecycleTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				if (!IsFNameInitialized()) FNameInit();
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				InitRenderingThread();
			}

			void TearDown() override
			{
				ShutdownTaskScheduler(false);
				FlushRenderingCommands();
				EXPECT_EQ(GetNumInitializedRenderResources(), 0u);
				EXPECT_EQ(GetNumPendingRenderResourceCleanup(), 0u);
				ShutdownRenderingThread();
			}
		};

		struct FRejectedAfterAdmissionCloseCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "RejectedAfterAdmissionClose";
			}
		};

		struct FAcceptedBeforeAdmissionCloseCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "AcceptedBeforeAdmissionClose";
			}
		};

		struct FAcceptedDuringSchedulerDrainCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "AcceptedDuringSchedulerDrain";
			}
		};

		struct FRejectedAfterIntegratedShutdownCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "RejectedAfterIntegratedShutdown";
			}
		};
	}

	TEST_F(FRenderResourceLifecycleTests,
		AsynchronousInitUpdateReleaseAndCleanupPreserveAffinity)
	{
		const auto Events = std::make_shared<FRenderResourceEvents>();
		auto Resource = std::make_unique<FTestRenderResource>(Events);
		FTestRenderResource* ResourceView = Resource.get();

#if DURIN_BUILD_DEBUG
		ResourceView->SetDebugOwner(FName("/Test/RenderResource"));
#endif
		ResourceView->BeginInit_GameThread();
		ResourceView->BeginUpdateRHI_GameThread();
#if DURIN_BUILD_DEBUG
		ENQUEUE_RENDER_COMMAND(ObserveResourceDiagnostics)(
			[ResourceView](FRHICommandListImmediate&) {
				EXPECT_EQ(ResourceView->GetDebugOwner(),
					FName("/Test/RenderResource"));
			});
#endif
		ResourceView->BeginRelease_GameThread();
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(Resource)));
		FlushRenderingCommands();

		EXPECT_EQ(Events->InitCount, 2);
		EXPECT_EQ(Events->ReleaseCount, 2);
		EXPECT_TRUE(Events->bCallbacksOnRenderingThread);
		EXPECT_TRUE(Events->bDestroyed);
		EXPECT_TRUE(Events->bDestroyedOnRenderingThread);
		EXPECT_EQ(GetNumInitializedRenderResources(), 0u);
		EXPECT_EQ(GetNumPendingRenderResourceCleanup(), 0u);
	}

	TEST_F(FRenderResourceLifecycleTests,
		DoubleOperationsAreIdempotentAndRegistryRemovalRemainsStable)
	{
		const auto FirstEvents = std::make_shared<FRenderResourceEvents>();
		const auto SecondEvents = std::make_shared<FRenderResourceEvents>();
		auto First = std::make_unique<FTestRenderResource>(FirstEvents);
		auto Second = std::make_unique<FTestRenderResource>(SecondEvents);
		FTestRenderResource* FirstView = First.get();
		FTestRenderResource* SecondView = Second.get();

		FirstView->BeginRelease_GameThread();
		FirstView->BeginInit_GameThread();
		FirstView->BeginInit_GameThread();
		SecondView->BeginInit_GameThread();
		FirstView->BeginRelease_GameThread();
		FirstView->BeginRelease_GameThread();
		SecondView->BeginRelease_GameThread();
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(First)));
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(Second)));
		FlushRenderingCommands();

		EXPECT_EQ(FirstEvents->InitCount, 1);
		EXPECT_EQ(FirstEvents->ReleaseCount, 1);
		EXPECT_EQ(SecondEvents->InitCount, 1);
		EXPECT_EQ(SecondEvents->ReleaseCount, 1);
		EXPECT_TRUE(FirstEvents->bDestroyedOnRenderingThread);
		EXPECT_TRUE(SecondEvents->bDestroyedOnRenderingThread);
		EXPECT_EQ(GetNumInitializedRenderResources(), 0u);
	}

	TEST_F(FRenderResourceLifecycleTests,
		AdmissionCloseDrainsAcceptedWorkRejectsNewWorkAndRestarts)
	{
		std::atomic<bool> bAcceptedCommandExecuted = false;
		const bool bAccepted =
			FRenderThreadCommandPipe::TryEnqueue<
				FAcceptedBeforeAdmissionCloseCommand>(
				[&bAcceptedCommandExecuted](FRHICommandListImmediate&) {
					bAcceptedCommandExecuted.store(
						true, std::memory_order_release);
				});
		EXPECT_TRUE(bAccepted);

		ShutdownRenderingThread();

		EXPECT_TRUE(bAcceptedCommandExecuted.load(
			std::memory_order_acquire));
		EXPECT_EQ(GetRenderCommandAdmissionState(),
			ERenderCommandAdmissionState::Stopped);
		EXPECT_EQ(GetNumPendingRenderCommands(), 0u);
		EXPECT_FALSE(
			FRenderThreadCommandPipe::TryEnqueue<
				FRejectedAfterAdmissionCloseCommand>(
				[](FRHICommandListImmediate&) {}));

		// Restore the fixture contract and prove the stopped pipe is reusable.
		InitRenderingThread();
		EXPECT_EQ(GetRenderCommandAdmissionState(),
			ERenderCommandAdmissionState::Running);
	}

	TEST_F(FRenderResourceLifecycleTests,
		SchedulerDrainCompletesBeforeRenderAdmissionCloses)
	{
		ShutdownTaskScheduler(false);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent ProbeStarted;
		std::atomic<bool> bRenderCommandAccepted = false;
		std::atomic<bool> bRenderCommandExecuted = false;
		std::atomic<bool> bTaskAdmissionRejected = false;
		FTaskHandle Probe = LaunchTask("RenderLifecycle.AdmissionProbe", [&]() {
			ProbeStarted.Trigger();
			while (IsTaskSchedulerRunning())
			{
				std::this_thread::yield();
			}
			bRenderCommandAccepted.store(
				FRenderThreadCommandPipe::TryEnqueue<
					FAcceptedDuringSchedulerDrainCommand>(
					[&bRenderCommandExecuted](FRHICommandListImmediate&) {
						bRenderCommandExecuted.store(
							true, std::memory_order_release);
					}),
				std::memory_order_release);
			bTaskAdmissionRejected.store(
				!LaunchTask("RenderLifecycle.RejectedAfterClose", []() {}).IsValid(),
				std::memory_order_release);
		});
		ASSERT_TRUE(ProbeStarted.WaitFor(1.0));

		ShutdownTaskScheduler(true);

		EXPECT_EQ(ETaskState::Succeeded, Probe.GetState());
		EXPECT_TRUE(bTaskAdmissionRejected.load(std::memory_order_acquire));
		EXPECT_TRUE(bRenderCommandAccepted.load(std::memory_order_acquire));
		FlushRenderingCommands();
		EXPECT_TRUE(bRenderCommandExecuted.load(std::memory_order_acquire));
		const FTaskSchedulerDiagnostics SchedulerDiagnostics =
			GetTaskSchedulerDiagnostics();
		EXPECT_FALSE(SchedulerDiagnostics.bRunning);
		EXPECT_EQ(0u, SchedulerDiagnostics.NonterminalTaskCount);
		EXPECT_GE(SchedulerDiagnostics.RejectedTaskCount, 1u);
		EXPECT_GE(SchedulerDiagnostics.RetainedTerminalHandleCount, 1u);

		ShutdownRenderingThread();
		EXPECT_EQ(ERenderCommandAdmissionState::Stopped,
			GetRenderCommandAdmissionState());
		EXPECT_EQ(0u, GetNumPendingRenderCommands());
		EXPECT_FALSE(FRenderThreadCommandPipe::TryEnqueue<
			FRejectedAfterIntegratedShutdownCommand>(
			[](FRHICommandListImmediate&) {}));

		InitRenderingThread();
	}

	TEST_F(FRenderResourceLifecycleTests,
		SchedulerDiscardCancelsAcceptedWorkAndRetainedHandlesSurviveRestart)
	{
		ShutdownTaskScheduler(false);
		ASSERT_TRUE(InitializeTaskScheduler(1));

		FThreadEvent RunningTaskStarted;
		FTaskHandle RunningTask = LaunchCancelableTask(
			"RenderLifecycle.RunningDuringDiscard",
			[&RunningTaskStarted](const FTaskCancellationToken& Token) {
				RunningTaskStarted.Trigger();
				while (!Token.IsCancellationRequested())
				{
					std::this_thread::yield();
				}
			});
		ASSERT_TRUE(RunningTaskStarted.WaitFor(1.0));
		FTaskHandle QueuedTask = LaunchTask(
			"RenderLifecycle.QueuedDuringDiscard", []() {});
		ASSERT_TRUE(QueuedTask.IsValid());

		ShutdownTaskScheduler(false);

		EXPECT_EQ(ETaskState::Canceled, RunningTask.GetState());
		EXPECT_EQ(ETaskState::Canceled, QueuedTask.GetState());
		const FTaskSchedulerDiagnostics DiscardDiagnostics =
			GetTaskSchedulerDiagnostics();
		EXPECT_FALSE(DiscardDiagnostics.bRunning);
		EXPECT_EQ(0u, DiscardDiagnostics.NonterminalTaskCount);
		EXPECT_EQ(0u, DiscardDiagnostics.ActiveWorkerCount);
		EXPECT_GE(DiscardDiagnostics.CanceledTaskCount, 2u);
		EXPECT_GE(DiscardDiagnostics.RetainedTerminalHandleCount, 2u);

		ASSERT_TRUE(InitializeTaskScheduler(1));
		FTaskHandle RestartedTask = LaunchTask(
			"RenderLifecycle.AfterRestart", []() {});
		ASSERT_TRUE(RestartedTask.IsValid());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(RestartedTask).TaskState);
		EXPECT_EQ(ETaskState::Canceled, RunningTask.GetState());
		EXPECT_EQ(ETaskState::Canceled, QueuedTask.GetState());
		ShutdownTaskScheduler(true);
	}

	TEST_F(FRenderResourceLifecycleTests,
		ShutdownAuditRejectsLiveRenderResources)
	{
		const auto Events = std::make_shared<FRenderResourceEvents>();
		auto Resource = std::make_unique<FTestRenderResource>(Events);
		FTestRenderResource* ResourceView = Resource.get();
		std::atomic<bool> bAuditAcceptedInvalidState = true;

		ResourceView->BeginInit_GameThread();
		ENQUEUE_RENDER_COMMAND(AuditLiveTestResource)(
			[&bAuditAcceptedInvalidState](FRHICommandListImmediate&) {
				bAuditAcceptedInvalidState.store(
					ValidateRenderResourceShutdown_RenderThread(),
					std::memory_order_release);
			});
		ResourceView->BeginRelease_GameThread();
		FlushRenderingCommands();

		EXPECT_FALSE(bAuditAcceptedInvalidState.load(
			std::memory_order_acquire));
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(Resource)));
		FlushRenderingCommands();
	}

	TEST_F(FRenderResourceLifecycleTests,
		ShutdownAuditRejectsPendingResourceCleanup)
	{
		const auto Events = std::make_shared<FRenderResourceEvents>();
		auto Resource = std::make_unique<FTestRenderResource>(Events);
		std::mutex GateMutex;
		std::condition_variable GateCV;
		bool bAuditCommandStarted = false;
		bool bRunAudit = false;
		std::atomic<bool> bAuditAcceptedInvalidState = true;

		ENQUEUE_RENDER_COMMAND(AuditPendingTestResourceCleanup)(
			[&](FRHICommandListImmediate&) {
				{
					std::unique_lock Lock(GateMutex);
					bAuditCommandStarted = true;
					GateCV.notify_one();
					GateCV.wait(Lock, [&bRunAudit]() {
						return bRunAudit;
					});
				}
				bAuditAcceptedInvalidState.store(
					ValidateRenderResourceShutdown_RenderThread(),
					std::memory_order_release);
			});

		{
			std::unique_lock Lock(GateMutex);
			GateCV.wait(Lock, [&bAuditCommandStarted]() {
				return bAuditCommandStarted;
			});
		}
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(Resource)));
		EXPECT_EQ(GetNumPendingRenderResourceCleanup(), 1u);
		{
			std::lock_guard Lock(GateMutex);
			bRunAudit = true;
		}
		GateCV.notify_one();
		FlushRenderingCommands();

		EXPECT_FALSE(bAuditAcceptedInvalidState.load(
			std::memory_order_acquire));
		EXPECT_TRUE(Events->bDestroyedOnRenderingThread);
	}

	TEST_F(FRenderResourceLifecycleTests,
		FinalShutdownDrainsPendingRHIDeletesBeforeStopping)
	{
		std::atomic<bool> bDestroyed = false;
		auto* Resource = new FTestRHIResource(bDestroyed);
		Resource->AddRef();
		Resource->Release();

		EXPECT_EQ(FRHIResource::GetNumPendingDeletes(), 1u);
		ShutdownRenderingThread();

		EXPECT_TRUE(bDestroyed.load(std::memory_order_acquire));
		EXPECT_EQ(FRHIResource::GetNumPendingDeletes(), 0u);
		EXPECT_EQ(GetRenderCommandAdmissionState(),
			ERenderCommandAdmissionState::Stopped);

		InitRenderingThread();
	}

	TEST_F(FRenderResourceLifecycleTests,
		DestroyingPendingFenceLeavesCallbackStateAlive)
	{
		FThreadEvent CommandStarted;
		FThreadEvent ReleaseCommand;
		ENQUEUE_RENDER_COMMAND(BlockFenceCallback)(
			[&](FRHICommandListImmediate&) {
				CommandStarted.Trigger();
				ReleaseCommand.Wait();
			});
		if (!CommandStarted.WaitFor(1.0))
		{
			ReleaseCommand.Trigger();
			FAIL() << "blocking render command did not start";
		}

		{
			FRenderCommandFence Fence;
			Fence.BeginFence();
		}

		ReleaseCommand.Trigger();
		FlushRenderingCommands();
	}

	TEST_F(FRenderResourceLifecycleTests,
		RejectedFenceCompletesWithoutStrandingWaiter)
	{
		ShutdownRenderingThread();
		FRenderCommandFence Fence;
		Fence.BeginFence(ERenderCommandFenceMode::RHIThread);

		EXPECT_TRUE(Fence.IsFenceComplete());
		Fence.Wait();
		InitRenderingThread();
	}

	TEST_F(FRenderResourceLifecycleTests,
		EndFramePacingStaysRenderOnlyWhileThreadsFlushWaitsExactRHIWork)
	{
		FRHIThread RHIThread;
		ASSERT_TRUE(RHIThread.Start());
		GCommandListExecutor.SetThreadedMode(RHIThread);
		FThreadEvent RHIWorkStarted;
		FThreadEvent ReleaseRHIWork;

		ENQUEUE_RENDER_COMMAND(DispatchBlockingRHIWork)(
			[&](FRHICommandListImmediate& RHICmdList) {
				RHICmdList.EnqueueLambda([&]() {
					RHIWorkStarted.Trigger();
					ReleaseRHIWork.Wait();
				});
				RHICmdList.ImmediateFlush(
					EImmediateFlushType::DispatchToRHIThread);
			});

		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		FFrameSync::Sync(FFrameSync::EFlushMode::EndFrame);
		EXPECT_TRUE(RHIWorkStarted.WaitFor(1.0));

		ReleaseRHIWork.Trigger();
		FFrameSync::Sync(FFrameSync::EFlushMode::Threads);
		GCommandListExecutor.SetInlineMode();
		RHIThread.Stop();
	}

#if DO_CHECK
	TEST_F(FRenderResourceLifecycleTests,
		FinalShutdownRejectsASecondInvocation)
	{
		EXPECT_DEATH_IF_SUPPORTED(
			{
				ShutdownRenderingThread();
				ShutdownRenderingThread();
			},
			"");
	}
#endif
}

static_assert(!std::is_copy_constructible_v<Durin::FSceneViewStateOwner>);
static_assert(!std::is_copy_assignable_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_nothrow_move_constructible_v<Durin::FSceneViewStateOwner>);
static_assert(std::is_nothrow_move_assignable_v<Durin::FSceneViewStateOwner>);

TEST(FSceneViewStatePublicContractTests, DefaultIdentityAndOwnerAreStateless)
{
	const Durin::FSceneViewStateId Id;
	Durin::FSceneViewStateOwner Owner;
	EXPECT_FALSE(Id.IsValid());
	EXPECT_FALSE(static_cast<bool>(Id));
	EXPECT_FALSE(static_cast<bool>(Owner));
	EXPECT_EQ(Owner.GetId(), Id);
	Owner.Reset();
}
