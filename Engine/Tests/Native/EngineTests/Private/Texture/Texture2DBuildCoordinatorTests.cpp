#include "TextureTestSupport.h"

#include "Misc/FileHelper.h"
#include "Asset/AssetCompilingManager.h"
#include "Modules/ModuleManager.h"
#include "Texture/Texture2DAuthoringCoordinator.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

namespace
{
	auto TransparentPngData() -> std::span<const std::byte>
	{
		return std::as_bytes(std::span{TransparentPngBytes});
	}

	auto IsTerminal(Durin::Asset::Build::ETexture2DBuildPhase Phase) -> bool
	{
		return Phase == Durin::Asset::Build::ETexture2DBuildPhase::UploadPending
			|| Phase == Durin::Asset::Build::ETexture2DBuildPhase::Failed
			|| Phase == Durin::Asset::Build::ETexture2DBuildPhase::Cancelled;
	}

	auto MakeCoordinatorRequest(
		std::span<const std::byte> Bytes,
		std::string Identity,
		Durin::Asset::Build::ETexture2DBuildPriority Priority =
			Durin::Asset::Build::ETexture2DBuildPriority::Background)
			-> Durin::Asset::Build::FTexture2DQueuedBuildRequest
	{
		Durin::FTextureSourceData SourceData;
		std::string Error;
		if (!Durin::AssetForge::Builtins::TranslateTexture2DSource(
			Bytes, SourceData, Error)) return {};
		const Durin::FXxHash128 SourceHash = Durin::FXxHash128::HashBuffer(Bytes);
		return {
			.AssetIdentity = std::move(Identity),
			.SourcePath = {.Path = "/TextureImportTests/Coordinator.png"},
			.SourceData = std::move(SourceData),
			.SourceHash = SourceHash,
			.Settings = {
				.Usage = Durin::ETextureUsage::Color,
				.bSRGB = true,
				.CompressionQuality = Durin::ETextureCompressionQuality::Low},
			.Generation = 1,
			.EstimatedWidth = 2,
			.EstimatedHeight = 1,
			.Priority = Priority,
			.bPersistDerivedData = false};
	}

	auto WaitForTerminalCount(
		std::mutex& Mutex,
		std::condition_variable& Condition,
		const uint32& TerminalCount,
		uint32 Expected) -> bool
	{
		std::unique_lock Lock(Mutex);
		return Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return TerminalCount >= Expected;
		});
	}

}

TEST(FBuildRecipeModuleTests, GeometryLifecycleDoesNotAddAnEmptyCompilationDomain)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModuleChecked("GeometryBuild");
	ASSERT_TRUE(RestartTextureCompilingManager({.MaxWorkers = 1}));
	const Durin::FAssetCompilingManagerDiagnostics Running =
		Durin::FAssetCompilingManager::Get().GetDiagnostics();
	EXPECT_EQ(Running.ManagerCount, 2u);
	EXPECT_TRUE(Running.bAcceptingRequests);
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	Durin::Asset::Build::ShutdownTextureBuildService();
	EXPECT_EQ(Durin::FAssetCompilingManager::Get().GetDiagnostics().ManagerCount, 1u);
	const auto GeometryUnload = Durin::FModuleManager::Get().UnloadModule("GeometryBuild");
	ASSERT_TRUE(GeometryUnload.Succeeded()) << GeometryUnload.Message;
	EXPECT_EQ(Durin::FAssetCompilingManager::Get().GetDiagnostics().ManagerCount, 1u);
	Durin::FModuleManager::Get().LoadModuleChecked("GeometryBuild");
	EXPECT_TRUE(EnsureTextureCompilingManager());
	EXPECT_EQ(Durin::FAssetCompilingManager::Get().GetDiagnostics().ManagerCount, 2u);
	Durin::Asset::Build::FTexture2DBuildCoordinator* Coordinator =
		Durin::Asset::Build::GetTexture2DBuildCoordinator();
	ASSERT_NE(Coordinator, nullptr);
	bool bCompleted = false;
	const uint64 RequestId = Coordinator->Submit(
		MakeCoordinatorRequest(TransparentPngData(), "/CompilingManager/Restarted"),
		[&](Durin::Asset::Build::FTexture2DQueuedBuildResult&& Result) {
			EXPECT_EQ(
				Result.Phase,
				Durin::Asset::Build::ETexture2DBuildPhase::UploadPending);
			bCompleted = true;
		});
	ASSERT_NE(RequestId, 0u);
	ASSERT_TRUE(Coordinator->WaitForRequest(RequestId, 10.0));
	EXPECT_EQ(Coordinator->PumpCompletions(), 1u);
	EXPECT_TRUE(bCompleted);
}

