#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHICommandList.h"
#include "RenderResource.h"
#include "RenderingThread.h"

namespace Durin
{
	static_assert(std::is_base_of_v<FRHITexture, FRHITextureReference>);

	namespace
	{
		struct FTextureLifetimeEvents
		{
			std::atomic<bool> bTextureDestroyed = false;
			std::atomic<bool> bTextureDestroyedOnRenderingThread = false;
			std::atomic<bool> bResourceDestroyed = false;
			std::atomic<bool> bResourceDestroyedOnRenderingThread = false;
		};

		class FTestTexture final : public FRHITexture
		{
		public:
			explicit FTestTexture(std::shared_ptr<FTextureLifetimeEvents> InEvents)
				: Events(std::move(InEvents))
			{
			}

		protected:
			~FTestTexture() override
			{
				Events->bTextureDestroyedOnRenderingThread.store(
					IsInRenderingThread(), std::memory_order_release);
				Events->bTextureDestroyed.store(true, std::memory_order_release);
			}

		private:
			std::shared_ptr<FTextureLifetimeEvents> Events;
		};

		class FTestTextureResource final : public FTextureResource
		{
		public:
			FTestTextureResource(FTextureReference* InTextureReference,
				FTextureRHIRef InTexture,
				std::shared_ptr<FTextureLifetimeEvents> InEvents)
				: FTextureResource(InTextureReference)
				, PendingTexture(std::move(InTexture))
				, Events(std::move(InEvents))
			{
			}

			~FTestTextureResource() override
			{
				Events->bResourceDestroyedOnRenderingThread.store(
					IsInRenderingThread(), std::memory_order_release);
				Events->bResourceDestroyed.store(true, std::memory_order_release);
			}

			auto InitRHI(FRHICommandListBase&) -> void override
			{
				SetTextureRHI_RenderThread(std::move(PendingTexture));
				PublishTexture_RenderThread();
			}

		private:
			FTextureRHIRef PendingTexture;
			std::shared_ptr<FTextureLifetimeEvents> Events;
		};

		auto DrainDeferredRHIResources() -> void
		{
			ENQUEUE_RENDER_COMMAND(DrainTextureReferenceTestRHIResources)(
				[](FRHICommandListImmediate&) {
					for (;;)
					{
						std::vector<FRHIResource*> Resources;
						FRHIResource::GatherResourcesToDelete(Resources);
						if (Resources.empty()) break;
						FRHIResource::DeleteResources(Resources);
					}
				});
			FlushRenderingCommands();
		}

		auto ResolveTexture(const FRHITextureReferenceRef& Reference)
			-> FRHITexture*
		{
			FRHITexture* Result = nullptr;
			ENQUEUE_RENDER_COMMAND(ResolveTextureReferenceTest)(
				[Reference, &Result](FRHICommandListImmediate&) {
					Result = Reference
						? Reference->GetReferencedTexture_RenderThread()
						: nullptr;
				});
			FlushRenderingCommands();
			return Result;
		}

