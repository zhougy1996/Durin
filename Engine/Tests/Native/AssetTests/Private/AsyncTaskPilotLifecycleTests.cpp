#include "NativeAssetBuildTestSupport.h"
#include "Threading/TaskComposition.h"
#include <gtest/gtest.h>
#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "Modules/ModuleManager.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/Texture2DCompilationTestSupport.h"
#include "DerivedDataCache/DerivedDataCache.h"

namespace
{
	using namespace Durin;
	class FHeldTextureCache final : public DerivedData::ICache
	{
	public:
		mutable std::atomic<uint32> Reads = 0;
		mutable std::atomic<uint32> Writes = 0;
		mutable FThreadEvent Release;
		mutable FThreadEvent ReleaseWrites;
		bool bHoldWrites = false;
		auto Get(const DerivedData::FCacheGetRequest&) const -> DerivedData::FCacheGetResult override
		{
			++Reads;
			// Model an uninterruptible physical read; cancellation must retain its owner.
			Release.WaitFor(10.0);
			return std::optional<DerivedData::FCacheRecord>{};
		}
		auto Put(const DerivedData::FCachePutRequest&) const -> DerivedData::FCachePutResult override
		{
			++Writes;
			if (bHoldWrites) ReleaseWrites.WaitFor(10.0);
			return {};
		}
	};
	// The aggregate has terminal shutdown, so this target owns one full lifecycle.
	struct FPilotLifecycle
	{
		~FPilotLifecycle() { ShutdownAssetCompilingManager(); ShutdownTaskSystem(ETaskShutdownMode::Cancel); }
	};