TEST(FTexture2DAuthoringCoordinatorTests, BuildsOwnedNormalizedRequestInBuildModule)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	Durin::FTextureSourceData SourceData;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		TransparentPngData(), SourceData, Error)) << Error;
	const Durin::FXxHash128 SourceHash =
		Durin::FXxHash128::HashBuffer(TransparentPngData());
	ASSERT_TRUE(RestartTextureCompilingManager({.MaxWorkers = 1}));
	Durin::Asset::Build::FTexture2DBuildCoordinator* Coordinator =
		Durin::Asset::Build::GetTexture2DBuildCoordinator();
	ASSERT_NE(Coordinator, nullptr);
	std::optional<Durin::Asset::Build::FTexture2DQueuedBuildResult> Completion;
	const uint64 RequestId = Coordinator->Submit({
		.AssetIdentity = "/AuthoringCoordinator/Normalized",
		.SourcePath = {.Path = "/TextureImportTests/Coordinator.png"},
		.SourceData = std::move(SourceData),
		.SourceHash = SourceHash,
		.Settings = {
			.Usage = Durin::ETextureUsage::Color,
			.bSRGB = true,
			.CompressionQuality = Durin::ETextureCompressionQuality::Low},
		.Generation = 1,
		.EstimatedWidth = 2,
		.EstimatedHeight = 1,
		.bPersistDerivedData = false},
		[&](Durin::Asset::Build::FTexture2DQueuedBuildResult&& Result) {
			Completion.emplace(std::move(Result));
		});
	ASSERT_NE(RequestId, 0u);
	ASSERT_TRUE(Coordinator->WaitForRequest(RequestId, 10.0));
	EXPECT_EQ(Durin::FAssetCompilingManager::Get().ProcessAsyncTasks().ProcessedCompletionCount, 1u);
	ASSERT_TRUE(Completion.has_value());
	EXPECT_EQ(Completion->Phase,
		Durin::Asset::Build::ETexture2DBuildPhase::UploadPending);
	ASSERT_NE(Completion->SourceData, nullptr);
	ASSERT_NE(Completion->PlatformData, nullptr);
	EXPECT_TRUE(Completion->SourceData->IsValid());
	EXPECT_TRUE(Completion->PlatformData->IsValid());
}

TEST(FTexture2DBuildCoordinatorTests, WorkerResultMatchesSynchronousBuildAndReportsMetrics)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureCoordinatorEquivalent.png";
	WriteTextureFixture(Source);
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, Source));

	Durin::FTextureSourceData BaselineSource;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTexture2DSource(
		Bytes, BaselineSource, Error)) << Error;
	Durin::FTexturePlatformData BaselinePlatform;
	ASSERT_TRUE(Durin::Asset::Build::TextureBuilder::BuildMipChain(
		BaselineSource,
		Durin::ETextureUsage::Color,
		true,
		BaselinePlatform,
		Error,
		0,
		Durin::ETextureCompressionQuality::Low)) << Error;

	Durin::Asset::Build::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 1});
	std::mutex Mutex;
	std::condition_variable Condition;
	uint32 TerminalCount = 0;
	std::optional<Durin::Asset::Build::FTexture2DQueuedBuildResult> Completion;
	Coordinator.SetPhaseHookForTests([&](uint64, Durin::Asset::Build::ETexture2DBuildPhase Phase) {
		if (!IsTerminal(Phase)) return;
		std::lock_guard Lock(Mutex);
		++TerminalCount;
		Condition.notify_all();
	});
	const uint64 RequestId = Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Equivalent"),
		[&](Durin::Asset::Build::FTexture2DQueuedBuildResult&& Result) {
			Completion.emplace(std::move(Result));
		});
	ASSERT_NE(RequestId, 0u);
	ASSERT_TRUE(WaitForTerminalCount(Mutex, Condition, TerminalCount, 1));
	EXPECT_EQ(Coordinator.PumpCompletions(), 1u);
	ASSERT_TRUE(Completion.has_value());
	ASSERT_EQ(Completion->Phase, Durin::Asset::Build::ETexture2DBuildPhase::UploadPending);
	ASSERT_NE(Completion->SourceData, nullptr);
	ASSERT_NE(Completion->PlatformData, nullptr);
	EXPECT_EQ(Completion->SourceData->Pixels, BaselineSource.Pixels);
	ExpectPlatformDataEqual(*Completion->PlatformData, BaselinePlatform);
	EXPECT_GT(Completion->Metrics.PreparationNanoseconds, 0u);
	EXPECT_GT(Completion->Metrics.PeakIntermediateBytes, 0u);
	EXPECT_GT(Completion->Metrics.ResultBytes, 0u);
	const Durin::Asset::Build::FTexture2DBuildDiagnostic Diagnostic = Coordinator.GetDiagnostic(RequestId);
	EXPECT_GT(Diagnostic.QueuedNanoseconds, 0u);
	EXPECT_EQ(Diagnostic.FailurePhase, Durin::Asset::Build::ETexture2DBuildPhase::None);
}

