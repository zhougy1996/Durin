#include "AssetSystem.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "Texture/Texture2D.h"

#include <gtest/gtest.h>

namespace
{
	constexpr Durin::uint8 TransparentPngBytes[] = {
		137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
		0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
		0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

	auto WriteTextureFixture(const std::filesystem::path& Path) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(TransparentPngBytes), sizeof(TransparentPngBytes));
	}
}

TEST(FTexture2DTests, ImportsSourceAndBuildsIndependentPlatformData)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureImports";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/TextureImportTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Transparent");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTextureSourceData* SourceData = Result.Asset->GetSourceData();
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(SourceData, nullptr);
	ASSERT_NE(PlatformData, nullptr);
	EXPECT_NE(Result.Asset->GetRenderResource(), nullptr);
	EXPECT_EQ(Result.Asset->GetBuildRevision(), 1u);
	EXPECT_TRUE(SourceData->IsValid());
	EXPECT_TRUE(SourceData->bHasTransparency);
	EXPECT_EQ(SourceData->Width, 2u);
	EXPECT_EQ(SourceData->Height, 1u);
	ASSERT_TRUE(PlatformData->IsValid());
	EXPECT_TRUE(Result.Asset->IsSRGB());
	EXPECT_EQ(PlatformData->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	ASSERT_EQ(PlatformData->Mips.size(), 1u);
	EXPECT_EQ(PlatformData->Mips[0].Pixels, SourceData->Pixels);
	EXPECT_NE(PlatformData->Mips[0].Pixels.data(), SourceData->Pixels.data());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transparent", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_TRUE(Loaded->GetSourceData()->IsValid());
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_EQ(Loaded->GetBuildRevision(), 1u);
	EXPECT_EQ(Loaded->GetSourceFile(), "Transparent.png");
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::FAssetPath RenamedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Renamed", RenamedPath));
	ASSERT_TRUE(Durin::Asset::MoveAsset(AssetPath, RenamedPath));
	const std::filesystem::path ImportRoot = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureImports";
	EXPECT_FALSE(std::filesystem::exists(ImportRoot / "Transparent.png"));
	EXPECT_TRUE(std::filesystem::is_regular_file(ImportRoot / "Renamed.png"));
	ASSERT_TRUE(Durin::Asset::LoadAsset(RenamedPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceFile(), "Renamed.png");
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(RenamedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(RenamedPath));
	EXPECT_FALSE(std::filesystem::exists(ImportRoot / "Renamed.png"));
}

TEST(FTexture2DTests, PreservesLinearBuildSettingAndRebuildsColorSpace)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "LinearTextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.bSRGB = false;
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Linear", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Asset->IsSRGB());
	ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::RGBA8_UNORM);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Linear", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::RGBA8_UNORM);

	const std::vector<Durin::uint8> SourcePixels = Loaded->GetSourceData()->Pixels;
	std::string Error;
	ASSERT_TRUE(Loaded->SetSRGB(true, Error)) << Error;
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::SRGBA8_UNORM);
	EXPECT_EQ(Loaded->GetPlatformData()->Mips[0].Pixels, SourcePixels);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, RejectsUnsupportedSourceWithoutCreatingAsset)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "UnsupportedTexture.gif";
	std::ofstream(Source, std::ios::binary | std::ios::trunc) << "not an image";
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Unsupported");
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Message.empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Unsupported", AssetPath));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
}
