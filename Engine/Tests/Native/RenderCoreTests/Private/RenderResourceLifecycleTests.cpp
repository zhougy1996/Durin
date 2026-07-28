#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHICommandList.h"
#include "RenderResource.h"
#include "RenderingThread.h"

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
			explicit FTestRenderResource(std::shared_ptr<FRenderResourceEvents> InEvents,
				EInitPhase InitPhase = EInitPhase::Default)
				: FRenderResource(InitPhase)
				, Events(std::move(InEvents))
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

		class FRenderResourceLifecycleTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				InitRenderingThread();
			}

			void TearDown() override
			{
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
	}

	TEST_F(FRenderResourceLifecycleTests,
		AsynchronousInitUpdateReleaseAndCleanupPreserveAffinity)
	{
		const auto Events = std::make_shared<FRenderResourceEvents>();
		auto Resource = std::make_unique<FTestRenderResource>(Events);
		FTestRenderResource* ResourceView = Resource.get();

		BeginInitResource(ResourceView);
		BeginUpdateResourceRHI(ResourceView);
		BeginReleaseResource(ResourceView);
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

		BeginReleaseResource(FirstView);
		BeginInitResource(FirstView);
		BeginInitResource(FirstView);
		BeginInitResource(SecondView);
		BeginReleaseResource(FirstView);
		BeginReleaseResource(FirstView);
		BeginReleaseResource(SecondView);
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
		GlobalRHIResetReleasesAndReinitializesRegisteredResources)
	{
		const auto Events = std::make_shared<FRenderResourceEvents>();
		auto Resource = std::make_unique<FTestRenderResource>(
			Events, FRenderResource::EInitPhase::Pre);
		FTestRenderResource* ResourceView = Resource.get();

		BeginInitResource(ResourceView);
		ENQUEUE_RENDER_COMMAND(ResetAllTestResources)(
			[](FRHICommandListImmediate& RHICmdList) {
				FRenderResource::ReleaseRHIForAllResources();
				FRenderResource::InitPreRHIResources();
				FRenderResource::InitRHIForAllResources(RHICmdList);
			});
		BeginReleaseResource(ResourceView);
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(Resource)));
		FlushRenderingCommands();

		EXPECT_EQ(Events->InitCount, 2);
		EXPECT_EQ(Events->ReleaseCount, 2);
		EXPECT_TRUE(Events->bCallbacksOnRenderingThread);
		EXPECT_TRUE(Events->bDestroyedOnRenderingThread);
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

		FinalizeRenderingThreadBeforeRHIExit();

		EXPECT_TRUE(bAcceptedCommandExecuted.load(
			std::memory_order_acquire));
		EXPECT_EQ(GetRenderCommandAdmissionState(),
			ERenderCommandAdmissionState::Draining);
		EXPECT_EQ(GetNumPendingRenderCommands(), 0u);
		EXPECT_FALSE(
			FRenderThreadCommandPipe::TryEnqueue<
				FRejectedAfterAdmissionCloseCommand>(
				[](FRHICommandListImmediate&) {}));

		ShutdownRenderingThread();
		EXPECT_EQ(GetRenderCommandAdmissionState(),
			ERenderCommandAdmissionState::Stopped);
		EXPECT_EQ(GetNumPendingRenderCommands(), 0u);

		// Restore the fixture contract and prove the stopped pipe is reusable.
		InitRenderingThread();
		EXPECT_EQ(GetRenderCommandAdmissionState(),
			ERenderCommandAdmissionState::Running);
	}
}
