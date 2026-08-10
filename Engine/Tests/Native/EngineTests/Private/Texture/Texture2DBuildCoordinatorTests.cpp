#include "TextureTestSupport.h"

#include "EngineAssetServices.h"
#include "Misc/FileHelper.h"
#include "Texture/Texture2DBuildCoordinator.h"
#include "Threading/RunnableThread.h"
#include "Threading/Task.h"

namespace
{
	auto IsTerminal(Durin::ETexture2DBuildPhase Phase) -> bool
	{
		return Phase == Durin::ETexture2DBuildPhase::UploadPending
			|| Phase == Durin::ETexture2DBuildPhase::Failed
			|| Phase == Durin::ETexture2DBuildPhase::Cancelled;
	}

	auto MakeCoordinatorRequest(
		std::span<const Durin::uint8> Bytes,
		std::string Identity,
		Durin::ETexture2DBuildPriority Priority = Durin::ETexture2DBuildPriority::Background)
		-> Durin::FTexture2DBuildRequest
	{
		return {
			.AssetIdentity = std::move(Identity),
			.SourcePath = {.Path = "/TextureImportTests/Coordinator.png"},
			.EncodedSource = {Bytes.begin(), Bytes.end()},
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
		const Durin::uint32& TerminalCount,
		Durin::uint32 Expected) -> bool
	{
		std::unique_lock Lock(Mutex);
		return Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return TerminalCount >= Expected;
		});
	}

	auto MakeTgaBytes(Durin::uint16 Dimension) -> std::vector<Durin::uint8>
	{
		std::vector<Durin::uint8> Bytes(
			18 + static_cast<size_t>(Dimension) * Dimension * 4, 0);
		Bytes[2] = 2;
		Bytes[12] = static_cast<Durin::uint8>(Dimension & 0xff);
		Bytes[13] = static_cast<Durin::uint8>(Dimension >> 8);
		Bytes[14] = static_cast<Durin::uint8>(Dimension & 0xff);
		Bytes[15] = static_cast<Durin::uint8>(Dimension >> 8);
		Bytes[16] = 32;
		Bytes[17] = 0x28;
		for (size_t Offset = 18; Offset < Bytes.size(); Offset += 4)
		{
			const Durin::uint8 Value = static_cast<Durin::uint8>((Offset / 4) * 37u);
			Bytes[Offset] = Value;
			Bytes[Offset + 1] = static_cast<Durin::uint8>(255u - Value);
			Bytes[Offset + 2] = static_cast<Durin::uint8>(Value ^ 0x5a);
			Bytes[Offset + 3] = 255;
		}
		return Bytes;
	}
}

TEST(FTexture2DBuildCoordinatorTests, WorkerResultMatchesSynchronousBuildAndReportsMetrics)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureCoordinatorEquivalent.png";
	WriteTextureFixture(Source);
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, Source.generic_string()));

	Durin::FTextureSourceData BaselineSource;
	std::string Error;
	ASSERT_TRUE(Durin::TextureBuild::DecodeRGBA8(Bytes, BaselineSource, Error)) << Error;
	Durin::FTexturePlatformData BaselinePlatform;
	ASSERT_TRUE(Durin::TextureBuild::BuildMipChain(
		BaselineSource,
		Durin::ETextureUsage::Color,
		true,
		BaselinePlatform,
		Error,
		0,
		Durin::ETextureCompressionQuality::Low)) << Error;

	Durin::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 1});
	std::mutex Mutex;
	std::condition_variable Condition;
	Durin::uint32 TerminalCount = 0;
	std::optional<Durin::FTexture2DBuildResult> Completion;
	Coordinator.SetPhaseHookForTests([&](Durin::uint64, Durin::ETexture2DBuildPhase Phase) {
		if (!IsTerminal(Phase)) return;
		std::lock_guard Lock(Mutex);
		++TerminalCount;
		Condition.notify_all();
	});
	const Durin::uint64 RequestId = Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Equivalent"),
		[&](Durin::FTexture2DBuildResult&& Result) {
			Completion.emplace(std::move(Result));
		});
	ASSERT_NE(RequestId, 0u);
	ASSERT_TRUE(WaitForTerminalCount(Mutex, Condition, TerminalCount, 1));
	EXPECT_EQ(Coordinator.PumpCompletions(), 1u);
	ASSERT_TRUE(Completion.has_value());
	ASSERT_EQ(Completion->Phase, Durin::ETexture2DBuildPhase::UploadPending);
	ASSERT_NE(Completion->SourceData, nullptr);
	ASSERT_NE(Completion->PlatformData, nullptr);
	EXPECT_EQ(Completion->SourceData->Pixels, BaselineSource.Pixels);
	ExpectPlatformDataEqual(*Completion->PlatformData, BaselinePlatform);
	EXPECT_GT(Completion->Metrics.DecodeNanoseconds, 0u);
	EXPECT_GT(Completion->Metrics.PeakIntermediateBytes, 0u);
	EXPECT_GT(Completion->Metrics.ResultBytes, 0u);
	const Durin::FTexture2DBuildDiagnostic Diagnostic = Coordinator.GetDiagnostic(RequestId);
	EXPECT_GT(Diagnostic.QueuedNanoseconds, 0u);
	EXPECT_EQ(Diagnostic.FailurePhase, Durin::ETexture2DBuildPhase::None);
}