TEST(FTexture2DBuildCoordinatorTests, RejectsInvalidNormalizedSourceBeforeAdmission)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	Durin::Asset::Build::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 1});
	uint32 CompletionCount = 0;
	const uint64 RequestId = Coordinator.Submit(
		{.AssetIdentity = "/Coordinator/Invalid", .SourceHash = {.HashLow = 1}},
		[&](Durin::Asset::Build::FTexture2DQueuedBuildResult&&) { ++CompletionCount; });
	EXPECT_EQ(RequestId, 0u);
	EXPECT_EQ(Coordinator.PumpCompletions(), 0u);
	EXPECT_EQ(CompletionCount, 0u);
}

TEST(FTexture2DBuildCoordinatorTests, BoundsAdmissionAndPreventsBackgroundStarvation)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureCoordinatorScheduling.png";
	WriteTextureFixture(Source);
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, Source));

	Durin::Asset::Build::FTexture2DBuildCoordinator Coordinator({
		.MaxWorkers = 2,
		.InteractiveBurstLimit = 4,
		.InFlightByteBudget = 50});
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bReleaseFirst = false;
	uint32 TerminalCount = 0;
	std::vector<uint64> PreparationOrder;
	Coordinator.SetPhaseHookForTests([&](uint64 RequestId, Durin::Asset::Build::ETexture2DBuildPhase Phase) {
		std::unique_lock Lock(Mutex);
		if (Phase == Durin::Asset::Build::ETexture2DBuildPhase::Preparing)
		{
			PreparationOrder.push_back(RequestId);
			Condition.notify_all();
			if (PreparationOrder.size() == 1)
				Condition.wait(Lock, [&] { return bReleaseFirst; });
		}
		if (IsTerminal(Phase))
		{
			++TerminalCount;
			Condition.notify_all();
		}
	});
	std::vector<uint64> BackgroundIds;
	std::vector<uint64> InteractiveIds;
	uint32 CompletionCount = 0;
	auto Complete = [&](Durin::Asset::Build::FTexture2DQueuedBuildResult&& Result) {
		EXPECT_EQ(Result.Phase, Durin::Asset::Build::ETexture2DBuildPhase::UploadPending);
		++CompletionCount;
	};
	BackgroundIds.push_back(Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Background0"), Complete));
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return PreparationOrder.size() == 1;
		}));
	}
	BackgroundIds.push_back(Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Background1"), Complete));
	for (uint32 Index = 0; Index < 5; ++Index)
	{
		InteractiveIds.push_back(Coordinator.Submit(
			MakeCoordinatorRequest(
				Bytes,
				std::format("/Coordinator/Interactive{}", Index),
				Durin::Asset::Build::ETexture2DBuildPriority::Interactive),
			Complete));
	}
	EXPECT_EQ(Coordinator.GetRunningCount(), 1u);
	EXPECT_EQ(Coordinator.GetQueuedCount(), 6u);
	EXPECT_LE(Coordinator.GetInFlightEstimatedBytes(), 100u);
	{
		std::lock_guard Lock(Mutex);
		bReleaseFirst = true;
		Condition.notify_all();
	}
	ASSERT_TRUE(WaitForTerminalCount(Mutex, Condition, TerminalCount, 7));
	EXPECT_EQ(Coordinator.PumpCompletions(16), 7u);
	EXPECT_EQ(CompletionCount, 7u);
	ASSERT_EQ(PreparationOrder.size(), 7u);
	EXPECT_EQ(PreparationOrder[0], BackgroundIds[0]);
	for (size_t Index = 0; Index < 4; ++Index)
		EXPECT_EQ(PreparationOrder[Index + 1], InteractiveIds[Index]);
	EXPECT_EQ(PreparationOrder[5], BackgroundIds[1]);
	EXPECT_EQ(PreparationOrder[6], InteractiveIds[4]);
}

