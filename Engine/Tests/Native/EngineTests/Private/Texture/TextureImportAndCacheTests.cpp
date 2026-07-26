#include "TextureTestSupport.h"

TEST(FTexture2DTests, ImportsSourceAndBuildsIndependentPlatformData)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureImportDerivedDataCache");

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
	EXPECT_EQ(Result.Asset->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(PlatformData->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_EQ(PlatformData->Mips.size(), 2u);
	EXPECT_EQ(PlatformData->Mips[0].RowPitch, 16u);
	EXPECT_EQ(PlatformData->Mips[0].Pixels.size(), 16u);
	EXPECT_EQ(PlatformData->Mips[1].Width, 1u);
	EXPECT_EQ(PlatformData->Mips[1].Height, 1u);
	EXPECT_EQ(PlatformData->Mips[1].RowPitch, 16u);
	EXPECT_EQ(PlatformData->Mips[1].Pixels.size(), 16u);
	ExpectPixelNear(DecodeFirstCompressedPixel(PlatformData->PixelFormat, PlatformData->Mips[1].Pixels),
		{188, 0, 0, 128});

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transparent", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceWidth(), 2u);
	EXPECT_EQ(Loaded->GetSourceHeight(), 1u);
	EXPECT_EQ(Loaded->GetSourceChannelCount(), 4u);
	EXPECT_TRUE(Loaded->SourceHasTransparency());
	EXPECT_TRUE(Loaded->GetPlatformData()->IsValid());
	EXPECT_EQ(Loaded->GetBuildRevision(), 1u);
	EXPECT_EQ(Loaded->GetSourceFile(), "Transparent.png");
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
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

TEST(FTexture2DTests, VersionedDerivedDataCacheHitsAndRecoversCorruptPayload)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureDerivedDataMount";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/TextureDerivedDataTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureDerivedDataCache");

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "DerivedDataSource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureDerivedDataTests/Cached");
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Asset->GetSourceContentHash().empty());
	EXPECT_EQ(Result.Asset->GetSourceContentHash().size(), 32u);
	EXPECT_FALSE(Result.Asset->GetDerivedDataKey().empty());
	EXPECT_FALSE(Result.Asset->WasLoadedFromDerivedDataCache());
	const std::filesystem::path CachePath = GetTextureCachePath(*Result.Asset);
	const std::string OriginalKey = Result.Asset->GetDerivedDataKey();
	EXPECT_TRUE(std::filesystem::is_regular_file(CachePath));
	const Durin::FTexturePlatformData ExpectedPlatformData = *Result.Asset->GetPlatformData();

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureDerivedDataTests/Cached", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Loaded->GetPlatformData(), ExpectedPlatformData);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	{
		const std::array<Durin::uint8, 7> CorruptBytes = {0, 1, 2, 3, 4, 5, 6};
		std::ofstream Stream(CachePath, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_FALSE(Loaded->WasLoadedFromDerivedDataCache());
	ASSERT_NE(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Loaded->GetPlatformData(), ExpectedPlatformData);
	EXPECT_GT(std::filesystem::file_size(CachePath), 7u);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));

	const std::filesystem::path CopiedSource =
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureDerivedDataMount" / "Cached.png";
	WriteNpotTextureFixture(CopiedSource);
	std::filesystem::last_write_time(CopiedSource,
		std::filesystem::last_write_time(CopiedSource) + std::chrono::seconds(1));
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_FALSE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_NE(Loaded->GetDerivedDataKey(), OriginalKey);
	EXPECT_EQ(Loaded->GetSourceWidth(), 5u);
	EXPECT_EQ(Loaded->GetSourceHeight(), 3u);
	EXPECT_TRUE(Loaded->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));
	ASSERT_TRUE(Durin::Asset::SavePackage(Loaded->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	EXPECT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, DerivedDataKeyCoversSourceContentAndBuildSettings)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureDerivedDataKeyCache");
	const std::filesystem::path FirstSource = std::filesystem::path(DURIN_TEST_WORK_DIR) / "DerivedKeyFirst.png";
	const std::filesystem::path SecondSource = std::filesystem::path(DURIN_TEST_WORK_DIR) / "DerivedKeySecond.tga";
	WriteTextureFixture(FirstSource);
	WriteNpotTextureFixture(SecondSource);

	const Durin::FTexture2DImportResult First = Durin::DTexture2D::ImportAsset(
		FirstSource.generic_string(), "/TextureImportTests/DerivedKeyFirst");
	const Durin::FTexture2DImportResult Second = Durin::DTexture2D::ImportAsset(
		SecondSource.generic_string(), "/TextureImportTests/DerivedKeySecond");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_TRUE(Second) << Second.Message;
	ASSERT_NE(First.Asset, nullptr);
	ASSERT_NE(Second.Asset, nullptr);
	EXPECT_NE(First.Asset->GetSourceContentHash(), Second.Asset->GetSourceContentHash());
	EXPECT_NE(First.Asset->GetDerivedDataKey(), Second.Asset->GetDerivedDataKey());

	Durin::FAssetPath FirstPath;
	Durin::FAssetPath SecondPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/DerivedKeyFirst", FirstPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/DerivedKeySecond", SecondPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(FirstPath, Loaded));
	ASSERT_TRUE(Loaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Loaded->GetSourceData(), nullptr);
	const std::string OriginalKey = Loaded->GetDerivedDataKey();
	std::string Error;
	ASSERT_TRUE(Loaded->SetMaxResolution(1, Error)) << Error;
	EXPECT_NE(Loaded->GetSourceData(), nullptr);
	EXPECT_NE(Loaded->GetDerivedDataKey(), OriginalKey);
	EXPECT_TRUE(std::filesystem::is_regular_file(GetTextureCachePath(*Loaded)));

	ASSERT_TRUE(Durin::Asset::UnloadPackage(FirstPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SecondPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(FirstPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(SecondPath));
}