TEST(FTexture2DBuildCoordinatorTests, AttributesDecodeFailureToTheRequestPhase)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(1));
	const std::array<Durin::uint8, 8> InvalidBytes{0, 1, 2, 3, 4, 5, 6, 7};
	Durin::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 1});
	std::mutex Mutex;
	std::condition_variable Condition;
	Durin::uint32 TerminalCount = 0;
	std::optional<Durin::FTexture2DBuildResult> Completion;
	Coordinator.SetPhaseHookForTests([&](Durin::uint64, Durin::ETexture2DBuildPhase Phase) {
		if (!IsTerminal(Phase)) return;
		std::lock_guard Lock(Mutex);
		++TerminalCount;
		Condition.notify_all();
	});
	const Durin::uint64 RequestId = Coordinator.Submit(
		MakeCoordinatorRequest(InvalidBytes, "/Coordinator/Invalid"),
		[&](Durin::FTexture2DBuildResult&& Result) {
			Completion.emplace(std::move(Result));
		});
	ASSERT_NE(RequestId, 0u);
	ASSERT_TRUE(WaitForTerminalCount(Mutex, Condition, TerminalCount, 1));
	ASSERT_EQ(Coordinator.PumpCompletions(), 1u);
	ASSERT_TRUE(Completion.has_value());
	EXPECT_EQ(Completion->Phase, Durin::ETexture2DBuildPhase::Failed);
	EXPECT_EQ(Completion->FailurePhase, Durin::ETexture2DBuildPhase::Decoding);
	const Durin::FTexture2DBuildDiagnostic Diagnostic = Coordinator.GetDiagnostic(RequestId);
	EXPECT_EQ(Diagnostic.Phase, Durin::ETexture2DBuildPhase::Failed);
	EXPECT_EQ(Diagnostic.FailurePhase, Durin::ETexture2DBuildPhase::Decoding);
	EXPECT_FALSE(Diagnostic.Message.empty());
}

