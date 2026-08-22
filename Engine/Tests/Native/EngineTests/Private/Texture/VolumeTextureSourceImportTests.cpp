#include "TextureTestSupport.h"

#include "VolumeTextureSourceTranslation.h"
#include "AssetForgeProviderTestFixture.h"
#include "ImportService.h"
#include "Components/VolumetricCloudComponent.h"
#include "Modules/ModuleManager.h"
#include "Texture/TextureDerivedData.h"

using namespace Durin;
using namespace Durin::Asset::Forge;

TEST(FVolumeTextureSourceImportTests, ValidatesDirectPngAtlasSettings)
{
	std::string Error;
	FVolumeTextureImportSettings Settings{
		.Channels = EVolumeTextureSourceChannels::Red,
		.SliceWidth = 128, .SliceHeight = 128,
		.Depth = 128, .TilesX = 12, .TilesY = 12};
	EXPECT_TRUE(Settings.IsValid(&Error)) << Error;
	EXPECT_EQ(Settings.GetOutputFormat(), EVolumeTextureFormat::R8_UNORM);
	Settings.Channels = EVolumeTextureSourceChannels::RGBA;
	EXPECT_EQ(Settings.GetOutputFormat(), EVolumeTextureFormat::RGBA8_UNORM);

	for (const FVolumeTextureImportSettings Invalid : {
		FVolumeTextureImportSettings{.SliceWidth = 0},
		FVolumeTextureImportSettings{.SliceWidth = 1, .SliceHeight = 1,
			.Depth = 3, .TilesX = 1, .TilesY = 2},
		FVolumeTextureImportSettings{.SliceWidth = MaximumVolumeTextureDimension + 1}})
	{
		EXPECT_FALSE(Invalid.IsValid(&Error));
		EXPECT_FALSE(Error.empty());
	}
}

TEST(FVolumeTextureSourceImportTests, UnpacksRowMajorAtlasAndChannels)
{
	const FVolumeTextureCapturedSource Atlas{
		.SourcePath = {.Path = "/Test/Noise.png"},
		.ContentHash = FXxHash128::HashBuffer(TransparentPngBytes),
		.Bytes = TransparentPngBytes};
	FVolumeTextureImportSettings Settings{
		.Channels = EVolumeTextureSourceChannels::Red,
		.SliceWidth = 1, .SliceHeight = 1, .Depth = 2, .TilesX = 2, .TilesY = 1};
	FVolumeTextureSourceData Source;
	std::string Error;
	ASSERT_TRUE(TranslateVolumeTextureAtlasSource(Atlas, Settings, Source, Error)) << Error;
	EXPECT_EQ(Source.Width, 1u);
	EXPECT_EQ(Source.Height, 1u);
	EXPECT_EQ(Source.Depth, 2u);
	EXPECT_EQ(Source.Format, EVolumeTextureFormat::R8_UNORM);
	ASSERT_EQ(Source.Voxels.size(), 2u);
	EXPECT_EQ(Source.Voxels[0], 255u);
	EXPECT_EQ(Source.Voxels[1], 0u);

	Settings.Channels = EVolumeTextureSourceChannels::RGBA;
	ASSERT_TRUE(TranslateVolumeTextureAtlasSource(Atlas, Settings, Source, Error)) << Error;
	EXPECT_EQ(Source.Format, EVolumeTextureFormat::RGBA8_UNORM);
	ASSERT_EQ(Source.Voxels.size(), 8u);
	EXPECT_EQ(Source.Voxels[0], 255u);
	EXPECT_EQ(Source.Voxels[3], 255u);
}

