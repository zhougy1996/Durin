#include "TextureTestSupport.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTexture.h"
#include "Texture/VolumeTextureBuildOperations.h"
#include "Texture/VolumeTextureBuilder.h"

TEST(FVolumeTextureTests, BuildsDeterministicOddThreeAxisMipChain)
{
	Durin::FVolumeTextureSourceData Source;
	Source.Width = 3;
	Source.Height = 3;
	Source.Depth = 3;
	Source.Format = Durin::EVolumeTextureFormat::R8_UNORM;
	Source.Voxels.resize(27);
	std::iota(Source.Voxels.begin(), Source.Voxels.end(), Durin::uint8{0});
	Durin::FVolumeTexturePlatformData First;
	Durin::FVolumeTexturePlatformData Second;
	std::string Error;
	const Durin::FVolumeTextureBuildSettings Settings{};
	ASSERT_TRUE(Durin::Asset::Build::VolumeTextureBuilder::BuildMipChain(
		Source, Settings, First, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::VolumeTextureBuilder::BuildMipChain(
		Source, Settings, Second, Error)) << Error;
	ASSERT_EQ(First.Mips.size(), 2u);
	EXPECT_EQ(First.Mips[1].Width, 1u);
	EXPECT_EQ(First.Mips[1].Height, 1u);
	EXPECT_EQ(First.Mips[1].Depth, 1u);
	EXPECT_EQ(First.Mips[1].Voxels, (std::vector<Durin::uint8>{7}));
	EXPECT_EQ(First.Mips[0].Voxels, Second.Mips[0].Voxels);
	EXPECT_EQ(First.Mips[1].Voxels, Second.Mips[1].Voxels);
}

TEST(FVolumeTextureTests, PayloadRoundTripsAndRejectsCorruption)
{
	Durin::FVolumeTextureSourceData Source{
		.Voxels = {0, 32, 64, 96, 128, 160, 192, 255},
		.Width = 2, .Height = 2, .Depth = 2,
		.Format = Durin::EVolumeTextureFormat::R8_UNORM};
	Durin::FVolumeTexturePlatformData Platform;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Build::VolumeTextureBuilder::BuildMipChain(
		Source, {}, Platform, Error)) << Error;
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::BuildVolumeTextureSerializedValue(Platform,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Bytes, Error)) << Error;
	std::unique_ptr<Durin::FVolumeTexturePlatformData> Decoded;
	Durin::EPayloadDecodeError Code = Durin::EPayloadDecodeError::Corrupt;
	ASSERT_TRUE(Durin::ParseVolumeTextureSerializedValue(Bytes,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Decoded, Error, Code)) << Error;
	ASSERT_NE(Decoded, nullptr);
	EXPECT_EQ(Decoded->Mips.back().Voxels, Platform.Mips.back().Voxels);
	Bytes.back() ^= 1;
	EXPECT_FALSE(Durin::ParseVolumeTextureSerializedValue(Bytes,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Decoded, Error, Code));
	EXPECT_NE(Error.find("checksum"), std::string::npos);
}

TEST(FVolumeTextureTests, DdcBuildIsStableAndKeySensitive)
{
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "VolumeTextureBuildDdc");
	Durin::FVolumeTextureSourceData Source{
		.Voxels = {1, 2, 3, 4, 5, 6, 7, 8},
		.Width = 2, .Height = 2, .Depth = 2,
		.Format = Durin::EVolumeTextureFormat::R8_UNORM};
	Durin::Asset::Build::FVolumeTextureBuildProduct First;
	Durin::Asset::Build::FVolumeTextureBuildProduct Second;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Build::BuildVolumeTexture(Source, {}, First, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::BuildVolumeTexture(Source, {}, Second, Error)) << Error;
	EXPECT_EQ(First.DerivedDataKey, Second.DerivedDataKey);
	EXPECT_TRUE(Second.bCacheHit);
	Source.Voxels[0] = 9;
	Durin::Asset::Build::FVolumeTextureBuildProduct Changed;
	ASSERT_TRUE(Durin::Asset::Build::BuildVolumeTexture(Source, {}, Changed, Error)) << Error;
	EXPECT_NE(First.DerivedDataKey, Changed.DerivedDataKey);
}