TEST(FTexture2DBuildCoordinatorTests, BoundsAdmissionAndPreventsBackgroundStarvation)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureCoordinatorScheduling.png";
	WriteTextureFixture(Source);
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, Source.generic_string()));

	Durin::FTexture2DBuildCoordinator Coordinator({
		.MaxWorkers = 2,
		.InteractiveBurstLimit = 4,
		.InFlightByteBudget = 100});
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bReleaseFirst = false;
	Durin::uint32 TerminalCount = 0;
	std::vector<Durin::uint64> DecodeOrder;
	Coordinator.SetPhaseHookForTests([&](Durin::uint64 RequestId, Durin::ETexture2DBuildPhase Phase) {
		std::unique_lock Lock(Mutex);
		if (Phase == Durin::ETexture2DBuildPhase::Decoding)
		{
			DecodeOrder.push_back(RequestId);
			Condition.notify_all();
			if (DecodeOrder.size() == 1)
				Condition.wait(Lock, [&] { return bReleaseFirst; });
		}
		if (IsTerminal(Phase))
		{
			++TerminalCount;
			Condition.notify_all();
		}
	});
	std::vector<Durin::uint64> BackgroundIds;
	std::vector<Durin::uint64> InteractiveIds;
	Durin::uint32 CompletionCount = 0;
	auto Complete = [&](Durin::FTexture2DBuildResult&& Result) {
		EXPECT_EQ(Result.Phase, Durin::ETexture2DBuildPhase::UploadPending);
		++CompletionCount;
	};
	BackgroundIds.push_back(Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Background0"), Complete));
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return DecodeOrder.size() == 1;
		}));
	}
	BackgroundIds.push_back(Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Background1"), Complete));
	for (Durin::uint32 Index = 0; Index < 5; ++Index)
	{
		InteractiveIds.push_back(Coordinator.Submit(
			MakeCoordinatorRequest(
				Bytes,
				std::format("/Coordinator/Interactive{}", Index),
				Durin::ETexture2DBuildPriority::Interactive),
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
	ASSERT_EQ(DecodeOrder.size(), 7u);
	EXPECT_EQ(DecodeOrder[0], BackgroundIds[0]);
	for (size_t Index = 0; Index < 4; ++Index)
		EXPECT_EQ(DecodeOrder[Index + 1], InteractiveIds[Index]);
	EXPECT_EQ(DecodeOrder[5], BackgroundIds[1]);
	EXPECT_EQ(DecodeOrder[6], InteractiveIds[4]);
}

TEST(FTexture2DBuildCoordinatorTests, CancelsRunningAndQueuedWorkExactlyOnceDuringShutdown)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureCoordinatorCancellation.png";
	WriteTextureFixture(Source);
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Bytes, Source.generic_string()));

	Durin::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 1});
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bWorkerEntered = false;
	bool bReleaseWorker = false;
	Coordinator.SetPhaseHookForTests([&](Durin::uint64, Durin::ETexture2DBuildPhase Phase) {
		if (Phase != Durin::ETexture2DBuildPhase::Decoding) return;
		std::unique_lock Lock(Mutex);
		bWorkerEntered = true;
		Condition.notify_all();
		Condition.wait(Lock, [&] { return bReleaseWorker; });
	});
	std::vector<Durin::ETexture2DBuildPhase> CompletionPhases;
	auto Complete = [&](Durin::FTexture2DBuildResult&& Result) {
		CompletionPhases.push_back(Result.Phase);
	};
	const Durin::uint64 RunningId = Coordinator.Submit(
		MakeCoordinatorRequest(Bytes, "/Coordinator/Running"), Complete);
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bWorkerEntered;
		}));
	}
	const Durin::uint64 QueuedId = Coordinator.Submit(
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
	EXPECT_EQ(CompletionPhases[0], Durin::ETexture2DBuildPhase::Cancelled);
	EXPECT_EQ(CompletionPhases[1], Durin::ETexture2DBuildPhase::Cancelled);
	EXPECT_EQ(Coordinator.GetRunningCount(), 0u);
	EXPECT_EQ(Coordinator.GetQueuedCount(), 0u);
}

TEST(FTexture2DBuildCoordinatorTests, EngineAssetServiceFramePumpAppliesAtMostSixtyFourExactlyOnce)
{
	InitializeDObjectSystem();
	Durin::FTexture2DBuildCoordinator* Coordinator =
		Durin::GetTexture2DBuildCoordinator();
	ASSERT_NE(Coordinator, nullptr);
	const std::array<Durin::uint8, 8> InvalidBytes{0, 1, 2, 3, 4, 5, 6, 7};
	std::mutex Mutex;
	std::condition_variable Condition;
	Durin::uint32 TerminalCount = 0;
	Durin::uint32 CompletionCount = 0;
	Coordinator->SetPhaseHookForTests(
		[&](Durin::uint64, Durin::ETexture2DBuildPhase Phase) {
			if (!IsTerminal(Phase)) return;
			std::lock_guard Lock(Mutex);
			++TerminalCount;
			Condition.notify_all();
		});
	for (Durin::uint32 Index = 0; Index < 65; ++Index)
	{
		ASSERT_NE(Coordinator->Submit(
			MakeCoordinatorRequest(
				InvalidBytes, std::format("/Coordinator/FrameBudget{}", Index)),
			[&](Durin::FTexture2DBuildResult&&) {
				EXPECT_TRUE(Durin::IsInGameThread());
				++CompletionCount;
			}), 0u);
	}
	ASSERT_TRUE(WaitForTerminalCount(Mutex, Condition, TerminalCount, 65));
	Durin::PumpEngineAssetServiceCompletions();
	EXPECT_EQ(CompletionCount, 64u);
	Durin::PumpEngineAssetServiceCompletions();
	EXPECT_EQ(CompletionCount, 65u);
	Durin::PumpEngineAssetServiceCompletions();
	EXPECT_EQ(CompletionCount, 65u);
	Coordinator->SetPhaseHookForTests({});
}