		class FTextureReferenceTests : public testing::Test
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
				DrainDeferredRHIResources();
				EXPECT_EQ(GetNumInitializedRenderResources(), 0u);
				EXPECT_EQ(GetNumPendingRenderResourceCleanup(), 0u);
				ShutdownRenderingThread();
			}
		};
	}

	TEST_F(FTextureReferenceTests,
		CopiedReferenceTracksReplacementFallbackAndConcreteRetirement)
	{
		const auto FallbackEvents = std::make_shared<FTextureLifetimeEvents>();
		const auto OldEvents = std::make_shared<FTextureLifetimeEvents>();
		const auto NewEvents = std::make_shared<FTextureLifetimeEvents>();
		FTextureRHIRef Fallback(new FTestTexture(FallbackEvents));
		FTextureRHIRef OldTexture(new FTestTexture(OldEvents));
		FTextureRHIRef NewTexture(new FTestTexture(NewEvents));
		FTextureReference Reference(Fallback);
		FRHITextureReferenceRef ConsumerReference =
			Reference.GetTextureReferenceRHI();
		ASSERT_NE(ConsumerReference, nullptr);
		EXPECT_EQ(ConsumerReference->GetResourceType(),
			ERHIResourceType::TextureReference);

		Reference.BeginInit_GameThread();
		FlushRenderingCommands();
		EXPECT_EQ(ResolveTexture(ConsumerReference), Fallback.GetReference());
		ENQUEUE_RENDER_COMMAND(ClearTextureReferenceTest)(
			[&Reference](FRHICommandListImmediate&) {
				Reference.Clear_RenderThread();
			});
		FlushRenderingCommands();
		EXPECT_EQ(ResolveTexture(ConsumerReference), nullptr);
		ENQUEUE_RENDER_COMMAND(ResetTextureReferenceFallbackTest)(
			[&Reference](FRHICommandListImmediate&) {
				Reference.ResetToFallback_RenderThread();
			});
		FlushRenderingCommands();
		EXPECT_EQ(ResolveTexture(ConsumerReference), Fallback.GetReference());

		auto OldResource = std::make_unique<FTestTextureResource>(
			&Reference, OldTexture, OldEvents);
		FTestTextureResource* OldResourceView = OldResource.get();
		OldResourceView->BeginInit_GameThread();
		FlushRenderingCommands();
		EXPECT_EQ(ResolveTexture(ConsumerReference), OldTexture.GetReference());

		auto NewResource = std::make_unique<FTestTextureResource>(
			&Reference, NewTexture, NewEvents);
		FTestTextureResource* NewResourceView = NewResource.get();
		NewResourceView->BeginInit_GameThread();
		OldResourceView->BeginRelease_GameThread();
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(OldResource)));
		FlushRenderingCommands();

		EXPECT_EQ(ResolveTexture(ConsumerReference), NewTexture.GetReference());
		EXPECT_TRUE(OldEvents->bResourceDestroyed.load(std::memory_order_acquire));
		EXPECT_TRUE(OldEvents->bResourceDestroyedOnRenderingThread.load(
			std::memory_order_acquire));

		NewResourceView->BeginRelease_GameThread();
		BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup(std::move(NewResource)));
		Reference.BeginRelease_GameThread();
		FlushRenderingCommands();

		EXPECT_EQ(ResolveTexture(ConsumerReference), Fallback.GetReference());
		EXPECT_TRUE(NewEvents->bResourceDestroyed.load(std::memory_order_acquire));
		EXPECT_TRUE(NewEvents->bResourceDestroyedOnRenderingThread.load(
			std::memory_order_acquire));
	}

	TEST_F(FTextureReferenceTests,
		FinalCountedReferenceReleaseUsesDeferredDeletionFromSupportedThreads)
	{
		enum class EReleaseThread
		{
			Game,
			Worker,
			Render,
		};

		for (const EReleaseThread ReleaseThread : {
			EReleaseThread::Game,
			EReleaseThread::Worker,
			EReleaseThread::Render})
		{
			const auto Events = std::make_shared<FTextureLifetimeEvents>();
			FTextureRHIRef Fallback(new FTestTexture(Events));
			auto Reference = std::make_unique<FTextureReference>(Fallback);
			Reference->BeginInit_GameThread();
			FlushRenderingCommands();
			FRHITextureReferenceRef ConsumerReference =
				Reference->GetTextureReferenceRHI();

			Reference->BeginRelease_GameThread();
			FlushRenderingCommands();
			Reference.reset();
			Fallback = nullptr;
			bool bDestroyedDuringFinalRelease = true;

			if (ReleaseThread == EReleaseThread::Game)
			{
				ConsumerReference = nullptr;
				bDestroyedDuringFinalRelease =
					Events->bTextureDestroyed.load(std::memory_order_acquire);
			}
			else if (ReleaseThread == EReleaseThread::Worker)
			{
				std::thread Worker(
					[ReferenceToRelease = std::move(ConsumerReference),
						Events, &bDestroyedDuringFinalRelease]() mutable {
						ReferenceToRelease = nullptr;
						bDestroyedDuringFinalRelease =
							Events->bTextureDestroyed.load(
								std::memory_order_acquire);
					});
				Worker.join();
			}
			else
			{
				ENQUEUE_RENDER_COMMAND(ReleaseTextureReferenceOnRenderThread)(
					[ReferenceToRelease = std::move(ConsumerReference),
						Events, &bDestroyedDuringFinalRelease](
							FRHICommandListImmediate&) mutable {
						ReferenceToRelease = nullptr;
						bDestroyedDuringFinalRelease =
							Events->bTextureDestroyed.load(
								std::memory_order_acquire);
					});
				FlushRenderingCommands();
			}

			EXPECT_FALSE(bDestroyedDuringFinalRelease);
			DrainDeferredRHIResources();
			EXPECT_TRUE(Events->bTextureDestroyed.load(
				std::memory_order_acquire));
			EXPECT_TRUE(Events->bTextureDestroyedOnRenderingThread.load(
				std::memory_order_acquire));
		}
	}
}