	TEST(FAsyncTaskPilotLifecycleTests, LargeCommitAndSaturatedShutdownPreserveCompletion)
	{
		Testing::InitializeDObjectSystemForTests();
		Testing::FScopedMountRegistryFixture Mounts;
		ASSERT_TRUE(Mounts.IsValid());
		FPilotLifecycle Lifetime;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 2, .MaxNonterminalTasks = 8}));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor({.MaxQueuedEntries = 1, .MaxQueuedPayloadBytes = 64, .MaxPayloadBytesPerEntry = 64}));
		ASSERT_TRUE(InitializeAssetCompilingManager());
		FModuleManager::Get().LoadModuleChecked("TextureCompressor");
		auto Root = Tasks::LaunchTask("TextureSaturationRoot", [] {}).GetCompletion().GetTaskHandle();
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		auto Deferred = Tasks::Then(Tasks::FTaskCompletion(Root), Tasks::ETaskExecutor::GameThreadDeferred, {}, [] {}).GetCompletion().GetTaskHandle();
		ASSERT_TRUE(Deferred.IsValid());
		Testing::RegisterMountPointForTests("/AsyncPilot/", (Testing::GetTestWorkDirectory() / "LargeContent").generic_string() + "/");
		{
			auto Cache = std::make_shared<FHeldTextureCache>();
			Cache->bHoldWrites = true;
			FThreadEvent CallbackEntered, ReleaseCallback;
			AssetPrivate::SetTexture2DCompilationPhaseHookForTests([&](uint64, ETexture2DCompilationPhase Phase) {
				if (Phase == ETexture2DCompilationPhase::UploadPending) {
					CallbackEntered.Trigger();
					ReleaseCallback.WaitFor(10.0);
				}
			});
			AssetPrivate::SetTexture2DAsyncCacheForTests(Cache);
			struct FReleaseCache
			{
				std::shared_ptr<FHeldTextureCache> Cache;
				FThreadEvent& ReleaseCallback;
				~FReleaseCache() {
					Cache->Release.Trigger();
					Cache->ReleaseWrites.Trigger();
					ReleaseCallback.Trigger();
					FAssetCompilingManager::Get().FinishAllCompilation();
					AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
					AssetPrivate::SetTexture2DAsyncCacheForTests({});
				}
			} ReleaseCache{Cache, ReleaseCallback};
			std::array<DTexture2D*, 2> Owners{};
			std::array<uint32, 2> Deliveries{};
			for (uint32 Index = 0; Index < Owners.size(); ++Index)
			{
				FPackagePath OwnerPath;
				ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/HeldCache" + std::to_string(Index), OwnerPath));
				ASSERT_TRUE(CreatePackageLeafAssetForTesting(OwnerPath, Owners[Index]));
				auto Image = Image::FImage::TryCreate({.Width = 4, .Height = 4,
					.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64, std::byte{static_cast<uint8>(Index + 11)}));
				ASSERT_TRUE(Image);
				FTextureSource Source;
				ASSERT_TRUE(Source.Init2D(Image->GetView(), 4));
				FTexture2DCompilationRequest Request;
				Request.Build = MakeTexture2DBuildRequest(Source).value();
				Request.ResultApplication.SourceReplacement = Source;
				Request.Priority = ETexture2DCompilationPriority::Interactive;
				ASSERT_TRUE(SubmitTexture2DCompilation(*Owners[Index], std::move(Request), [&, Index](FTexture2DCompilationResult Result) {
					EXPECT_EQ(Index == 0 ? ETexture2DCompilationStatus::Canceled : ETexture2DCompilationStatus::Succeeded, Result.Status);
					++Deliveries[Index];
				}));
			}
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (Cache->Reads.load() < 2 && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
			ASSERT_EQ(2u, Cache->Reads.load());
			// Both owner slots are suspended in I/O; the two Core workers remain usable.
			auto Marker = Tasks::LaunchTask("TextureOwnerReleasedWorkers", [] { return 42; });
			const auto MarkerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!Marker.GetCompletion().IsReady() && std::chrono::steady_clock::now() < MarkerDeadline) std::this_thread::yield();
			ASSERT_TRUE(Marker.GetCompletion().IsReady());
			EXPECT_EQ(ETaskState::Succeeded, Marker.GetState());
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Owners[0]);
			FAssetCompilingManager::Get().ProcessAsyncTasks();
			EXPECT_EQ(2u, FAssetCompilingManager::Get().GetNumRemainingAssets());
			EXPECT_EQ(0u, Deliveries[0]);
			EXPECT_FALSE(WaitForTexture2DCompilation(*Owners[0], 0.0));
			Cache->Release.Trigger();
			const auto WriteDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (Cache->Writes.load() < 1 && std::chrono::steady_clock::now() < WriteDeadline) std::this_thread::yield();
			ASSERT_EQ(1u, Cache->Writes.load());
			EXPECT_FALSE(WaitForTexture2DCompilation(*Owners[1], 0.0));
			EXPECT_EQ(0u, Deliveries[1]);
			Cache->ReleaseWrites.Trigger();
			ASSERT_TRUE(CallbackEntered.WaitFor(5.0));
			// The diagnostic can be terminal before its callback/captures retire.
			EXPECT_EQ(ETexture2DCompilationPhase::UploadPending,
				AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Owners[1]).Phase);
			EXPECT_FALSE(WaitForTexture2DCompilation(*Owners[1], 0.0));
			EXPECT_EQ(0u, Deliveries[1]);
			ReleaseCallback.Trigger();
			ASSERT_TRUE(WaitForTexture2DCompilation(*Owners[1], 10.0));
			EXPECT_EQ(1u, Deliveries[1]);
			EXPECT_EQ(0u, Deliveries[0]);
			EXPECT_TRUE(HasPendingTexture2DCompilation(*Owners[0]));
			EXPECT_FALSE(Owners[0]->HasPlatformData());
			FAssetCompilingManager::Get().FinishAllCompilation();
			EXPECT_EQ(1u, Deliveries[0]);
			EXPECT_EQ(1u, Cache->Writes.load());
			EXPECT_EQ(0u, FAssetCompilingManager::Get().GetNumRemainingAssets());
			// A failing internal assembly/publication callback still produces a terminal result.
			AssetPrivate::SetTexture2DCompilationPhaseHookForTests([](uint64, ETexture2DCompilationPhase Phase) {
				if (Phase == ETexture2DCompilationPhase::UploadPending) throw std::runtime_error("Texture callback failure");
			});
			FTexture2DCompilationRequest FailingCallback;
			FailingCallback.Build = MakeTexture2DBuildRequest(Owners[1]->GetSource()).value();
			FailingCallback.Build.bPersistDerivedData = false;
			ASSERT_TRUE(SubmitTexture2DCompilation(*Owners[1], std::move(FailingCallback), [&](FTexture2DCompilationResult Result) {
				EXPECT_EQ(ETexture2DCompilationStatus::Failed, Result.Status);
				EXPECT_EQ(ETexture2DCompilationError::WorkerFailed, Result.Error.Code);
				++Deliveries[1];
			}));
			EXPECT_FALSE(WaitForTexture2DCompilation(*Owners[1], 10.0));
			EXPECT_EQ(2u, Deliveries[1]);
			EXPECT_EQ(0u, FAssetCompilingManager::Get().GetNumRemainingAssets());
		}
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/LargeTexture", Path));
		DTexture2D* Texture = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
		auto ImageResult1 = Image::FImage::TryCreate({.Width = 256, .Height = 256,
			.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(256 * 256 * 4, std::byte{42}));
		ASSERT_TRUE(ImageResult1);
		auto SourceImage = std::move(*ImageResult1);
		FTextureSource Source;
		ASSERT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
		FTexture2DCompilationRequest Request;
		Request.Build = Durin::MakeTexture2DBuildRequest(Source).value();
		Request.ResultApplication.SourceReplacement = Source;
		Request.Build.bPersistDerivedData = false;
		uint32 LargeCompleted = 0;
		ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(Request), [&](FTexture2DCompilationResult Result) {
			EXPECT_TRUE(Result.Succeeded()); ++LargeCompleted;
		}));
		ASSERT_TRUE(WaitForTexture2DCompilation(*Texture, 10.0));
		EXPECT_EQ(1u, LargeCompleted);
		EXPECT_GT(AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).Metrics.ResultBytes, 64u);
		EXPECT_FALSE(Deferred.IsComplete());
		EXPECT_EQ(0u, FAssetCompilingManager::Get().GetNumRemainingAssets());
		CancelTask(Deferred);
		WaitTask(Deferred);

		// Keep an accepted asynchronous build suspended when the owner and Core shut down.
		auto ShutdownCache = std::make_shared<FHeldTextureCache>();
		AssetPrivate::SetTexture2DAsyncCacheForTests(ShutdownCache);
		struct FReleaseShutdownCache
		{
			std::shared_ptr<FHeldTextureCache> Cache;
			~FReleaseShutdownCache() { Cache->Release.Trigger(); }
		} ReleaseShutdownCache{ShutdownCache};
		FPackagePath HeldPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/HeldShutdownTexture", HeldPath));
		DTexture2D* HeldTexture = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(HeldPath, HeldTexture));
		FTexture2DCompilationRequest HeldRequest;
		HeldRequest.Build = MakeTexture2DBuildRequest(Source).value();
		HeldRequest.ResultApplication.SourceReplacement = Source;
		uint32 HeldCompleted = 0;
		ASSERT_TRUE(SubmitTexture2DCompilation(*HeldTexture, std::move(HeldRequest), [&](FTexture2DCompilationResult Result) {
			EXPECT_EQ(ETexture2DCompilationStatus::Canceled, Result.Status);
			++HeldCompleted;
		}));
		const auto ReadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (ShutdownCache->Reads.load() == 0 && std::chrono::steady_clock::now() < ReadDeadline) std::this_thread::yield();
		ASSERT_EQ(1u, ShutdownCache->Reads.load());

		FThreadEvent StartedA, StartedB, Release;
		std::array<FTaskHandle, 8> Blockers;
		uint32 SaturatedCompleted = 0;
		std::array<uint32, 4> Completed{};
		std::array<DObject*, 4> PendingTextures{};
		auto HoldWorker = [&](FThreadEvent& Started, const FTaskCancellationToken& Token) {
			Started.Trigger();
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Token.IsCancellationRequested() && std::chrono::steady_clock::now() < Deadline)
				if (Release.WaitFor(0.001)) break;
		};
		struct FReleaseBlockers
		{
			FThreadEvent& Event;
			std::array<FTaskHandle, 8>& Tasks;
			~FReleaseBlockers() { Event.Trigger(); ShutdownAssetCompilingManager(); for (const auto& Task : Tasks) if (Task.IsValid()) WaitTask(Task); }
		} ReleaseBlockers{Release, Blockers};
		Blockers[0] = Tasks::LaunchTask("HoldTextureWorkerA", [&](const FTaskCancellationToken& Token) { HoldWorker(StartedA, Token); }).GetCompletion().GetTaskHandle();
		Blockers[1] = Tasks::LaunchTask("HoldTextureWorkerB", [&](const FTaskCancellationToken& Token) { HoldWorker(StartedB, Token); }).GetCompletion().GetTaskHandle();
		ASSERT_TRUE(StartedA.WaitFor(1.0));
		ASSERT_TRUE(StartedB.WaitFor(1.0));
		Testing::RegisterMountPointForTests("/AsyncPilot/", (Testing::GetTestWorkDirectory() / "ShutdownContent").generic_string() + "/");
		// Fill scheduler capacity independently of the manager's pending-request limit.
		// Requests accepted below must still receive failure/cancellation delivery.
		for (uint32 Index = 2; Index < Blockers.size(); ++Index)
		{
			Blockers[Index] = Tasks::LaunchTask("FillTextureScheduler", [] {}).GetCompletion().GetTaskHandle();
			ASSERT_TRUE(Blockers[Index].IsValid());
		}
		auto ImageResult2 = Image::FImage::TryCreate({.Width = 1, .Height = 1,
			.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(4));
		ASSERT_TRUE(ImageResult2);
		auto SaturatedSourceImage = std::move(*ImageResult2);
		FTextureSource SaturatedSource;
		ASSERT_TRUE(SaturatedSource.Init2D(SaturatedSourceImage.GetView(), 4));
		FTexture2DCompilationRequest SaturatedRequest;
		SaturatedRequest.Build = Durin::MakeTexture2DBuildRequest(SaturatedSource).value();
		SaturatedRequest.ResultApplication.SourceReplacement = SaturatedSource;
		ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(SaturatedRequest),
			[&](FTexture2DCompilationResult Result) {
				EXPECT_EQ(ETexture2DCompilationStatus::Canceled, Result.Status);
				++SaturatedCompleted;
			}));
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_EQ(0u, SaturatedCompleted);
		for (uint32 Index = 2; Index < Blockers.size(); ++Index)
		{
			CancelTask(Blockers[Index]);
			EXPECT_EQ(ETaskState::Canceled, WaitTask(Blockers[Index]).TaskState);
		}
		for (uint32 Index = 0; Index < 4; ++Index)
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/ShutdownTexture" + std::to_string(Index), Path));
			DTexture2D* Texture = nullptr;
			ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
			PendingTextures[Index] = Texture;
			auto ImageResult3 = Image::FImage::TryCreate({.Width = 64, .Height = 64,
				.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64 * 64 * 4, std::byte{42}));
			ASSERT_TRUE(ImageResult3);
			auto SourceImage = std::move(*ImageResult3);
			FTextureSource Source;
			ASSERT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
			FTexture2DCompilationRequest Request;
			Request.Build = Durin::MakeTexture2DBuildRequest(Source).value();
			Request.ResultApplication.SourceReplacement = Source;
			Request.Build.bPersistDerivedData = false;
			ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(Request), [&, Index](FTexture2DCompilationResult Result) { EXPECT_EQ(ETexture2DCompilationStatus::Canceled, Result.Status); ++Completed[Index]; }));
		}
		// Scheduler thresholds do not reject queued owner work.
		for (uint32 Index = 2; Index < 6; ++Index)
		{
			Blockers[Index] = Tasks::LaunchTask("FillTextureShutdown", [] {}).GetCompletion().GetTaskHandle();
			ASSERT_TRUE(Blockers[Index].IsValid());
		}
		EXPECT_TRUE(Tasks::LaunchTask("SaturatedTextureShutdown", [] {}).GetCompletion().GetTaskHandle().IsValid());
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(PendingTextures);
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*HeldTexture);
		EXPECT_EQ(0u, HeldCompleted);
		ShutdownCache->Release.Trigger();
		// Exercise native cancellation without relying on a producer body or deferred callback.
		ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		ShutdownAssetCompilingManager();
		EXPECT_EQ(1u, HeldCompleted);
		EXPECT_FALSE(HeldTexture->HasPlatformData());
		AssetPrivate::SetTexture2DAsyncCacheForTests({});
		for (uint32 Count : Completed) EXPECT_EQ(1u, Count);
		EXPECT_EQ(1u, SaturatedCompleted);
		Release.Trigger();
		for (const auto& Blocker : Blockers) if (Blocker.IsValid()) WaitTask(Blocker);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().NonterminalTaskCount != 0 && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().NonterminalTaskCount);
	}
}
