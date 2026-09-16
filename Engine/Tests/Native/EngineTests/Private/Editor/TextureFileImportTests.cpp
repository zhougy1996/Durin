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
