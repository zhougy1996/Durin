#include "Import/TextureFileImport.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "AssetTools/IAssetTools.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Texture/TexturePlatformDataTestFixtures.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;

	auto MakeNormalPixels(bool bVaried) -> FByteBuffer
	{
		FByteBuffer Bytes(16 * 4);
		for (size_t Index = 0; Index < 16; ++Index)
		{
			Bytes[Index * 4] = std::byte(bVaried ? (Index % 2 ? 153 : 102) : 128);
			Bytes[Index * 4 + 1] = std::byte(128);
			Bytes[Index * 4 + 2] = std::byte(bVaried ? 252 : 255);
			Bytes[Index * 4 + 3] = std::byte(255);
		}
		return Bytes;
	}
}

TEST(FTextureFileImportTests, InfersSemanticFilenameTokensWithoutSubstringMatches)
{
	EXPECT_EQ(InferTexture2DImportSettings("/art/Brick_NORMAL.PNG").Usage, ETextureUsage::Normal);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_n.tga").Usage, ETextureUsage::Normal);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_ORM.png").Usage, ETextureUsage::DataMask);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_roughness.png").Usage, ETextureUsage::DataMask);
	EXPECT_EQ(InferTexture2DImportSettings("/normal/abnormal.png").Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_basecolor.png").Usage, ETextureUsage::Color);
}

TEST(FTextureFileImportTests, RecognizesNormalVectorsButRespectsExplicitColorAndMaskNames)
{
	Image::FImage Image;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, MakeNormalPixels(true), Image));
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 3));
	EXPECT_EQ(InferTexture2DImportSettings("unnamed.png", &Source).Usage, ETextureUsage::Normal);
	EXPECT_EQ(InferTexture2DImportSettings("wall_albedo.png", &Source).Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("wall_orm.png", &Source).Usage, ETextureUsage::DataMask);
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, MakeNormalPixels(false), Image));
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 3));
	EXPECT_EQ(InferTexture2DImportSettings("purple.png", &Source).Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("flat_normal.png", &Source).Usage, ETextureUsage::Normal);
}