TEST(FTexture2DBuildCoordinatorTests, ExplicitWaitLeavesCompletionForAnUnboundedPump)
{
	InitializeDObjectSystem();
	const std::array<Durin::uint8, 8> InvalidBytes{0, 1, 2, 3, 4, 5, 6, 7};
	Durin::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 2});
	Durin::uint32 CompletionCount = 0;
	std::vector<Durin::uint64> RequestIds;
	for (Durin::uint32 Index = 0; Index < 70; ++Index)
	{
		RequestIds.push_back(Coordinator.Submit(
			MakeCoordinatorRequest(
				InvalidBytes, std::format("/Coordinator/ExplicitWait{}", Index)),
			[&](Durin::FTexture2DBuildResult&&) { ++CompletionCount; }));
		ASSERT_NE(RequestIds.back(), 0u);
	}
	for (const Durin::uint64 RequestId : RequestIds)
		ASSERT_TRUE(Coordinator.WaitForRequest(RequestId, 10.0));
	EXPECT_EQ(CompletionCount, 0u);
	EXPECT_EQ(
		Coordinator.PumpCompletions(std::numeric_limits<Durin::uint32>::max()),
		70u);
	EXPECT_EQ(CompletionCount, 70u);
}

TEST(FTexture2DBuildCharacterization, ReportsBuildCostForRequestedDimensions)
{
	const char* RequestedDimensions = std::getenv(
		"DURIN_TEXTURE_BUILD_CHARACTERIZATION_DIMENSIONS");
	if (!RequestedDimensions || std::string_view(RequestedDimensions).empty())
		GTEST_SKIP() << "Set DURIN_TEXTURE_BUILD_CHARACTERIZATION_DIMENSIONS to a comma-separated dimension list.";
	std::vector<Durin::uint32> Dimensions;
	std::stringstream DimensionStream(RequestedDimensions);
	std::string DimensionText;
	while (std::getline(DimensionStream, DimensionText, ','))
	{
		const unsigned long Parsed = std::stoul(DimensionText);
		ASSERT_GT(Parsed, 0u);
		ASSERT_LE(Parsed, Durin::TextureBuild::MaxDimension);
		Dimensions.push_back(static_cast<Durin::uint32>(Parsed));
	}
	ASSERT_FALSE(Dimensions.empty());
	std::cout
		<< "TEXTURE_BUILD_CHARACTERIZATION profile=Win64-Debug-DurinEditor\n"
		<< "dimension,usage,quality,total_ms,mip_ms,compression_ms,decoded_bytes,intermediate_bytes,result_bytes\n";
	for (const Durin::uint32 Dimension : Dimensions)
	{
		Durin::FTextureSourceData Source;
		Source.Width = Dimension;
		Source.Height = Dimension;
		Source.SourceChannelCount = 4;
		Source.Format = Durin::ETextureSourceFormat::RGBA8;
		Source.Pixels.resize(
			static_cast<size_t>(Dimension) * Dimension * Durin::TextureBuild::ChannelCount);
		for (size_t Offset = 0; Offset < Source.Pixels.size(); Offset += 4)
		{
			const Durin::uint8 Value = static_cast<Durin::uint8>((Offset / 4) * 37u);
			Source.Pixels[Offset] = Value;
			Source.Pixels[Offset + 1] = static_cast<Durin::uint8>(255u - Value);
			Source.Pixels[Offset + 2] = static_cast<Durin::uint8>(Value ^ 0x5a);
			Source.Pixels[Offset + 3] = 255;
		}
		for (const Durin::ETextureUsage Usage : {
			Durin::ETextureUsage::Color,
			Durin::ETextureUsage::Normal,
			Durin::ETextureUsage::DataMask})
		{
			for (const Durin::ETextureCompressionQuality Quality : {
				Durin::ETextureCompressionQuality::Low,
				Durin::ETextureCompressionQuality::Normal,
				Durin::ETextureCompressionQuality::High})
			{
				Durin::FTexturePlatformData Platform;
				Durin::TextureBuild::FBuildMipChainMetrics Metrics;
				const Durin::TextureBuild::FBuildExecutionControl Control{
					.Metrics = &Metrics};
				std::string Error;
				const auto Start = std::chrono::steady_clock::now();
				ASSERT_TRUE(Durin::TextureBuild::BuildMipChain(
					Source,
					Usage,
					Usage == Durin::ETextureUsage::Color,
					Platform,
					Error,
					0,
					Quality,
					Durin::ETextureAlphaMipMode::Average,
					0.5f,
					&Control)) << Error;
				const Durin::uint64 TotalNanoseconds = static_cast<Durin::uint64>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - Start).count());
				Durin::uint64 ResultBytes = 0;
				for (const Durin::FTexture2DMipData& Mip : Platform.Mips)
					ResultBytes += Mip.Pixels.size();
				std::cout
					<< Dimension << ','
					<< static_cast<Durin::uint32>(Usage) << ','
					<< static_cast<Durin::uint32>(Quality) << ','
					<< TotalNanoseconds / 1'000'000.0 << ','
					<< Metrics.MipGenerationNanoseconds / 1'000'000.0 << ','
					<< Metrics.CompressionNanoseconds / 1'000'000.0 << ','
					<< Source.Pixels.size() << ','
					<< Metrics.PeakIntermediateBytes << ','
					<< ResultBytes << '\n';
			}
		}
	}
}