TEST(FVolumeTextureSourceImportTests, RejectsCorruptAndMismatchedAtlas)
{
	FVolumeTextureImportSettings Settings{
		.SliceWidth = 1, .SliceHeight = 1, .Depth = 2, .TilesX = 1, .TilesY = 2};
	FVolumeTextureSourceData Source;
	std::string Error;
	const std::array<uint8, 4> Corrupt = {1, 2, 3, 4};
	EXPECT_FALSE(TranslateVolumeTextureAtlasSource({.Bytes = Corrupt}, Settings, Source, Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(TranslateVolumeTextureAtlasSource(
		{.Bytes = TransparentPngBytes}, Settings, Source, Error));
	EXPECT_NE(Error.find("expected 1x2"), std::string::npos);
}

TEST(FVolumeTextureSourceImportTests, ImportsReimportsRepairsAndDisplaysDirectSource)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FModuleManager::Get().LoadModuleChecked("TextureBuild");
	ASSERT_TRUE(EnsureTextureBuildHost());
	Durin::Tests::FScopedAssetForgeProviders Providers;
	std::string ProviderError;
	ASSERT_TRUE(Providers.Register(ProviderError)) << ProviderError;
	const std::filesystem::path SourceDirectory =
		Testing::GetTestWorkDirectory() / "TextureImports/Content/VolumeSource";
	std::filesystem::create_directories(SourceDirectory);
	const std::filesystem::path AtlasPath = SourceDirectory / "Noise.png";
	WriteTextureFixture(AtlasPath);
	const FVolumeTextureImportSettings Settings{
		.Channels = EVolumeTextureSourceChannels::Red,
		.SliceWidth = 1, .SliceHeight = 1, .Depth = 2, .TilesX = 2, .TilesY = 1};

	const FVolumeTextureImportResult Imported = ImportVolumeTextureAsset(
		AtlasPath.generic_string(), "/TextureImportTests/ImportedVolume", Settings);
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_NE(Imported.Asset, nullptr);
	EXPECT_EQ(Imported.Asset->GetSourceData().Depth, 2u);
	const auto& Provenance = Imported.Asset->GetSourceImportData();
	EXPECT_TRUE(Provenance.Source.SourcePath.Path.ends_with("/VolumeSource/Noise.png"));
	EXPECT_EQ(Provenance.SourceFile, Provenance.Source.SourcePath.Path);
	EXPECT_EQ(Provenance.ImportFormat, EVolumeTextureImportFormat::PngRowMajorAtlas);
	EXPECT_EQ(Provenance.SliceWidth, 1u);
	EXPECT_EQ(Provenance.TilesX, 2u);

	FProperty* ImportProperty = Imported.Asset->GetClass()->FindPropertyByName("SourceImportData");
	ASSERT_NE(ImportProperty, nullptr);
	EXPECT_TRUE(ImportProperty->HasAnyPropertyFlags(EPropertyFlags::Edit));
	EXPECT_TRUE(ImportProperty->HasAnyPropertyFlags(EPropertyFlags::ReadOnly));
	FProperty* SourceFileProperty = FVolumeTextureSourceImportData::StaticStruct()
		->FindPropertyByName("SourceFile", false);
	ASSERT_NE(SourceFileProperty, nullptr);
	EXPECT_TRUE(SourceFileProperty->HasAnyPropertyFlags(EPropertyFlags::Edit));
	EXPECT_TRUE(SourceFileProperty->HasAnyPropertyFlags(EPropertyFlags::ReadOnly));

	auto Planned = Asset::GetImportService().CreateSingleAssetReimportPlan({
		.Asset = Imported.Asset});
	ASSERT_TRUE(Planned) << Planned.Message;
	EXPECT_NE(Planned.Plan.GetSnapshot().FindSource("root"), nullptr);
	EXPECT_EQ(Planned.Plan.GetSnapshot().FindSource("slice-0000"), nullptr);
	auto Executed = Asset::GetImportService().ExecuteSingleAssetImport(Planned.Plan);
	ASSERT_TRUE(Executed) << Executed.Message;
	const std::string LastKnownGoodKey = Imported.Asset->GetDerivedDataKey();

	{
		std::ofstream Corrupt(AtlasPath, std::ios::binary | std::ios::trunc);
		Corrupt << "not a png";
	}
	Planned = Asset::GetImportService().CreateSingleAssetReimportPlan({.Asset = Imported.Asset});
	ASSERT_TRUE(Planned) << Planned.Message;
	Executed = Asset::GetImportService().ExecuteSingleAssetImport(Planned.Plan);
	EXPECT_FALSE(Executed);
	EXPECT_EQ(Imported.Asset->GetDerivedDataKey(), LastKnownGoodKey);
	EXPECT_EQ(Imported.Asset->GetBuildStatus(), ETextureBuildStatus::Ready);

	WriteTextureFixture(AtlasPath);
	const std::filesystem::path MovedDirectory =
		Testing::GetTestWorkDirectory() / "TextureImports/Content/MovedVolume";
	std::filesystem::create_directories(MovedDirectory);
	const std::filesystem::path MovedAtlas = MovedDirectory / "Noise.png";
	std::filesystem::copy_file(AtlasPath, MovedAtlas,
		std::filesystem::copy_options::overwrite_existing);
	std::string RepairError;
	ASSERT_TRUE(RepairVolumeTextureSource(*Imported.Asset,
		"/TextureImportTests/MovedVolume/Noise.png", RepairError)) << RepairError;
	EXPECT_EQ(Imported.Asset->GetSourceImportData().SourceFile,
		"/TextureImportTests/MovedVolume/Noise.png");
	ASSERT_TRUE(Asset::SavePackage(Imported.Asset->GetPackage()));

	const FVolumeTextureImportResult Detail = ImportVolumeTextureAsset(
		MovedAtlas.generic_string(), "/TextureImportTests/ImportedDetailVolume", Settings);
	ASSERT_TRUE(Detail) << Detail.Message;
	FAssetPath BaseAssetPath;
	FAssetPath DetailAssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TextureImportTests/ImportedVolume", BaseAssetPath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TextureImportTests/ImportedDetailVolume", DetailAssetPath));
	ASSERT_TRUE(Asset::UnloadPackage(BaseAssetPath));
	ASSERT_TRUE(Asset::UnloadPackage(DetailAssetPath));
	DVolumeTexture* ReloadedBase = nullptr;
	DVolumeTexture* ReloadedDetail = nullptr;
	ASSERT_TRUE(Asset::LoadAsset(BaseAssetPath, ReloadedBase));
	ASSERT_TRUE(Asset::LoadAsset(DetailAssetPath, ReloadedDetail));
	ASSERT_NE(ReloadedBase, nullptr);
	ASSERT_NE(ReloadedDetail, nullptr);
	EXPECT_EQ(ReloadedBase->GetSourceImportData().SourceFile,
		"/TextureImportTests/MovedVolume/Noise.png");
	auto* Component = NewObject<DVolumetricCloudComponent>(nullptr, "ImportedCloud");
	Component->SetBaseDensityTexture(ReloadedBase);
	Component->SetDetailDensityTexture(ReloadedDetail);
	Component->RefreshEligibilityDiagnostic();
	EXPECT_EQ(Component->GetEligibilityStatus(),
		"Ready: eligible for volumetric cloud rendering.");
	MarkAsGarbage(Component);
	CollectGarbage();
	Asset::UnloadPackage(BaseAssetPath);
	Asset::UnloadPackage(DetailAssetPath);
}