TEST(FTextureFileImportTests, ImportsNormalSavesAndChoosesAvailableNames)
{
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	FModuleManager::Get().LoadModuleChecked("TextureBuild");
	const auto Root = Testing::GetTestWorkDirectory() / "TextureFileImport";
	std::filesystem::create_directories(Root / "Content");
	Testing::RegisterMountPointForTests("/TextureFileImport/", (Root / "Content").generic_string() + "/");
	FScopedDerivedDataCacheRoot Cache(Root / "Ddc");
	const auto Source = Root / "unnamed.tga";
	FByteBuffer Tga(18);
	Tga[2] = std::byte(2); Tga[12] = std::byte(4); Tga[14] = std::byte(4);
	Tga[16] = std::byte(32); Tga[17] = std::byte(0x28);
	auto Pixels = MakeNormalPixels(true);
	for (size_t Index = 0; Index < Pixels.size(); Index += 4) std::swap(Pixels[Index], Pixels[Index + 2]);
	Tga.insert(Tga.end(), Pixels.begin(), Pixels.end());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Tga, Source));
	// Occupied but uncataloged files must also reserve their names.
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Tga, Root / "Content" / "unnamed.dasset"));
	std::vector<std::string> Published;
	std::string Error;
	Editor::Texture::FTextureFileImport Importer({
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
		.AssetCreated = [&](std::string Path) { Published.push_back(std::move(Path)); }});
	for (int Index = 1; Index <= 2; ++Index)
	{
		const auto Result = Importer.ImportFile(Source.generic_string(), "/TextureFileImport/");
		ASSERT_TRUE(Result) << Error;
		ASSERT_NE(Result.Asset, nullptr);
		const auto* Texture = Cast<DTexture2D>(Result.Asset);
		ASSERT_NE(Texture, nullptr);
		EXPECT_EQ(Texture->GetUsage(), ETextureUsage::Normal);
		EXPECT_FALSE(Texture->IsSRGB());
		EXPECT_EQ(Result.Persistence, EAssetOperationPersistenceState::Persisted);
		EXPECT_EQ(Published.back(), std::format("/TextureFileImport/unnamed_{}", Index));
		EXPECT_TRUE(UnloadPackage(Result.Package));
	}
	EXPECT_FALSE(Importer.HasPendingSaves());
	EXPECT_EQ(Published.size(), 2u);
	EXPECT_EQ(std::filesystem::file_size(Source), Tga.size());
	const auto Invalid = Importer.ImportFile((Root / "missing.png").generic_string(), "/TextureFileImport/");
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Published.size(), 2u);

	int SaveAttempts = 0;
	Editor::Texture::FTextureFileImport RetryImporter({
		.AssetCreated = [&](std::string Path) { Published.push_back(std::move(Path)); }},
		[&](const FAssetSaveRequest& Request) -> FAssetOperationResult {
			if (++SaveAttempts == 1) return {.Kind = EAssetOperationKind::Save,
				.State = EAssetOperationTerminalState::Rejected, .Message = "Injected save failure"};
			return IAssetTools::Get().SaveAssets(Request);
		});
	const auto Unsaved = RetryImporter.ImportFile(Source.generic_string(), "/TextureFileImport/");
	EXPECT_FALSE(Unsaved);
	ASSERT_NE(Unsaved.Asset, nullptr);
	EXPECT_EQ(Unsaved.Persistence, EAssetOperationPersistenceState::Dirty);
	EXPECT_TRUE(RetryImporter.HasPendingSaves());
	EXPECT_EQ(Published.size(), 2u);
	ASSERT_TRUE(std::filesystem::remove(Source));
	RetryImporter.RetryPendingSaves();
	EXPECT_FALSE(RetryImporter.HasPendingSaves());
	EXPECT_EQ(SaveAttempts, 2);
	ASSERT_EQ(Published.size(), 3u);
	EXPECT_EQ(Published.back(), "/TextureFileImport/unnamed_3");
}

namespace
{
	class FTextureImportQueueTests : public testing::Test
	{
	protected:
		std::filesystem::path Root, Source;
		std::string Destination;
		std::unique_ptr<FScopedDerivedDataCacheRoot> Cache;
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
			FModuleManager::Get().LoadModuleChecked("TextureBuild");
			const std::string TestName = testing::UnitTest::GetInstance()->current_test_info()->name();
			Root = Testing::CreateTestFixtureDirectory(TestName);
			Destination = "/" + TestName + "/";
			std::filesystem::create_directories(Root / "Content");
			Testing::RegisterMountPointForTests(Destination, (Root / "Content").generic_string() + "/");
			Cache = std::make_unique<FScopedDerivedDataCacheRoot>(Root / "Ddc");
			Source = Root / "wall_normal.tga";
			FByteBuffer Tga(18);
			Tga[2] = std::byte(2); Tga[12] = std::byte(4); Tga[14] = std::byte(4);
			Tga[16] = std::byte(32); Tga[17] = std::byte(0x28);
			auto Pixels = MakeNormalPixels(true);
			for (size_t Index = 0; Index < Pixels.size(); Index += 4) std::swap(Pixels[Index], Pixels[Index + 2]);
			Tga.insert(Tga.end(), Pixels.begin(), Pixels.end());
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Tga, Source));
		}
		auto Drain(Editor::Texture::FTextureFileImport& Importer) -> bool
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (Importer.IsRunning() && std::chrono::steady_clock::now() < Deadline)
			{
				FAssetCompilingManager::Get().ProcessAsyncTasks();
				Importer.Tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return !Importer.IsRunning();
		}
	};
}