TEST(FVolumeTextureTests, PackageReloadCookAndFailedReplacementAreTransactional)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "VolumeTextureAssetDdc");
	Durin::FVolumeTextureSourceData Source{
		.Voxels = {1, 2, 3, 4, 5, 6, 7, 8},
		.Width = 2, .Height = 2, .Depth = 2,
		.Format = Durin::EVolumeTextureFormat::R8_UNORM};
	Durin::Asset::Build::FVolumeTextureBuildProduct Product;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Build::BuildVolumeTexture(Source, {}, Product, Error)) << Error;
	ASSERT_NE(Product.PlatformData, nullptr);
	const Durin::FVolumeTexturePlatformData Expected = *Product.PlatformData;
	const std::string ExpectedKey = Product.DerivedDataKey;

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/VolumePackage", AssetPath));
	Durin::DVolumeTexture* Texture = nullptr;
	const Durin::Asset::FAssetResult Created = Durin::Asset::CreateAsset(AssetPath, Texture);
	ASSERT_TRUE(Created) << Created.Message;
	ASSERT_NE(Texture, nullptr);
	ASSERT_TRUE(Texture->PublishBuiltData(Source, {},
		std::make_unique<Durin::FVolumeTexturePlatformData>(*Product.PlatformData),
		Product.DerivedDataKey, Error)) << Error;
	const Durin::uint64 ValidRevision = Texture->GetBuildRevision();
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_FALSE(Texture->PublishBuiltData({}, {}, nullptr, "invalid", Error));
	EXPECT_EQ(Texture->GetBuildRevision(), ValidRevision);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Voxels,
		Expected.Mips.front().Voxels);
	const Durin::Asset::FAssetResult Saved = Durin::Asset::SavePackage(Texture->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Texture = nullptr;
	const Durin::Asset::FAssetResult Loaded = Durin::Asset::LoadAsset(AssetPath, Texture);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_EQ(Texture->GetDerivedDataKey(), ExpectedKey);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Voxels,
		Expected.Mips.front().Voxels);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "VolumeTextureCook");
	Durin::Testing::RemoveTestWorkDirectory(CookRoot);
	Durin::Asset::FCookContext Cook(CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Texture->AddToCook(Cook, "/Game/CookedVolume", Error)) << Error;
	ASSERT_TRUE(Cook.Publish(&Error)) << Error;
	std::vector<Durin::uint8> BulkBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BulkBytes,
		(CookRoot / "Game/CookedVolume.dbulk").generic_string()));
	Durin::Asset::FCookedBulkContainer Container;
	ASSERT_TRUE(Durin::Asset::DecodeCookedBulk(BulkBytes,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Container, &Error)) << Error;
	ASSERT_EQ(Container.Entries.size(), 1u);
	EXPECT_EQ(Container.Entries.front().PayloadId,
		Durin::VolumeTexturePrimaryCookedPayloadId);
	Durin::FVolumeTexturePlatformData Decoded;
	Durin::FCanonicalMemoryReader Reader(Container.Payloads.front(),
		Durin::EArchivePurpose::CookedPayload);
	Decoded.Serialize(Reader, {
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game});
	ASSERT_FALSE(Reader.HasError());
	EXPECT_EQ(Decoded.Mips.front().Voxels, Expected.Mips.front().Voxels);
	EXPECT_EQ(Decoded.Mips.back().Voxels, Expected.Mips.back().Voxels);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Texture = nullptr;
	Durin::Asset::ShutdownAssetManager();
	Durin::CollectGarbage();
	auto CookedConfiguration = Durin::Asset::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Durin::Asset::FAssetRuntimeConfiguration::Cooked(
		CookRoot, CookedConfiguration));
	ASSERT_TRUE(Durin::Asset::InitializeAssetManager(std::move(CookedConfiguration)));
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::Asset::RefreshAssetCatalog(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	Durin::FAssetPath CookedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedVolume", CookedPath));
	Durin::DVolumeTexture* CookedTexture = nullptr;
	const Durin::Asset::FAssetResult CookedLoad =
		Durin::Asset::LoadAsset(CookedPath, CookedTexture);
	ASSERT_TRUE(CookedLoad) << CookedLoad.Message;
	ASSERT_NE(CookedTexture, nullptr);
	ASSERT_NE(CookedTexture->GetPlatformData(), nullptr);
	EXPECT_FALSE(CookedTexture->GetSourceData().IsValid());
	EXPECT_TRUE(CookedTexture->GetDerivedDataKey().empty());
	EXPECT_EQ(CookedTexture->GetPlatformData()->Mips.front().Voxels,
		Expected.Mips.front().Voxels);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CookedPath));
	Durin::Asset::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Asset::InitializeAssetManager());
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FVolumeTextureTests, BuildsAllPortableFormatsAcrossDegenerateAxes)
{
	const std::array Formats{
		Durin::EVolumeTextureFormat::R8_UNORM,
		Durin::EVolumeTextureFormat::RG8_UNORM,
		Durin::EVolumeTextureFormat::RGBA8_UNORM,
		Durin::EVolumeTextureFormat::R16_FLOAT,
		Durin::EVolumeTextureFormat::RGBA16_FLOAT};
	const std::array<Durin::uint32, 5> BytesPerVoxel{1, 2, 4, 2, 8};
	for (size_t Index = 0; Index < Formats.size(); ++Index)
	{
		Durin::FVolumeTextureSourceData Source;
		Source.Width = 1;
		Source.Height = 3;
		Source.Depth = 5;
		Source.Format = Formats[Index];
		Source.Voxels.assign(15 * BytesPerVoxel[Index], 0);
		Durin::FVolumeTextureBuildSettings Settings;
		Settings.OutputFormat = Formats[Index];
		Durin::FVolumeTexturePlatformData Platform;
		std::string Error;
		ASSERT_TRUE(Durin::Asset::Build::VolumeTextureBuilder::BuildMipChain(
			Source, Settings, Platform, Error)) << Error;
		ASSERT_EQ(Platform.Mips.size(), 3u);
		const std::array<Durin::uint32, 3> MiddleExtent{
			Platform.Mips[1].Width, Platform.Mips[1].Height, Platform.Mips[1].Depth};
		const std::array<Durin::uint32, 3> TailExtent{
			Platform.Mips[2].Width, Platform.Mips[2].Height, Platform.Mips[2].Depth};
		EXPECT_EQ(MiddleExtent, (std::array<Durin::uint32, 3>{1, 1, 2}));
		EXPECT_EQ(TailExtent, (std::array<Durin::uint32, 3>{1, 1, 1}));
		EXPECT_TRUE(Platform.IsValid());
	}
}

