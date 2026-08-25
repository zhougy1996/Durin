#include <gtest/gtest.h>

#include "Assets/SourceImageThumbnailCache.h"
#include "Assets/SourceImageThumbnailDecoder.h"
#include "Assets/SourceImageThumbnailDiskCache.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		auto WriteBinaryFixture(std::string_view Name, std::span<const std::byte> Bytes) -> std::filesystem::path
		{
			const std::filesystem::path Path = Testing::GetTestWorkDirectory() / Name;
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			return Path;
		}

		auto TransparentPngBytes() -> std::span<const std::byte>
		{
			static constexpr uint8 Bytes[] = {
				137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
				0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
				0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
			return std::as_bytes(std::span{Bytes});
		}

		auto PrepareDiskCacheTest(std::string_view Name) -> std::pair<std::filesystem::path, std::filesystem::path>
		{
			const std::filesystem::path Root = Testing::GetTestWorkDirectory() / Name;
			Durin::Testing::RemoveTestWorkDirectory(Root);
			const std::filesystem::path SourceRoot = Root / "Content";
			std::filesystem::create_directories(SourceRoot);
			return {Root / "Cache", SourceRoot};
		}

		auto CountPngObjects(const std::filesystem::path& CacheRoot) -> size_t
		{
			size_t Count = 0;
			std::error_code Error;
			if (!std::filesystem::exists(CacheRoot, Error)) return 0;
			for (std::filesystem::recursive_directory_iterator It(CacheRoot, Error), End; It != End && !Error; It.increment(Error))
				if (It->is_regular_file() && It->path().extension() == ".png") ++Count;
			return Count;
		}

		struct FBlockingTaskState
		{
			std::mutex Mutex;
			std::condition_variable Condition;
			bool bEntered = false;
			bool bRelease = false;
		};
	}

	TEST(FSourceImageThumbnailTests, RecognizesOnlySupportedExtensionsCaseInsensitively)
	{
		EXPECT_TRUE(IsSupportedSourceImageExtension(".PNG"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".jpg"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".jpeg"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".bmp"));
		EXPECT_TRUE(IsSupportedSourceImageExtension(".tga"));
		EXPECT_FALSE(IsSupportedSourceImageExtension(".gif"));
		EXPECT_FALSE(IsSupportedSourceImageExtension(".dasset"));
	}

	TEST(FSourceImageThumbnailTests, PublicRequestsAndViewsUseItemIdentity)
	{
		FSourceImageThumbnailCache Cache;
		Cache.BeginFrame();
		const auto LastWriteTime = std::filesystem::file_time_type::clock::now();
		Cache.Request({
			.Identity = "/Game/Textures/Example",
			.PhysicalPath = "C:/Project/Content/Textures/Example.png",
			.FileSize = 128,
			.LastWriteTime = LastWriteTime,
			.Priority = Editor::EAssetThumbnailPriority::Visible});

		const Editor::FAssetThumbnailView AssetView = Cache.Find("/Game/Textures/Example");
		EXPECT_EQ(AssetView.State, Editor::EAssetThumbnailState::Queued);
		EXPECT_EQ(AssetView.RequestSerial, 1u);
		EXPECT_EQ(Cache.Find("C:/Project/Content/Textures/Example.png").State,
			Editor::EAssetThumbnailState::NotRequested);

		Cache.CancelPendingRequests();
		const Editor::FAssetThumbnailView CancelledView = Cache.Find("/Game/Textures/Example");
		EXPECT_EQ(CancelledView.State, Editor::EAssetThumbnailState::NotRequested);
		EXPECT_GT(CancelledView.RequestSerial, AssetView.RequestSerial);
	}

	TEST(FSourceImageThumbnailTests, MultipleItemIdentitiesCoalesceOneSourceEntry)
	{
		FSourceImageThumbnailCache Cache;
		Cache.BeginFrame();
		const FSourceImageThumbnailRequest AssetRequest{
			.Identity = "/Game/Textures/Shared",
			.PhysicalPath = "C:/Project/Content/Textures/Shared.png",
			.FileSize = 256,
			.LastWriteTime = std::filesystem::file_time_type::clock::now()};
		Cache.Request(AssetRequest);
		FSourceImageThumbnailRequest SourceRequest = AssetRequest;
		SourceRequest.Identity = "C:/Project/Content/Textures/Shared.png";
		SourceRequest.Priority = Editor::EAssetThumbnailPriority::Visible;
		Cache.Request(SourceRequest);

		const Editor::FAssetThumbnailView AssetView = Cache.Find(AssetRequest.Identity);
		const Editor::FAssetThumbnailView SourceView = Cache.Find(SourceRequest.Identity);
		EXPECT_EQ(AssetView.State, Editor::EAssetThumbnailState::Queued);
		EXPECT_EQ(SourceView.State, Editor::EAssetThumbnailState::Queued);
		EXPECT_EQ(AssetView.RequestSerial, SourceView.RequestSerial);
	}

	TEST(FSourceImageThumbnailTests, ShutdownRejectsRequestsAndIsIdempotent)
	{
		FSourceImageThumbnailCache Cache;
		Cache.BeginFrame();
		Cache.Request({
			.Identity = "/Game/Textures/Pending",
			.PhysicalPath = "C:/Project/Content/Textures/Pending.png",
			.FileSize = 128,
			.LastWriteTime = std::filesystem::file_time_type::clock::now()});
		ASSERT_EQ(
			Cache.Find("/Game/Textures/Pending").State,
			Editor::EAssetThumbnailState::Queued);

		Cache.Shutdown();
		Cache.Shutdown();
		EXPECT_TRUE(Cache.IsShuttingDown());
		EXPECT_EQ(
			Cache.Find("/Game/Textures/Pending").State,
			Editor::EAssetThumbnailState::NotRequested);

		Cache.Request({
			.Identity = "/Game/Textures/Late",
			.PhysicalPath = "C:/Project/Content/Textures/Late.png",
			.FileSize = 128,
			.LastWriteTime = std::filesystem::file_time_type::clock::now()});
		Cache.BeginFrame();
		Cache.EndFrame();
		EXPECT_EQ(
			Cache.Find("/Game/Textures/Late").State,
			Editor::EAssetThumbnailState::NotRequested);
	}

	TEST(FSourceImageThumbnailTests, DecodeTasksPublishOwnerDiagnostics)
	{
		ShutdownTaskScheduler(false);
		struct FTaskSchedulerShutdownGuard
		{
			~FTaskSchedulerShutdownGuard() { ShutdownTaskScheduler(false); }
		} SchedulerGuard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		const std::array<uint8, 4> CorruptPng{0, 1, 2, 3};
		const std::filesystem::path Path = WriteBinaryFixture(
			"AttributedThumbnail.png", std::as_bytes(std::span{CorruptPng}));
		{
			FSourceImageThumbnailCache Cache;
			Cache.BeginFrame();
			Cache.Request({
				.Identity = "/Game/Textures/AttributedThumbnail",
				.PhysicalPath = Path.generic_string(),
				.FileSize = CorruptPng.size(),
				.LastWriteTime = std::filesystem::last_write_time(Path),
				.Priority = Editor::EAssetThumbnailPriority::Visible});
			Cache.EndFrame();

			Editor::EAssetThumbnailState State = Editor::EAssetThumbnailState::Loading;
			for (uint32 Attempt = 0; Attempt < 1'000 && State == Editor::EAssetThumbnailState::Loading; ++Attempt)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				Cache.BeginFrame();
				State = Cache.Find("/Game/Textures/AttributedThumbnail").State;
				Cache.EndFrame();
			}
			EXPECT_EQ(State, Editor::EAssetThumbnailState::Failed);
			Cache.Shutdown();
		}

		const FTaskSchedulerDiagnostics Diagnostics = GetTaskSchedulerDiagnostics();
		const auto Iterator = std::ranges::find_if(Diagnostics.OwnerCategoryDiagnostics, [](const FTaskOwnerCategoryDiagnostics& Entry) {
			return Entry.Owner == "SourceImageThumbnail" && Entry.Category == "Decode";
		});
		ASSERT_NE(Iterator, Diagnostics.OwnerCategoryDiagnostics.end());
		EXPECT_EQ(Iterator->AcceptedCount, 1u);
		EXPECT_EQ(Iterator->SucceededCount, 1u);
		EXPECT_EQ(Iterator->CurrentNonterminalCount, 0u);
		ShutdownTaskScheduler(true);
	}

	TEST(FSourceImageThumbnailTests, ShutdownCancelsQueuedDecodeAndRecreatedCacheStartsClean)
	{
		ShutdownTaskScheduler(false);
		struct FTaskSchedulerShutdownGuard
		{
			~FTaskSchedulerShutdownGuard() { ShutdownTaskScheduler(false); }
		} SchedulerGuard;
		ASSERT_TRUE(InitializeTaskScheduler(1));

		const auto BlockingState = std::make_shared<FBlockingTaskState>();
		const FTaskHandle Blocker = LaunchTask("BlockThumbnailWorker", [BlockingState] {
			std::unique_lock Lock(BlockingState->Mutex);
			BlockingState->bEntered = true;
			BlockingState->Condition.notify_all();
			BlockingState->Condition.wait(Lock, [&] { return BlockingState->bRelease; });
		});
		ASSERT_TRUE(Blocker.IsValid());
		{
			std::unique_lock Lock(BlockingState->Mutex);
			ASSERT_TRUE(BlockingState->Condition.wait_for(
				Lock, std::chrono::seconds(5), [&] { return BlockingState->bEntered; }));
		}

		const std::filesystem::path Path =
			WriteBinaryFixture("CanceledThumbnail.png", TransparentPngBytes());
		{
			FSourceImageThumbnailCache Cache;
			Cache.BeginFrame();
			Cache.Request({
				.Identity = "/Game/Textures/CanceledThumbnail",
				.PhysicalPath = Path.generic_string(),
				.FileSize = std::filesystem::file_size(Path),
				.LastWriteTime = std::filesystem::last_write_time(Path),
				.Priority = Editor::EAssetThumbnailPriority::Visible});
			Cache.EndFrame();

			const FTaskSchedulerDiagnostics ActiveDiagnostics =
				GetTaskSchedulerDiagnostics();
			EXPECT_EQ(ActiveDiagnostics.LiveScopeCount, 1u);
			EXPECT_EQ(ActiveDiagnostics.OpenScopeCount, 1u);
			EXPECT_EQ(ActiveDiagnostics.NonquiescentScopeCount, 1u);

			Cache.Shutdown();
			EXPECT_EQ(Cache.Find("/Game/Textures/CanceledThumbnail").State,
				Editor::EAssetThumbnailState::NotRequested);
			const FTaskSchedulerDiagnostics ClosedDiagnostics =
				GetTaskSchedulerDiagnostics();
			EXPECT_EQ(ClosedDiagnostics.LiveScopeCount, 1u);
			EXPECT_EQ(ClosedDiagnostics.OpenScopeCount, 0u);
			EXPECT_EQ(ClosedDiagnostics.NonquiescentScopeCount, 0u);
		}

		{
			std::lock_guard Lock(BlockingState->Mutex);
			BlockingState->bRelease = true;
		}
		BlockingState->Condition.notify_all();
		EXPECT_EQ(WaitTask(Blocker), ETaskState::Succeeded);
		EXPECT_EQ(GetTaskSchedulerDiagnostics().OpenScopeCount, 0u);
		EXPECT_EQ(GetTaskSchedulerDiagnostics().NonquiescentScopeCount, 0u);

		{
			FSourceImageThumbnailCache RecreatedCache;
			const FTaskSchedulerDiagnostics RestartDiagnostics =
				GetTaskSchedulerDiagnostics();
			EXPECT_EQ(RestartDiagnostics.OpenScopeCount, 1u);
			EXPECT_EQ(RestartDiagnostics.NonquiescentScopeCount, 1u);
			RecreatedCache.Shutdown();
			const FTaskSchedulerDiagnostics RestartClosedDiagnostics =
				GetTaskSchedulerDiagnostics();
			EXPECT_EQ(RestartClosedDiagnostics.OpenScopeCount, 0u);
			EXPECT_EQ(RestartClosedDiagnostics.NonquiescentScopeCount, 0u);
		}
		EXPECT_EQ(GetTaskSchedulerDiagnostics().OpenScopeCount, 0u);
		EXPECT_EQ(GetTaskSchedulerDiagnostics().NonquiescentScopeCount, 0u);
		ShutdownTaskScheduler(true);
	}

	TEST(FSourceImageThumbnailTests, DecodesTransparentPngAndPreservesAspectRatio)
	{
		// 2x1 RGBA PNG with one opaque red texel and one transparent texel.
		constexpr uint8 PngBytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
		const std::filesystem::path Path = WriteBinaryFixture(
			"ThumbnailTransparent.png", std::as_bytes(std::span{PngBytes}));
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		ASSERT_TRUE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error)) << Error;
		EXPECT_EQ(Thumbnail.Width, 2u);
		EXPECT_EQ(Thumbnail.Height, 1u);
		EXPECT_EQ(Thumbnail.Pixels.size(), 8u);
		EXPECT_TRUE(Thumbnail.bHasTransparency);
	}

	TEST(FSourceImageThumbnailTests, DecodesJpeg)
	{
		constexpr uint8 JpegBytes[] = {
			255, 216, 255, 224, 0, 16, 74, 70, 73, 70, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 255, 219, 0, 67, 0, 40, 28, 30, 35, 30, 25, 40,
			35, 33, 35, 45, 43, 40, 48, 60, 100, 65, 60, 55, 55, 60, 123, 88, 93, 73, 100, 145, 128, 153, 150, 143, 128, 140, 138, 160, 180, 230,
			195, 160, 170, 218, 173, 138, 140, 200, 255, 203, 218, 238, 245, 255, 255, 255, 155, 193, 255, 255, 255, 250, 255, 230, 253, 255, 248, 255,
			192, 0, 11, 8, 0, 1, 0, 1, 1, 1, 17, 0, 255, 196, 0, 20, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			255, 196, 0, 20, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 218, 0, 8, 1, 1, 0, 0, 63, 0, 63, 255, 217};
		const std::filesystem::path Path = WriteBinaryFixture(
			"Thumbnail.jpg", std::as_bytes(std::span{JpegBytes}));
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		ASSERT_TRUE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error)) << Error;
		EXPECT_EQ(Thumbnail.Width, 1u);
		EXPECT_EQ(Thumbnail.Height, 1u);
		EXPECT_FALSE(Thumbnail.bHasTransparency);
	}

	TEST(FSourceImageThumbnailTests, ShrinksLargeTgaToMaximumDimension)
	{
		constexpr uint32 Width = 512;
		constexpr uint32 Height = 256;
		std::vector<std::byte> TgaBytes(
			18 + static_cast<size_t>(Width) * Height * 3, std::byte{0});
		TgaBytes[2] = std::byte{2};
		TgaBytes[12] = static_cast<std::byte>(Width & 0xff);
		TgaBytes[13] = static_cast<std::byte>(Width >> 8);
		TgaBytes[14] = static_cast<std::byte>(Height & 0xff);
		TgaBytes[15] = static_cast<std::byte>(Height >> 8);
		TgaBytes[16] = std::byte{24};
		TgaBytes[17] = std::byte{0x20};
		const std::filesystem::path Path = WriteBinaryFixture("ThumbnailLarge.tga", TgaBytes);
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		ASSERT_TRUE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error)) << Error;
		EXPECT_EQ(Thumbnail.Width, 256u);
		EXPECT_EQ(Thumbnail.Height, 128u);
		EXPECT_EQ(Thumbnail.Pixels.size(), 256u * 128u * 4u);
	}

	TEST(FSourceImageThumbnailTests, RejectsCorruptImage)
	{
		constexpr uint8 Bytes[] = {1, 2, 3, 4, 5};
		const std::filesystem::path Path = WriteBinaryFixture(
			"ThumbnailCorrupt.png", std::as_bytes(std::span{Bytes}));
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		EXPECT_FALSE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error));
		EXPECT_FALSE(Error.empty());
	}

	TEST(FSourceImageThumbnailTests, RejectsOversizedImageBeforeFullResolutionDecode)
	{
		// The PNG IHDR advertises 8192 x 8192 pixels; the payload is intentionally left tiny because it must never be decoded.
		constexpr uint8 PngBytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 32, 0, 0, 0, 32, 0, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
		const std::filesystem::path Path = WriteBinaryFixture(
			"ThumbnailOversized.png", std::as_bytes(std::span{PngBytes}));
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		EXPECT_FALSE(DecodeSourceImageThumbnail(Path.generic_string(), 256, Thumbnail, Error));
		EXPECT_EQ(Error, "The decoded image is too large.");
		EXPECT_TRUE(Thumbnail.Pixels.empty());
	}

	TEST(FSourceImageThumbnailTests, PersistsResizedPngAndServesWarmInstanceWithoutSourceDecode)
	{
		const auto [CacheRoot, SourceRoot] = PrepareDiskCacheTest("WarmDiskHit");
		const std::filesystem::path Source = SourceRoot / "Texture.png";
		{
			std::ofstream Stream(Source, std::ios::binary);
			const auto Bytes = TransparentPngBytes();
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		}
		const uintmax_t FileSize = std::filesystem::file_size(Source);
		const auto LastWriteTime = std::filesystem::last_write_time(Source);
		FSourceImageThumbnailDiskCacheSettings Settings{.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot};
		FDecodedSourceImageThumbnail First;
		std::string Error;
		{
			FSourceImageThumbnailDiskCache Cache(Settings);
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, First, Error)) << Error;
			EXPECT_EQ(Cache.GetStats().SourceDecodes, 1u);
		}
		ASSERT_EQ(CountPngObjects(CacheRoot), 1u);

		FDecodedSourceImageThumbnail Warm;
		FSourceImageThumbnailDiskCache Cache(Settings);
		ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Warm, Error)) << Error;
		EXPECT_EQ(Cache.GetStats().CacheHits, 1u);
		EXPECT_EQ(Cache.GetStats().SourceDecodes, 0u);
		EXPECT_EQ(Warm.Pixels, First.Pixels);
	}

	TEST(FSourceImageThumbnailTests, InvalidatesSettingsAndRegeneratesCorruptOrMissingObjects)
	{
		const auto [CacheRoot, SourceRoot] = PrepareDiskCacheTest("DiskInvalidation");
		const std::filesystem::path Source = SourceRoot / "Texture.png";
		{
			std::ofstream Stream(Source, std::ios::binary);
			const auto Bytes = TransparentPngBytes();
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		}
		const uintmax_t FileSize = std::filesystem::file_size(Source);
		const auto LastWriteTime = std::filesystem::last_write_time(Source);
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		{
			FSourceImageThumbnailDiskCache Cache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot});
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
		}
		{
			FSourceImageThumbnailDiskCache Cache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot, .GeneratorVersion = 2});
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
			EXPECT_EQ(Cache.GetStats().SourceDecodes, 1u);
		}
		{
			FSourceImageThumbnailDiskCache Cache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot, .MaximumDimension = 128});
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
			EXPECT_EQ(Cache.GetStats().SourceDecodes, 1u);
		}
		ASSERT_EQ(CountPngObjects(CacheRoot), 3u);

		size_t CorruptObjectCount = 0;
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(CacheRoot))
			if (Entry.path().extension() == ".png")
			{
				std::ofstream Stream(Entry.path(), std::ios::binary | std::ios::trunc);
				Stream << "corrupt";
				++CorruptObjectCount;
			}
		ASSERT_GT(CorruptObjectCount, 0u);
		{
			FSourceImageThumbnailDiskCache Cache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot});
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
			EXPECT_EQ(Cache.GetStats().SourceDecodes, 1u);
			EXPECT_GE(Cache.GetStats().Regenerations, 1u);
		}
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(CacheRoot))
			if (Entry.path().extension() == ".png") std::filesystem::remove(Entry.path());
		FSourceImageThumbnailDiskCache MissingObjectCache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot});
		ASSERT_TRUE(MissingObjectCache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
		EXPECT_EQ(MissingObjectCache.GetStats().SourceDecodes, 1u);
		EXPECT_GE(MissingObjectCache.GetStats().Regenerations, 1u);

		{
			std::ofstream Stream(Source, std::ios::binary | std::ios::app);
			Stream.put('\0');
		}
		FSourceImageThumbnailDiskCache ChangedSourceCache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot});
		ASSERT_TRUE(ChangedSourceCache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
		EXPECT_EQ(ChangedSourceCache.GetStats().SourceDecodes, 1u);

		std::filesystem::remove(Source);
		FSourceImageThumbnailDiskCache MissingSourceCache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot});
		EXPECT_FALSE(MissingSourceCache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error));
		EXPECT_EQ(MissingSourceCache.GetStats().CacheHits, 0u);
	}

	TEST(FSourceImageThumbnailTests, RejectsCorruptAndIncompatibleThumbnailIndices)
	{
		const auto [CacheRoot, SourceRoot] = PrepareDiskCacheTest("IndexValidation");
		const std::filesystem::path Source = SourceRoot / "Texture.png";
		{
			std::ofstream Stream(Source, std::ios::binary);
			const auto Bytes = TransparentPngBytes();
			Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		}
		const uintmax_t FileSize = std::filesystem::file_size(Source);
		const auto LastWriteTime = std::filesystem::last_write_time(Source);
		FDecodedSourceImageThumbnail Thumbnail;
		std::string Error;
		{
			FSourceImageThumbnailDiskCache Cache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot});
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
		}
		{
			std::ofstream Stream(CacheRoot / "Index.bin", std::ios::binary | std::ios::trunc);
			Stream << "invalid index";
		}
		{
			FSourceImageThumbnailDiskCache Cache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot});
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
			EXPECT_EQ(Cache.GetStats().SourceDecodes, 1u);
		}
		FSourceImageThumbnailDiskCache IncompatibleCache({
			.CacheRoot = CacheRoot,
			.SourceIdentityRoot = SourceRoot,
			.OutputEncodingVersion = 2,
		});
		ASSERT_TRUE(IncompatibleCache.LoadOrGenerate(Source.generic_string(), FileSize, LastWriteTime, Thumbnail, Error)) << Error;
		EXPECT_EQ(IncompatibleCache.GetStats().SourceDecodes, 1u);
	}

	TEST(FSourceImageThumbnailTests, DiskBudgetCleanupStaysInsideThumbnailRoot)
	{
		const auto [CacheRoot, SourceRoot] = PrepareDiskCacheTest("DiskBudget");
		const std::filesystem::path Outside = CacheRoot.parent_path() / "outside.keep";
		{
			std::ofstream Stream(Outside);
			Stream << "authored";
		}
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			const std::filesystem::path Source = SourceRoot / std::format("Texture{}.png", Index);
			{
				std::ofstream Stream(Source, std::ios::binary);
				const auto Bytes = TransparentPngBytes();
				Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			}
			FSourceImageThumbnailDiskCache Cache({.CacheRoot = CacheRoot, .SourceIdentityRoot = SourceRoot, .DiskBudgetBytes = 128});
			FDecodedSourceImageThumbnail Thumbnail;
			std::string Error;
			ASSERT_TRUE(Cache.LoadOrGenerate(Source.generic_string(), std::filesystem::file_size(Source),
				std::filesystem::last_write_time(Source), Thumbnail, Error)) << Error;
		}
		uint64 TotalObjectBytes = 0;
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(CacheRoot))
			if (Entry.path().extension() == ".png") TotalObjectBytes += Entry.file_size();
		EXPECT_LE(TotalObjectBytes, 128u);
		EXPECT_TRUE(std::filesystem::exists(Outside));
	}
} // namespace Durin::Editor::ContentBrowser::Private