TEST(FTexture2DBuildCoordinatorTests, CancelsRunningAndQueuedWorkExactlyOnceDuringShutdown)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureCoordinatorCancellation.png";
	WriteTextureFixture(Source);
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, Source));

	Durin::Asset::Build::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 1});
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bWorkerEntered = false;
	bool bReleaseWorker = false;
	Coordinator.SetPhaseHookForTests([&](uint64, Durin::Asset::Build::ETexture2DBuildPhase Phase) {
		if (Phase != Durin::Asset::Build::ETexture2DBuildPhase::Preparing) return;
		std::unique_lock Lock(Mutex);
		bWorkerEntered = true;
		Condition.notify_all();
		Condition.wait(Lock, [&] { return bReleaseWorker; });
	});
	std::vector<Durin::Asset::Build::ETexture2DBuildPhase> CompletionPhases;
	auto Complete = [&](Durin::Asset::Build::FTexture2DQueuedBuildResult&& Result) {
		CompletionPhases.push_back(Result.Phase);
	};
	const uint64 RunningId = Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Running"), Complete);
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bWorkerEntered;
		}));
	}
	const uint64 QueuedId = Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Queued"), Complete);
	ASSERT_TRUE(Coordinator.Cancel(RunningId));
	ASSERT_TRUE(Coordinator.Cancel(QueuedId));
	{
		std::lock_guard Lock(Mutex);
		bReleaseWorker = true;
		Condition.notify_all();
	}
	Coordinator.Shutdown();
	ASSERT_EQ(CompletionPhases.size(), 2u);
	EXPECT_EQ(CompletionPhases[0], Durin::Asset::Build::ETexture2DBuildPhase::Cancelled);
	EXPECT_EQ(CompletionPhases[1], Durin::Asset::Build::ETexture2DBuildPhase::Cancelled);
	EXPECT_EQ(Coordinator.GetRunningCount(), 0u);
	EXPECT_EQ(Coordinator.GetQueuedCount(), 0u);
}

TEST(FTexture2DBuildCoordinatorTests, BuildModuleFramePumpAppliesAtMostSixtyFourExactlyOnce)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(EnsureTextureCompilingManager());
	Durin::Asset::Build::FTexture2DBuildCoordinator* Coordinator =
		Durin::Asset::Build::GetTexture2DBuildCoordinator();
	ASSERT_NE(Coordinator, nullptr);
	std::mutex Mutex;
	std::condition_variable Condition;
	uint32 TerminalCount = 0;
	uint32 CompletionCount = 0;
	Coordinator->SetPhaseHookForTests(
		[&](uint64, Durin::Asset::Build::ETexture2DBuildPhase Phase) {
			if (!IsTerminal(Phase)) return;
			std::lock_guard Lock(Mutex);
			++TerminalCount;
			Condition.notify_all();
		});
	for (uint32 Index = 0; Index < 65; ++Index)
	{
		ASSERT_NE(Coordinator->Submit(
			MakeCoordinatorRequest(
				TransparentPngData(), std::format("/Coordinator/FrameBudget{}", Index)),
			[&](Durin::Asset::Build::FTexture2DQueuedBuildResult&&) {
				EXPECT_TRUE(Durin::IsInGameThread());
				++CompletionCount;
			}), 0u);
	}
	ASSERT_TRUE(WaitForTerminalCount(Mutex, Condition, TerminalCount, 65));
	Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(CompletionCount, 64u);
	Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(CompletionCount, 65u);
	Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(CompletionCount, 65u);
	Coordinator->SetPhaseHookForTests({});
}

TEST(FTexture2DBuildCoordinatorTests, ExplicitWaitLeavesCompletionForAnUnboundedPump)
{
	InitializeDObjectSystem();
	Durin::Asset::Build::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 2});
	uint32 CompletionCount = 0;
	std::vector<uint64> RequestIds;
	for (uint32 Index = 0; Index < 70; ++Index)
	{
		RequestIds.push_back(Coordinator.Submit(
			MakeCoordinatorRequest(
				TransparentPngData(), std::format("/Coordinator/ExplicitWait{}", Index)),
			[&](Durin::Asset::Build::FTexture2DQueuedBuildResult&&) { ++CompletionCount; }));
		ASSERT_NE(RequestIds.back(), 0u);
	}
	for (const uint64 RequestId : RequestIds)
		ASSERT_TRUE(Coordinator.WaitForRequest(RequestId, 10.0));
	EXPECT_EQ(CompletionCount, 0u);
	EXPECT_EQ(
		Coordinator.PumpCompletions(std::numeric_limits<uint32>::max()),
		70u);
	EXPECT_EQ(CompletionCount, 70u);
}

TEST(FAuthoringBuildServiceTests, OwnsRestartableProviderLifecycleAndDiagnostics)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(RestartTextureCompilingManager(
		{.MaxWorkers = 1, .InFlightByteBudget = 1024}));
	const Durin::FAssetCompilingManagerDiagnostics Running =
		Durin::FAssetCompilingManager::Get().GetDiagnostics();
	EXPECT_TRUE(Running.bAcceptingRequests);
	EXPECT_EQ(Running.RemainingAssetCount, 0u);
	EXPECT_EQ(Running.ManagerCount, 2u);

	Durin::Asset::Build::ShutdownTextureBuildService();
	const Durin::FAssetCompilingManagerDiagnostics WithoutTexture =
		Durin::FAssetCompilingManager::Get().GetDiagnostics();
	EXPECT_TRUE(WithoutTexture.bAcceptingRequests);
	EXPECT_EQ(WithoutTexture.ManagerCount, 1u);
	ASSERT_TRUE(EnsureTextureCompilingManager());
}