TEST(FTexture2DTests, StandardTranslationFeedsDetachedNormalizedBuildProduct)
{
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "NormalizedTexture2DBuildDdc");
	Durin::FTextureSourceData SourceData;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::TranslateTexture2DSource(
		TransparentPngBytes, SourceData, Error)) << Error;
	ASSERT_TRUE(SourceData.IsValid());
	EXPECT_EQ(SourceData.Width, 2u);
	EXPECT_EQ(SourceData.Height, 1u);

	const Durin::FXxHash128 SourceHash = Durin::FXxHash128::HashBuffer(TransparentPngBytes);
	Durin::Asset::Build::FTexture2DBuildProduct Product;
	ASSERT_TRUE(Durin::Asset::Build::BuildTexture2D({
		.SourceData = std::move(SourceData),
		.SourceContentHashLow = SourceHash.HashLow,
		.SourceContentHashHigh = SourceHash.HashHigh}, Product, Error)) << Error;
	EXPECT_TRUE(Product.SourceData.IsValid());
	EXPECT_TRUE(Product.PlatformData.IsValid());
	EXPECT_FALSE(Product.DerivedDataKey.empty());

	Product.DerivedDataKey = "sentinel";
	EXPECT_FALSE(Durin::Asset::Build::BuildTexture2D({}, Product, Error));
	EXPECT_TRUE(Product.DerivedDataKey.empty());
}

TEST(FTexture2DTests, BuildOwnedAuthoringServicePublishesLatestNormalizedProduct)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "Texture2DAuthoringServiceDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "Texture2DAuthoringService.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Imported =
		Durin::Asset::Forge::ImportTexture2DAsset(
			Source.generic_string(), "/TextureImportTests/AuthoringService");
	ASSERT_TRUE(Imported) << Imported.Message;

	Durin::FTextureSourceData SourceData;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::TranslateTexture2DSource(
		TransparentPngBytes, SourceData, Error)) << Error;
	const Durin::FXxHash128 SourceHash =
		Durin::FXxHash128::HashBuffer(TransparentPngBytes);
	ASSERT_TRUE(Durin::Asset::Build::SubmitTexture2DBuild(*Imported.Asset, {
		.SourceData = std::move(SourceData),
		.SourceContentHashLow = SourceHash.HashLow,
		.SourceContentHashHigh = SourceHash.HashHigh,
		.SourcePath = Imported.Asset->GetSourceImportData().Source.SourcePath,
		.Settings = {.MaxResolution = 1},
		.DecoderId = "DurinImage",
		.DecoderVersion = 1,
		.SourceFileSize = sizeof(TransparentPngBytes),
		.Priority = Durin::Asset::Build::ETexture2DBuildPriority::Interactive}, Error)) << Error;
	EXPECT_TRUE(Durin::Asset::Build::HasPendingTexture2DBuild(*Imported.Asset));
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Imported.Asset, 10.0));
	EXPECT_FALSE(Durin::Asset::Build::HasPendingTexture2DBuild(*Imported.Asset));
	EXPECT_EQ(Imported.Asset->GetMaxResolution(), 1u);
	ASSERT_NE(Imported.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Imported.Asset->GetPlatformData()->Mips.front().Width, 1u);
}

