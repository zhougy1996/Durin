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

namespace
{
	using namespace Durin;
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
		FModuleManager::Get().LoadModuleChecked("TextureBuild");
		auto Root = LaunchTask("TextureSaturationRoot", [] {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		auto Deferred = Then(Root, "FullDeferredMailbox", [] {}, {.EstimatedPayloadBytes = 32, .Target = ETaskTarget::GameThreadDeferred});
		ASSERT_TRUE(Deferred.IsValid());
		Testing::RegisterMountPointForTests("/AsyncPilot/", (Testing::GetTestWorkDirectory() / "LargeContent").generic_string() + "/");
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/AsyncPilot/LargeTexture", Path));
		DTexture2D* Texture = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
		FTextureSourceData Source;
		Source.Width = 256; Source.Height = 256; Source.SourceChannelCount = 4;
		Source.Format = ETextureSourceFormat::RGBA8;
		Source.Pixels.resize(256 * 256 * 4, std::byte{42});
		FTexture2DCompilationRequest Request;
		Request.Build.ImportedData = FTexture2DImportedData(Source);
		Request.Build.bPersistDerivedData = false;
		uint32 LargeCompleted = 0;
		std::string Error;
		ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(Request), Error, [&](FTexture2DCompilationResult Result) {
			EXPECT_TRUE(Result.Succeeded()); ++LargeCompleted;
		})) << Error;
		ASSERT_TRUE(WaitForTexture2DCompilation(*Texture, 10.0));
		EXPECT_EQ(1u, LargeCompleted);
		EXPECT_GT(GetTexture2DCompilationDiagnostic(*Texture).Metrics.ResultBytes, 64u);
		EXPECT_FALSE(Deferred.IsComplete());
		EXPECT_EQ(0u, GetTexture2DCompilationManagerDiagnostics().ActiveRecordCount);
		CancelTask(Deferred);
		WaitTask(Deferred);

		FThreadEvent StartedA, StartedB, Release;
		std::array<FTaskHandle, 8> Blockers;
		uint32 RejectedCompleted = 0;
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
		Blockers[0] = LaunchCancelableTask("HoldTextureWorkerA", [&](const FTaskCancellationToken& Token) { HoldWorker(StartedA, Token); });
		Blockers[1] = LaunchCancelableTask("HoldTextureWorkerB", [&](const FTaskCancellationToken& Token) { HoldWorker(StartedB, Token); });
		ASSERT_TRUE(StartedA.WaitFor(1.0));
		ASSERT_TRUE(StartedB.WaitFor(1.0));
		Testing::RegisterMountPointForTests("/AsyncPilot/", (Testing::GetTestWorkDirectory() / "ShutdownContent").generic_string() + "/");
		// Fill scheduler capacity independently of the manager's pending-request limit.
		// Requests accepted below must still receive failure/cancellation delivery.
		for (uint32 Index = 2; Index < Blockers.size(); ++Index)
		{
			Blockers[Index] = LaunchTask("FillTextureScheduler", [] {});
			ASSERT_TRUE(Blockers[Index].IsValid());
		}
		FTextureSourceData RejectedSource;
		RejectedSource.Width = 1; RejectedSource.Height = 1; RejectedSource.SourceChannelCount = 4;
		RejectedSource.Format = ETextureSourceFormat::RGBA8;
		RejectedSource.Pixels.resize(4);
		FTexture2DCompilationRequest RejectedRequest;
		RejectedRequest.Build.ImportedData = FTexture2DImportedData(RejectedSource);
		ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(RejectedRequest), Error,
			[&](FTexture2DCompilationResult Result) {
				EXPECT_EQ(ETexture2DCompilationStatus::Failed, Result.Status);
				++RejectedCompleted;
			})) << Error;
		FAssetCompilingManager::Get().ProcessAsyncTasks({});
		EXPECT_EQ(1u, RejectedCompleted);
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
			FTextureSourceData Source;
			Source.Width = 64; Source.Height = 64; Source.SourceChannelCount = 4;
			Source.Format = ETextureSourceFormat::RGBA8;
			Source.Pixels.resize(64 * 64 * 4, std::byte{42});
			FTexture2DCompilationRequest Request;
			Request.Build.ImportedData = FTexture2DImportedData(Source);
			Request.Build.bPersistDerivedData = false;
			std::string Error;
			ASSERT_TRUE(SubmitTexture2DCompilation(*Texture, std::move(Request), Error, [&, Index](FTexture2DCompilationResult Result) { EXPECT_EQ(ETexture2DCompilationStatus::Canceled, Result.Status); ++Completed[Index]; })) << Error;
		}
		// Two queued texture tasks plus the two running blockers leave four slots.
		for (uint32 Index = 2; Index < 6; ++Index)
		{
			Blockers[Index] = LaunchTask("FillTextureShutdown", [] {});
			ASSERT_TRUE(Blockers[Index].IsValid());
		}
		EXPECT_FALSE(LaunchTask("SaturatedTextureShutdown", [] {}).IsValid());
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(PendingTextures);
		// Exercise native cancellation without relying on a producer body or deferred callback.
		ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		ShutdownAssetCompilingManager();
		for (uint32 Count : Completed) EXPECT_EQ(1u, Count);
		Release.Trigger();
		for (const auto& Blocker : Blockers) if (Blocker.IsValid()) WaitTask(Blocker);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().NonterminalTaskCount != 0 && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().NonterminalTaskCount);
	}
}