TEST(FTexture2DBuildCharacterization, ReportsCompletionCallbackCost)
{
	if (const char* Enabled = std::getenv("DURIN_TEXTURE_COMPLETION_CHARACTERIZATION");
		!Enabled || std::string_view(Enabled) != "1")
	{
		GTEST_SKIP() << "Set DURIN_TEXTURE_COMPLETION_CHARACTERIZATION=1 to measure representative callbacks.";
	}
	InitializeDObjectSystem();
	std::cout
		<< "TEXTURE_COMPLETION_CHARACTERIZATION profile=Win64-Debug-DurinEditor\n"
		<< "dimension,outcome,completion_us\n";
	for (const Durin::uint16 Dimension : {Durin::uint16{1024}, Durin::uint16{4096}})
	{
		Durin::FTexture2DBuildCoordinator Coordinator({.MaxWorkers = 1});
		auto Run = [&](std::string_view Outcome, std::vector<Durin::uint8> Bytes,
			bool bCancel, bool bStale) {
			std::unique_ptr<Durin::FTexturePlatformData> AcceptedPlatformData;
			Durin::FTexture2DBuildRequest Request = MakeCoordinatorRequest(
				Bytes, std::format("/Coordinator/Characterization/{}/{}", Dimension, Outcome));
			Request.EstimatedWidth = Dimension;
			Request.EstimatedHeight = Dimension;
			Request.Generation = bStale ? 2 : 1;
			const Durin::uint64 ExpectedGeneration = 1;
			const Durin::uint64 RequestId = Coordinator.Submit(
				std::move(Request),
				[&](Durin::FTexture2DBuildResult&& Result) {
					if (Result.Generation != ExpectedGeneration) return;
					if (Result.Phase == Durin::ETexture2DBuildPhase::UploadPending)
						AcceptedPlatformData = std::move(Result.PlatformData);
				});
			ASSERT_NE(RequestId, 0u);
			if (bCancel) ASSERT_TRUE(Coordinator.Cancel(RequestId));
			ASSERT_TRUE(Coordinator.WaitForRequest(RequestId, 60.0));
			ASSERT_EQ(Coordinator.PumpCompletions(1), 1u);
			const Durin::FTexture2DBuildDiagnostic Diagnostic =
				Coordinator.GetDiagnostic(RequestId);
			std::cout << Dimension << ',' << Outcome << ','
				<< Diagnostic.Metrics.CompletionNanoseconds / 1000.0 << '\n';
		};
		const std::vector<Durin::uint8> ValidBytes = MakeTgaBytes(Dimension);
		Run("success", ValidBytes, false, false);
		Run("stale", ValidBytes, false, true);
		Run("failed", {0, 1, 2, 3, 4, 5, 6, 7}, false, false);
		Run("cancelled", ValidBytes, true, false);
	}
}