TEST(FTexture2DTests, UsagePresetsChooseColorSpaceAndMipFilter)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "UsagePresetSource.png";
	WriteTextureFixture(Source);

	struct FExpectedPreset
	{
		Durin::ETextureUsage Usage;
		std::string_view AssetName;
		Durin::EPixelFormat PixelFormat;
		std::array<Durin::uint8, 4> ExpectedPixel;
	};
	const std::array Presets = {
		FExpectedPreset{Durin::ETextureUsage::Color, "PresetColor", Durin::EPixelFormat::BC3_UNORM_SRGB, {188, 0, 0, 128}},
		FExpectedPreset{Durin::ETextureUsage::Normal, "PresetNormal", Durin::EPixelFormat::BC5_UNORM, {128, 37, 0, 0}},
		FExpectedPreset{Durin::ETextureUsage::DataMask, "PresetDataMask", Durin::EPixelFormat::BC7_UNORM, {128, 0, 0, 128}}
	};

	for (const FExpectedPreset& Preset : Presets)
	{
		Durin::FTexture2DImportSettings Settings;
		Settings.Usage = Preset.Usage;
		const std::string AssetPathString = std::format("/TextureImportTests/{}", Preset.AssetName);
		Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(Source.generic_string(), AssetPathString, Settings);
		ASSERT_TRUE(Result) << Result.Message;
		ASSERT_NE(Result.Asset, nullptr);
		EXPECT_EQ(Result.Asset->GetUsage(), Preset.Usage);
		EXPECT_EQ(Result.Asset->IsSRGB(), Preset.Usage == Durin::ETextureUsage::Color);
		ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
		EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Preset.PixelFormat);
		ASSERT_EQ(Result.Asset->GetPlatformData()->Mips.size(), 2u);
		ExpectPixelNear(DecodeFirstCompressedPixel(Preset.PixelFormat,
			Result.Asset->GetPlatformData()->Mips.back().Pixels), Preset.ExpectedPixel);

		Durin::FAssetPath AssetPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(AssetPathString, AssetPath));
		ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
		Durin::DTexture2D* Loaded = nullptr;
		ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
		ASSERT_NE(Loaded, nullptr);
		EXPECT_EQ(Loaded->GetUsage(), Preset.Usage);
		EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Preset.PixelFormat);
		ExpectPixelNear(DecodeFirstCompressedPixel(Preset.PixelFormat,
			Loaded->GetPlatformData()->Mips.back().Pixels), Preset.ExpectedPixel);
		ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
		ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
	}
}