TEST_F(FTextureImportQueueTests, BatchContinuesAfterFailureAndPresentsOnce)
{
	const auto Second = Root / "other_normal.tga";
	std::filesystem::copy_file(Source, Second);
	int Assets = 0, Directories = 0;
	Editor::Texture::FTextureFileImport Importer({
		.AssetCreated = [&](std::string) { ++Assets; },
		.ImportedDirectory = [&](std::string Directory) { EXPECT_EQ(Directory, Destination); ++Directories; }});
	ASSERT_TRUE(Importer.Begin({(Root / "missing.png").generic_string(), Source.generic_string(),
		Second.generic_string(), Source.generic_string()}, Destination));
	EXPECT_FALSE(Importer.Begin({Source.generic_string()}, Destination));
	EXPECT_EQ(Importer.GetTotalCount(), 3u);
	Importer.Tick(false);
	EXPECT_EQ(Importer.GetCompletedCount(), 0u);
	EXPECT_EQ(Assets + Directories, 0);
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetSavedCount(), 2u);
	EXPECT_EQ(Importer.GetFailedCount(), 1u);
	EXPECT_EQ(Importer.GetCanceledCount(), 0u);
	EXPECT_EQ(Assets, 0);
	EXPECT_EQ(Directories, 1);
	EXPECT_NE(Importer.GetTimingDetails().find("wall_normal.tga: prepare"), std::string::npos);
	EXPECT_NE(Importer.GetTimingDetails().find("cache hit"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content" / "other_normal.dasset"));
}

TEST_F(FTextureImportQueueTests, CancelSkipsQueuedFilesButFinishesActiveCapture)
{
	Editor::Texture::FTextureFileImport Importer;
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	Importer.Cancel();
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetCanceledCount(), 1u);
	EXPECT_EQ(Importer.GetSavedCount(), 0u);
	ASSERT_TRUE(Importer.Begin({Source.generic_string(), (Root / "missing.png").generic_string()}, Destination));
	Importer.Tick(); // Starts the detached capture before cancellation.
	Importer.Cancel();
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetSavedCount(), 1u);
	EXPECT_EQ(Importer.GetCanceledCount(), 1u);
	EXPECT_EQ(Importer.GetFailedCount(), 0u);
}

TEST_F(FTextureImportQueueTests, FailedBatchSaveRetainsAssetForRetryWithoutSource)
{
	int Attempts = 0, Published = 0;
	Editor::Texture::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }},
		[&](const FAssetSaveRequest& Request) -> FAssetOperationResult {
			if (++Attempts == 1) return {.Kind = EAssetOperationKind::Save,
				.State = EAssetOperationTerminalState::Rejected, .Message = "Injected save failure"};
			return IAssetTools::Get().SaveAssets(Request);
		});
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	ASSERT_TRUE(Drain(Importer));
	EXPECT_TRUE(Importer.HasPendingSaves());
	EXPECT_EQ(Importer.GetFailedCount(), 1u);
	EXPECT_EQ(Published, 0);
	ASSERT_TRUE(std::filesystem::remove(Source));
	Importer.Tick(false);
	EXPECT_FALSE(Importer.CanRetrySaves());
	Importer.RetryPendingSaves();
	EXPECT_TRUE(Importer.HasPendingSaves());
	EXPECT_EQ(Attempts, 1);
	Importer.Tick(true);
	Importer.RetryPendingSaves();
	EXPECT_FALSE(Importer.HasPendingSaves());
	EXPECT_EQ(Published, 1);
	EXPECT_EQ(Attempts, 2);
}

TEST_F(FTextureImportQueueTests, TeardownJoinsCaptureWithoutPublishing)
{
	int Published = 0;
	{
		Editor::Texture::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }});
		ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
		Importer.Tick();
	}
	EXPECT_EQ(Published, 0);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
}

TEST_F(FTextureImportQueueTests, TeardownDrainsActiveCompilationAndDiscardsCandidate)
{
	int Published = 0;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(Destination + "wall_normal", Path));
	{
		Editor::Texture::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }});
		ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!FindResidentPackage(Path) && std::chrono::steady_clock::now() < Deadline)
		{
			Importer.Tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ASSERT_NE(FindResidentPackage(Path), nullptr);
	}
	EXPECT_EQ(Published, 0);
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
}