TEST(FTexture2DTests, BuildsCompleteNpotMipChainWithoutDroppingEdges)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "NpotTextureSource.tga";
	WriteNpotTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.Usage = Durin::ETextureUsage::DataMask;
	Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(Source.generic_string(), "/TextureImportTests/Npot", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(PlatformData, nullptr);
	EXPECT_EQ(PlatformData->PixelFormat, Durin::EPixelFormat::BC7_UNORM);
	ASSERT_EQ(PlatformData->Mips.size(), 3u);
	EXPECT_EQ(std::pair(PlatformData->Mips[0].Width, PlatformData->Mips[0].Height), std::pair(5u, 3u));
	EXPECT_EQ(std::pair(PlatformData->Mips[1].Width, PlatformData->Mips[1].Height), std::pair(2u, 1u));
	EXPECT_EQ(std::pair(PlatformData->Mips[2].Width, PlatformData->Mips[2].Height), std::pair(1u, 1u));
	ExpectPixelNear(DecodeFirstCompressedPixel(PlatformData->PixelFormat, PlatformData->Mips[2].Pixels),
		{43, 43, 43, 255});

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Npot", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));

	Durin::FTexture2DImportResult ColorResult = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/NpotColor");
	ASSERT_TRUE(ColorResult) << ColorResult.Message;
	ASSERT_NE(ColorResult.Asset, nullptr);
	ASSERT_NE(ColorResult.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(ColorResult.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC1_UNORM_SRGB);
	EXPECT_TRUE(ColorResult.Asset->GetPlatformData()->IsValid());
	Durin::FAssetPath ColorAssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/NpotColor", ColorAssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ColorAssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(ColorAssetPath));
}

TEST(FTexture2DTests, MaximumResolutionSelectsMipAlignedBaseLevel)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "LimitedTextureSource.tga";
	WriteNpotTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.MaxResolution = 4;
	Settings.CompressionQuality = Durin::ETextureCompressionQuality::Low;
	Settings.AlphaMipMode = Durin::ETextureAlphaMipMode::PreserveCoverage;
	Settings.AlphaCoverageThreshold = 0.4f;
	Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/Limited", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_EQ(Result.Asset->GetMaxResolution(), 4u);
	EXPECT_EQ(Result.Asset->GetCompressionQuality(), Durin::ETextureCompressionQuality::Low);
	EXPECT_EQ(Result.Asset->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	EXPECT_FLOAT_EQ(Result.Asset->GetAlphaCoverageThreshold(), 0.4f);
	const Durin::FTexturePlatformData* PlatformData = Result.Asset->GetPlatformData();
	ASSERT_NE(PlatformData, nullptr);
	ASSERT_EQ(PlatformData->Mips.size(), 2u);
	EXPECT_EQ(std::pair(PlatformData->Mips[0].Width, PlatformData->Mips[0].Height), std::pair(2u, 1u));
	EXPECT_EQ(std::pair(PlatformData->Mips[1].Width, PlatformData->Mips[1].Height), std::pair(1u, 1u));

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Limited", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_EQ(Loaded->GetMaxResolution(), 4u);
	EXPECT_EQ(Loaded->GetCompressionQuality(), Durin::ETextureCompressionQuality::Low);
	EXPECT_EQ(Loaded->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	EXPECT_FLOAT_EQ(Loaded->GetAlphaCoverageThreshold(), 0.4f);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_EQ(Loaded->GetPlatformData()->Mips.front().Width, 2u);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, PreservesMaskedAlphaCoverageWithoutChangingColor)
{
	Durin::FTextureSourceData Source;
	Source.Width = 8;
	Source.Height = 8;
	Source.SourceChannelCount = 4;
	Source.Format = Durin::ETextureSourceFormat::RGBA8;
	Source.bHasTransparency = true;
	Source.Pixels.resize(8 * 8 * 4);
	constexpr std::array<Durin::uint8, 16> OpaqueCounts = {
		3, 3, 3, 3,
		3, 2, 2, 1,
		0, 0, 0, 0,
		0, 0, 0, 0
	};
	for (Durin::uint32 BlockY = 0; BlockY < 4; ++BlockY)
	{
		for (Durin::uint32 BlockX = 0; BlockX < 4; ++BlockX)
		{
			const Durin::uint8 OpaqueCount = OpaqueCounts[BlockY * 4 + BlockX];
			for (Durin::uint32 Pixel = 0; Pixel < 4; ++Pixel)
			{
				const Durin::uint32 X = BlockX * 2 + Pixel % 2;
				const Durin::uint32 Y = BlockY * 2 + Pixel / 2;
				const size_t Offset = (static_cast<size_t>(Y) * Source.Width + X) * 4;
				Source.Pixels[Offset] = static_cast<Durin::uint8>(X * 24);
				Source.Pixels[Offset + 1] = static_cast<Durin::uint8>(Y * 24);
				Source.Pixels[Offset + 2] = 64;
				Source.Pixels[Offset + 3] = Pixel < OpaqueCount ? 255 : 0;
			}
		}
	}

	Durin::FTexturePlatformData Average;
	Durin::FTexturePlatformData Preserved;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Build::TextureBuilder::BuildMipChain(Source, Durin::ETextureUsage::Color, false,
		Average, Error, 0, Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::Average, 0.5f)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::TextureBuilder::BuildMipChain(Source, Durin::ETextureUsage::Color, false,
		Preserved, Error, 0, Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::PreserveCoverage, 0.5f)) << Error;
	ASSERT_GE(Average.Mips.size(), 2u);
	ASSERT_EQ(Preserved.Mips.size(), Average.Mips.size());

	const std::vector<Durin::uint8> AveragePixels = DecodeBC3Mip(Average.Mips[1]);
	const std::vector<Durin::uint8> PreservedPixels = DecodeBC3Mip(Preserved.Mips[1]);
	const double SourceCoverage = 20.0 / 64.0;
	const double AverageError = std::abs(CalculateDecodedCoverage(AveragePixels, 128) - SourceCoverage);
	const double PreservedError = std::abs(CalculateDecodedCoverage(PreservedPixels, 128) - SourceCoverage);
	EXPECT_LT(PreservedError, AverageError);
	for (size_t Offset = 0; Offset < AveragePixels.size(); Offset += 4)
	{
		EXPECT_EQ(PreservedPixels[Offset], AveragePixels[Offset]);
		EXPECT_EQ(PreservedPixels[Offset + 1], AveragePixels[Offset + 1]);
		EXPECT_EQ(PreservedPixels[Offset + 2], AveragePixels[Offset + 2]);
	}
}

TEST(FTexture2DTests, CompressedLayoutsCoverNpotAndTailMips)
{
	const Durin::FPixelFormatLayout BC1Npot = Durin::GetPixelFormatLayout(Durin::EPixelFormat::BC1_UNORM, 5, 3);
	EXPECT_EQ(BC1Npot.BlocksWide, 2u);
	EXPECT_EQ(BC1Npot.BlocksHigh, 1u);
	EXPECT_EQ(BC1Npot.RowPitch, 16u);
	EXPECT_EQ(BC1Npot.DataSize, 16u);

	const Durin::FPixelFormatLayout BC3Npot = Durin::GetPixelFormatLayout(Durin::EPixelFormat::BC3_UNORM, 5, 5);
	EXPECT_EQ(BC3Npot.BlocksWide, 2u);
	EXPECT_EQ(BC3Npot.BlocksHigh, 2u);
	EXPECT_EQ(BC3Npot.RowPitch, 32u);
	EXPECT_EQ(BC3Npot.DataSize, 64u);

	const Durin::FPixelFormatLayout BC7Tail = Durin::GetPixelFormatLayout(Durin::EPixelFormat::BC7_UNORM, 1, 1);
	EXPECT_EQ(BC7Tail.BlocksWide, 1u);
	EXPECT_EQ(BC7Tail.BlocksHigh, 1u);
	EXPECT_EQ(BC7Tail.RowPitch, 16u);
	EXPECT_EQ(BC7Tail.DataSize, 16u);

	Durin::FTexture2DMipData Mip;
	Mip.Width = 5;
	Mip.Height = 3;
	Mip.RowPitch = static_cast<Durin::uint32>(BC1Npot.RowPitch);
	Mip.Pixels.resize(static_cast<size_t>(BC1Npot.DataSize));
	EXPECT_TRUE(Mip.IsValid(Durin::EPixelFormat::BC1_UNORM));
	Mip.RowPitch = 8;
	EXPECT_FALSE(Mip.IsValid(Durin::EPixelFormat::BC1_UNORM));
}

TEST(FTexture2DTests, CooperativeBuildCancellationUsesFrozenCheckpointIntervals)
{
	static_assert(Durin::Asset::Build::TextureBuilder::CancellationBlockInterval == 64);
	static_assert(Durin::Asset::Build::TextureBuilder::CancellationScanlineInterval == 8);
	Durin::FTextureSourceData Source;
	Source.Width = 512;
	Source.Height = 512;
	Source.SourceChannelCount = 4;
	Source.Format = Durin::ETextureSourceFormat::RGBA8;
	Source.Pixels.resize(
		static_cast<size_t>(Source.Width) * Source.Height
		* Durin::Asset::Build::TextureBuilder::ChannelCount,
		127);
	Durin::uint32 CheckpointCount = 0;
	const Durin::Asset::Build::TextureBuilder::FBuildExecutionControl Control{
		.ShouldCancel = [&] { return ++CheckpointCount == 20; }};
	Durin::FTexturePlatformData Platform;
	std::string Error;
	EXPECT_FALSE(Durin::Asset::Build::TextureBuilder::BuildMipChain(
		Source,
		Durin::ETextureUsage::DataMask,
		false,
		Platform,
		Error,
		0,
		Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::Average,
		0.5f,
		&Control));
	EXPECT_EQ(CheckpointCount, 20u);
	EXPECT_EQ(Error, "Texture build was cancelled.");
	EXPECT_FALSE(Platform.IsValid());
}

TEST(FTexture2DTests, PreservesLinearBuildSettingAndRebuildsColorSpace)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "LinearTextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.bSRGB = false;
	Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(Source.generic_string(), "/TextureImportTests/Linear", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Asset->IsSRGB());
	EXPECT_EQ(Result.Asset->GetUsage(), Durin::ETextureUsage::Color);
	ASSERT_NE(Result.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(Result.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Linear", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::DTexture2D* Loaded = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(AssetPath, Loaded));
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {128, 0, 0, 128});

	const std::vector<Durin::uint8> LinearTail = Loaded->GetPlatformData()->Mips.back().Pixels;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::SetTexture2DSRGB(*Loaded, true, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Loaded, 10.0))
		<< Durin::Asset::Build::GetTexture2DBuildDiagnostic(*Loaded).Message;
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	EXPECT_NE(Loaded->GetPlatformData()->Mips.back().Pixels, LinearTail);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {188, 0, 0, 128});
	ASSERT_TRUE(Durin::Asset::Forge::SetTexture2DUsage(
		*Loaded, Durin::ETextureUsage::Normal, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Loaded, 10.0))
		<< Durin::Asset::Build::GetTexture2DBuildDiagnostic(*Loaded).Message;
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {128, 37, 0, 0});
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		AssetPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, ReflectedBuildSettingsRebuildTransactionallyAndSupportUndoRedo)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "TransactionalTextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/Transactional");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTexture2D* Texture = Result.Asset;
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Texture->GetPackage(), nullptr);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());

	Durin::FProperty* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
	Durin::FProperty* SRGBProperty = Texture->GetClass()->FindPropertyByName("bSRGB");
	Durin::FProperty* MaxResolutionProperty = Texture->GetClass()->FindPropertyByName("MaxResolution");
	Durin::FProperty* CompressionQualityProperty = Texture->GetClass()->FindPropertyByName("CompressionQuality");
	Durin::FProperty* AlphaMipModeProperty = Texture->GetClass()->FindPropertyByName("AlphaMipMode");
	Durin::FProperty* AlphaCoverageThresholdProperty = Texture->GetClass()->FindPropertyByName("AlphaCoverageThreshold");
	ASSERT_NE(UsageProperty, nullptr);
	ASSERT_NE(SRGBProperty, nullptr);
	ASSERT_NE(MaxResolutionProperty, nullptr);
	ASSERT_NE(CompressionQualityProperty, nullptr);
	ASSERT_NE(AlphaMipModeProperty, nullptr);
	ASSERT_NE(AlphaCoverageThresholdProperty, nullptr);
	Durin::Editor::FPropertyView PropertyView;
	Durin::Editor::FTransactionManager Transactions;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};

	const auto SubmitUsage = [&](Durin::ETextureUsage Usage) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, UsageProperty),
			[Usage](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Usage), ArrayIndex);
			}, false);
	};
	const auto SubmitSRGB = [&](bool bSRGB) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, SRGBProperty),
			[bSRGB](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<bool>(Container, ArrayIndex) = bSRGB;
			}, false);
	};
	const auto SubmitMaxResolution = [&](Durin::uint32 MaxResolution) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, MaxResolutionProperty),
			[MaxResolution](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<Durin::uint32>(Container, ArrayIndex) = MaxResolution;
			}, false);
	};
	const auto SubmitCompressionQuality = [&](Durin::ETextureCompressionQuality Quality) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, CompressionQualityProperty),
			[Quality](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Quality), ArrayIndex);
			}, false);
	};
	const auto SubmitAlphaMipMode = [&](Durin::ETextureAlphaMipMode Mode) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, AlphaMipModeProperty),
			[Mode](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Mode), ArrayIndex);
			}, false);
	};
	const auto SubmitAlphaCoverageThreshold = [&](float Threshold) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, AlphaCoverageThresholdProperty),
			[Threshold](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = Threshold;
			}, false);
	};

	const Durin::uint64 InitialRevision = Texture->GetBuildRevision();
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());

	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());

	ASSERT_TRUE(SubmitSRGB(true)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);

	ASSERT_TRUE(SubmitMaxResolution(1)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_EQ(Texture->GetPlatformData()->Mips.size(), 1u);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Width, 1u);
	ASSERT_TRUE(SubmitCompressionQuality(Durin::ETextureCompressionQuality::High)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::Normal);
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetMaxResolution(), 0u);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Width, 2u);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	ASSERT_TRUE(SubmitAlphaMipMode(Durin::ETextureAlphaMipMode::PreserveCoverage)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	ASSERT_TRUE(SubmitAlphaCoverageThreshold(0.4f)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.4f);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.5f);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::Average);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.4f);

	Error.clear();
	EXPECT_FALSE(SubmitUsage(static_cast<Durin::ETextureUsage>(255)));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	Error.clear();
	EXPECT_FALSE(SubmitCompressionQuality(static_cast<Durin::ETextureCompressionQuality>(255)));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	Error.clear();
	EXPECT_FALSE(SubmitAlphaMipMode(static_cast<Durin::ETextureAlphaMipMode>(255)));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	Error.clear();
	EXPECT_FALSE(SubmitAlphaCoverageThreshold(1.0f));
	EXPECT_FALSE(Error.empty());
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.4f);

	Transactions.Clear();
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transactional", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		AssetPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, AsyncBuildSettingCancellationAndSupersessionPreserveTransactions)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "AsyncTransactionalTextureSource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Imported = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/AsyncTransactional");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Texture = Imported.Asset;
	ASSERT_NE(Texture, nullptr);
	Durin::FProperty* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
	ASSERT_NE(UsageProperty, nullptr);
	Durin::Editor::FPropertyView PropertyView;
	Durin::Editor::FTransactionManager Transactions;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); }};
	const auto SubmitUsage = [&](Durin::ETextureUsage Usage) {
		return PropertyView.SubmitPropertyValueEdit(
			Context,
			Durin::Editor::FPropertyEditTarget::ForMember(Texture, UsageProperty),
			[Usage](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Usage), ArrayIndex);
			},
			false);
	};
	ASSERT_TRUE(EnsureTextureBuildHost());
	Durin::Asset::Build::FTexture2DBuildCoordinator* Coordinator =
		Durin::Asset::Build::GetTexture2DBuildCoordinator();
	ASSERT_NE(Coordinator, nullptr);
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bEntered = false;
	bool bRelease = false;
	Coordinator->SetPhaseHookForTests(
		[&](Durin::uint64, Durin::Asset::Build::ETexture2DBuildPhase Phase) {
			if (Phase != Durin::Asset::Build::ETexture2DBuildPhase::Preparing) return;
			std::unique_lock Lock(Mutex);
			if (bEntered) return;
			bEntered = true;
			Condition.notify_all();
			Condition.wait(Lock, [&] { return bRelease; });
		});

	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bEntered;
		}));
	}
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	EXPECT_FALSE(Transactions.CanUndo());
	ASSERT_TRUE(Durin::Asset::Build::CancelTexture2DBuild(*Texture));
	{
		std::lock_guard Lock(Mutex);
		bRelease = true;
		Condition.notify_all();
	}
	Coordinator->SetPhaseHookForTests({});
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	EXPECT_FALSE(Transactions.CanUndo());

	bEntered = false;
	bRelease = false;
	Coordinator->SetPhaseHookForTests(
		[&](Durin::uint64, Durin::Asset::Build::ETexture2DBuildPhase Phase) {
			if (Phase != Durin::Asset::Build::ETexture2DBuildPhase::Preparing) return;
			std::unique_lock Lock(Mutex);
			if (bEntered) return;
			bEntered = true;
			Condition.notify_all();
			Condition.wait(Lock, [&] { return bRelease; });
		});
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bEntered;
		}));
	}
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::DataMask)) << Error;
	{
		std::lock_guard Lock(Mutex);
		bRelease = true;
		Condition.notify_all();
	}
	Coordinator->SetPhaseHookForTests({});
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::DataMask);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Transactions.CanUndo());
	const Durin::Editor::FTransactionId UndoId = Transactions.GetUndoId();
	bEntered = false;
	bRelease = false;
	Coordinator->SetPhaseHookForTests(
		[&](Durin::uint64, Durin::Asset::Build::ETexture2DBuildPhase Phase) {
			if (Phase != Durin::Asset::Build::ETexture2DBuildPhase::Preparing) return;
			std::unique_lock Lock(Mutex);
			if (bEntered) return;
			bEntered = true;
			Condition.notify_all();
			Condition.wait(Lock, [&] { return bRelease; });
		});
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Transactions.HasPendingOperation());
	EXPECT_FALSE(Transactions.CanUndo());
	EXPECT_FALSE(Transactions.CanRedo());
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bEntered;
		}));
	}
	ASSERT_TRUE(Durin::Asset::Build::CancelTexture2DBuild(*Texture));
	{
		std::lock_guard Lock(Mutex);
		bRelease = true;
		Condition.notify_all();
	}
	Coordinator->SetPhaseHookForTests({});
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_FALSE(Transactions.HasPendingOperation());
	EXPECT_EQ(Transactions.GetUndoId(), UndoId);
	EXPECT_TRUE(Transactions.CanUndo());
	EXPECT_FALSE(Transactions.CanRedo());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::DataMask);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture, 10.0));
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);

	Transactions.Clear();
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/AsyncTransactional", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		AssetPath,
		Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}
