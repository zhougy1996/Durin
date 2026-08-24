#include "TextureTestSupport.h"

#include "VolumeTextureSourceTranslation.h"
#include "AssetForgeProviderTestFixture.h"
#include "ImportService.h"
#include "Components/VolumetricCloudComponent.h"
#include "Modules/ModuleManager.h"
#include "Texture/TextureDerivedData.h"
#include "AssetCook.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/PackageInspection.h"
#include "Misc/FileHelper.h"

using namespace Durin;
using namespace Durin::Asset::Forge;

namespace
{
	auto ReimportVolumeTexture(DVolumeTexture& Texture,
		const FVolumeTextureImportSettings& Settings,
		const FVolumeTextureAuthoringOptions& AuthoringOptions = {})
		-> Asset::FInterchangeImportResult
	{
		FAssetPath Destination;
		std::string Error;
		if (!Texture.GetPackage() || !FAssetPath::TryCreate(
			Texture.GetPackage()->GetPackagePath(), Destination, &Error))
			return {.Outcome = {.State = Asset::EImportOperationState::Failed,
				.Diagnostic = std::move(Error)}};
		Asset::FInterchangeProvenance Provenance;
		if (!InspectVolumeTextureInterchangeProvenance(Texture, Provenance, Error))
			return {.Outcome = {.State = Asset::EImportOperationState::Failed,
				.Diagnostic = std::move(Error)}};
		Asset::FInterchangeImportRequest Request;
		if (!MakeVolumeTextureInterchangeRequest(
			Texture.GetSourceImportData().Source.SourcePath, Destination, Settings,
			Asset::EInterchangeImportMode::Reimport,
			{.OwnerId = "Tests.VolumeTexture.InterchangeReimport"}, Provenance,
			Request, Error, AuthoringOptions))
			return {.Outcome = {.State = Asset::EImportOperationState::Failed,
				.Diagnostic = std::move(Error)}};
		return Asset::GetImportService().RunInterchangeImportInline(
			std::move(Request), "Reimport VolumeTexture");
	}

	void AppendBigEndian32(std::vector<std::byte>& Bytes, uint32 Value)
	{
		for (int Shift : {24, 16, 8, 0})
			Bytes.push_back(static_cast<std::byte>(Value >> Shift));
	}

	uint32 PngCrc32(std::span<const std::byte> Bytes)
	{
		uint32 Crc = 0xffffffffu;
		for (std::byte Byte : Bytes)
		{
			Crc ^= std::to_integer<uint8>(Byte);
			for (int Bit = 0; Bit < 8; ++Bit)
				Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
		}
		return ~Crc;
	}

	void AppendPngChunk(std::vector<std::byte>& Bytes, std::string_view Type,
		std::span<const std::byte> Payload)
	{
		AppendBigEndian32(Bytes, static_cast<uint32>(Payload.size()));
		const size_t CrcStart = Bytes.size();
		const std::span<const std::byte> TypeBytes =
			std::as_bytes(std::span{Type.data(), Type.size()});
		Bytes.insert(Bytes.end(), TypeBytes.begin(), TypeBytes.end());
		Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
		AppendBigEndian32(Bytes, PngCrc32(std::span(Bytes).subspan(CrcStart)));
	}

	std::vector<std::byte> MakeHorizontal128CubedAtlasPng()
	{
		constexpr uint32 Width = 16384;
		constexpr uint32 Height = 128;
		std::vector<std::byte> Scanlines;
		Scanlines.reserve(static_cast<size_t>(Height) * (1 + Width * 4));
		for (uint32 Y = 0; Y < Height; ++Y)
		{
			Scanlines.push_back(std::byte{0});
			for (uint32 X = 0; X < Width; ++X)
			{
				Scanlines.push_back(static_cast<std::byte>(X / 128));
				Scanlines.insert(Scanlines.end(), {
					std::byte{0}, std::byte{0}, std::byte{255}});
			}
		}

		std::vector<std::byte> Deflate{std::byte{0x78}, std::byte{0x01}};
		size_t Offset = 0;
		while (Offset < Scanlines.size())
		{
			const uint16 BlockSize = static_cast<uint16>(
				std::min<size_t>(65535, Scanlines.size() - Offset));
			Deflate.push_back(Offset + BlockSize == Scanlines.size()
				? std::byte{1} : std::byte{0});
			Deflate.push_back(static_cast<std::byte>(BlockSize));
			Deflate.push_back(static_cast<std::byte>(BlockSize >> 8));
			const uint16 Complement = static_cast<uint16>(~BlockSize);
			Deflate.push_back(static_cast<std::byte>(Complement));
			Deflate.push_back(static_cast<std::byte>(Complement >> 8));
			Deflate.insert(Deflate.end(), Scanlines.begin() + Offset,
				Scanlines.begin() + Offset + BlockSize);
			Offset += BlockSize;
		}
		uint32 AdlerA = 1;
		uint32 AdlerB = 0;
		for (std::byte Byte : Scanlines)
		{
			AdlerA = (AdlerA + std::to_integer<uint8>(Byte)) % 65521;
			AdlerB = (AdlerB + AdlerA) % 65521;
		}
		AppendBigEndian32(Deflate, (AdlerB << 16) | AdlerA);

		std::vector<std::byte> Png{
			std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
			std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
		std::array<uint8, 13> Header{};
		Header[0] = static_cast<uint8>(Width >> 24);
		Header[1] = static_cast<uint8>(Width >> 16);
		Header[2] = static_cast<uint8>(Width >> 8);
		Header[3] = static_cast<uint8>(Width);
		Header[4] = static_cast<uint8>(Height >> 24);
		Header[5] = static_cast<uint8>(Height >> 16);
		Header[6] = static_cast<uint8>(Height >> 8);
		Header[7] = static_cast<uint8>(Height);
		Header[8] = 8;
		Header[9] = 6;
		AppendPngChunk(Png, "IHDR", std::as_bytes(std::span{Header}));
		AppendPngChunk(Png, "IDAT", Deflate);
		AppendPngChunk(Png, "IEND", {});
		return Png;
	}

	std::vector<std::byte> MakeGrayscaleRgbaPng(uint32 Width, uint32 Height)
	{
		std::vector<std::byte> Scanlines;
		Scanlines.reserve(static_cast<size_t>(Height) * (1 + Width * 4));
		for (uint32 Y = 0; Y < Height; ++Y)
		{
			Scanlines.push_back(std::byte{0});
			for (uint32 X = 0; X < Width; ++X)
			{
				const uint8 Value = static_cast<uint8>((X + Y) & 0xff);
				const std::array Pixel{Value, Value, Value, uint8{255}};
				const auto PixelBytes = std::as_bytes(std::span{Pixel});
				Scanlines.insert(Scanlines.end(), PixelBytes.begin(), PixelBytes.end());
			}
		}

		std::vector<std::byte> Deflate{std::byte{0x78}, std::byte{0x01}};
		size_t Offset = 0;
		while (Offset < Scanlines.size())
		{
			const uint16 BlockSize = static_cast<uint16>(
				std::min<size_t>(65535, Scanlines.size() - Offset));
			Deflate.push_back(Offset + BlockSize == Scanlines.size()
				? std::byte{1} : std::byte{0});
			Deflate.push_back(static_cast<std::byte>(BlockSize));
			Deflate.push_back(static_cast<std::byte>(BlockSize >> 8));
			const uint16 Complement = static_cast<uint16>(~BlockSize);
			Deflate.push_back(static_cast<std::byte>(Complement));
			Deflate.push_back(static_cast<std::byte>(Complement >> 8));
			Deflate.insert(Deflate.end(), Scanlines.begin() + Offset,
				Scanlines.begin() + Offset + BlockSize);
			Offset += BlockSize;
		}
		uint32 AdlerA = 1;
		uint32 AdlerB = 0;
		for (std::byte Byte : Scanlines)
		{
			AdlerA = (AdlerA + std::to_integer<uint8>(Byte)) % 65521;
			AdlerB = (AdlerB + AdlerA) % 65521;
		}
		AppendBigEndian32(Deflate, (AdlerB << 16) | AdlerA);

		std::vector<std::byte> Png{
			std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
			std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
		std::array<uint8, 13> Header{};
		Header[0] = static_cast<uint8>(Width >> 24);
		Header[1] = static_cast<uint8>(Width >> 16);
		Header[2] = static_cast<uint8>(Width >> 8);
		Header[3] = static_cast<uint8>(Width);
		Header[4] = static_cast<uint8>(Height >> 24);
		Header[5] = static_cast<uint8>(Height >> 16);
		Header[6] = static_cast<uint8>(Height >> 8);
		Header[7] = static_cast<uint8>(Height);
		Header[8] = 8;
		Header[9] = 6;
		AppendPngChunk(Png, "IHDR", std::as_bytes(std::span{Header}));
		AppendPngChunk(Png, "IDAT", Deflate);
		AppendPngChunk(Png, "IEND", {});
		return Png;
	}
}

TEST(FVolumeTextureSourceImportTests, InfersCubicLayoutAndScalarChannelFromPngContent)
{
	const std::filesystem::path Directory =
		Testing::GetTestWorkDirectory() / "VolumeTextureInspection";
	std::filesystem::create_directories(Directory);
	const std::filesystem::path AtlasPath = Directory / "Atlas.png";
	const std::vector<std::byte> Png = MakeGrayscaleRgbaPng(512, 512);
	{
		std::ofstream Stream(AtlasPath, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(reinterpret_cast<const char*>(Png.data()),
			static_cast<std::streamsize>(Png.size()));
	}

	const FVolumeTextureAtlasInspection Inspection =
		InspectVolumeTextureAtlasSource(AtlasPath.generic_string());
	ASSERT_TRUE(Inspection) << Inspection.Message;
	EXPECT_EQ(Inspection.AtlasWidth, 512u);
	EXPECT_EQ(Inspection.AtlasHeight, 512u);
	EXPECT_EQ(Inspection.SuggestedChannels, EVolumeTextureSourceChannels::Red);
	ASSERT_TRUE(Inspection.bHasConfidentLayout) << Inspection.Message;
	ASSERT_FALSE(Inspection.SuggestedLayouts.empty());
	const FVolumeTextureImportSettings& Suggested =
		Inspection.SuggestedLayouts.front();
	EXPECT_EQ(Suggested.SliceWidth, 64u);
	EXPECT_EQ(Suggested.SliceHeight, 64u);
	EXPECT_EQ(Suggested.Depth, 64u);
	EXPECT_EQ(Suggested.TilesX, 8u);
	EXPECT_EQ(Suggested.TilesY, 8u);
}

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
	const std::span<const std::byte> TransparentPngData =
		std::as_bytes(std::span{TransparentPngBytes});
	const FVolumeTextureCapturedSource Atlas{
		.SourcePath = {.Path = "/Test/Noise.png"},
		.ContentHash = FXxHash128::HashBuffer(TransparentPngData),
		.Bytes = TransparentPngData};
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
	ASSERT_EQ(Source.GetVoxelBytes().size(), 2u);
	EXPECT_EQ(Source.GetVoxelBytes()[0], std::byte{255});
	EXPECT_EQ(Source.GetVoxelBytes()[1], std::byte{0});

	Settings.Channels = EVolumeTextureSourceChannels::RGBA;
	ASSERT_TRUE(TranslateVolumeTextureAtlasSource(Atlas, Settings, Source, Error)) << Error;
	EXPECT_EQ(Source.Format, EVolumeTextureFormat::RGBA8_UNORM);
	ASSERT_EQ(Source.GetVoxelBytes().size(), 8u);
	EXPECT_EQ(Source.GetVoxelBytes()[0], std::byte{255});
	EXPECT_EQ(Source.GetVoxelBytes()[3], std::byte{255});
}

TEST(FVolumeTextureSourceImportTests, RejectsCorruptAndMismatchedAtlas)
{
	FVolumeTextureImportSettings Settings{
		.SliceWidth = 1, .SliceHeight = 1, .Depth = 2, .TilesX = 1, .TilesY = 2};
	FVolumeTextureSourceData Source;
	std::string Error;
	const std::array<uint8, 4> Corrupt = {1, 2, 3, 4};
	EXPECT_FALSE(TranslateVolumeTextureAtlasSource(
		{.Bytes = std::as_bytes(std::span{Corrupt})}, Settings, Source, Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(TranslateVolumeTextureAtlasSource(
		{.Bytes = std::as_bytes(std::span{TransparentPngBytes})},
		Settings, Source, Error));
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
	EXPECT_TRUE(ImportProperty->HasAnyPropertyFlags(EPropertyFlags::EditorOnly));
	FProperty* SourceFileProperty = FVolumeTextureSourceImportData::StaticStruct()
		->FindPropertyByName("SourceFile", false);
	ASSERT_NE(SourceFileProperty, nullptr);
	EXPECT_TRUE(SourceFileProperty->HasAnyPropertyFlags(EPropertyFlags::Edit));
	EXPECT_TRUE(SourceFileProperty->HasAnyPropertyFlags(EPropertyFlags::ReadOnly));

	auto Executed = ReimportVolumeTexture(*Imported.Asset, Settings);
	ASSERT_EQ(Executed.Outcome.State, Asset::EImportOperationState::Succeeded)
		<< Executed.Outcome.Diagnostic;
	const std::string LastKnownGoodKey = Imported.Asset->GetDerivedDataKey();

	{
		std::ofstream Corrupt(AtlasPath, std::ios::binary | std::ios::trunc);
		Corrupt << "not a png";
	}
	Executed = ReimportVolumeTexture(*Imported.Asset, Settings);
	EXPECT_EQ(Executed.Outcome.State, Asset::EImportOperationState::Failed);
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
		MovedAtlas.generic_string(), "/TextureImportTests/ImportedDetailVolume", Settings,
		false, {.WriterSelection = Asset::EAssetPackageWriterSelection::DastV5});
	ASSERT_TRUE(Detail) << Detail.Message;
	FAssetPath BaseAssetPath;
	FAssetPath DetailAssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TextureImportTests/ImportedVolume", BaseAssetPath));
	ASSERT_TRUE(FAssetPath::TryCreate("/TextureImportTests/ImportedDetailVolume", DetailAssetPath));
	ASSERT_EQ(Asset::FindAssetExact(BaseAssetPath)->FormatVersion,
		5u);
	const Asset::FAssetCatalogEntry InlineV5 = Asset::FindAssetExact(DetailAssetPath);
	ASSERT_TRUE(InlineV5);
	ASSERT_EQ(InlineV5->FormatVersion, 5u);
	Asset::FAssetPackageInspection InlineInspection;
	ASSERT_TRUE(Asset::InspectAssetPackage(InlineV5->PhysicalPath, InlineInspection));
	std::vector<Asset::FEditorBulkDataStorageDescriptor> InlineDescriptors;
	ASSERT_TRUE(Asset::InspectEditorBulkDataStorageDescriptors(
		InlineInspection, InlineDescriptors, &RepairError)) << RepairError;
	ASSERT_EQ(InlineDescriptors.size(), 1u);
	EXPECT_EQ(InlineDescriptors.front().StorageKind,
		Asset::EEditorBulkDataStorageKind::Inline);
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

TEST(FVolumeTextureSourceImportTests, ImportsSavesReloadsReimportsAndCooksHorizontal128CubedAtlas)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FModuleManager::Get().LoadModuleChecked("TextureBuild");
	ASSERT_TRUE(EnsureTextureBuildHost());
	Durin::Tests::FScopedAssetForgeProviders Providers;
	std::string Error;
	ASSERT_TRUE(Providers.Register(Error)) << Error;
	FScopedDerivedDataCacheRoot CacheRoot(
		Testing::GetTestWorkDirectory() / "VolumeTextureProductionAtlasDdc");
	const std::filesystem::path SourceDirectory =
		Testing::GetTestWorkDirectory() / "TextureImports/Content/ProductionVolume";
	std::filesystem::create_directories(SourceDirectory);
	const std::filesystem::path AtlasPath = SourceDirectory / "Noise128.png";
	const std::vector<std::byte> Png = MakeHorizontal128CubedAtlasPng();
	{
		std::ofstream Stream(AtlasPath, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream.write(reinterpret_cast<const char*>(Png.data()),
			static_cast<std::streamsize>(Png.size()));
	}
	const FVolumeTextureImportSettings Settings{
		.Channels = EVolumeTextureSourceChannels::Red,
		.SliceWidth = 128, .SliceHeight = 128, .Depth = 128,
		.TilesX = 128, .TilesY = 1};
	const FVolumeTextureImportResult Imported = ImportVolumeTextureAsset(
		AtlasPath.generic_string(), "/TextureImportTests/ProductionVolume", Settings,
		false, {.WriterSelection = Asset::EAssetPackageWriterSelection::DastV5});
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_NE(Imported.Asset, nullptr);
	ASSERT_EQ(Imported.Asset->GetSourceData().GetVoxelBytes().size(), 128ull * 128 * 128);
	for (uint32 Slice : {0u, 1u, 63u, 127u})
		EXPECT_EQ(Imported.Asset->GetSourceData().GetVoxelBytes()[Slice * 128ull * 128],
			static_cast<std::byte>(Slice));

	const std::string V5DerivedDataKey = Imported.Asset->GetDerivedDataKey();
	const std::vector<std::byte> V5SourceBytes(
		Imported.Asset->GetSourceData().GetVoxelBytes().begin(),
		Imported.Asset->GetSourceData().GetVoxelBytes().end());
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/TextureImportTests/ProductionVolume", AssetPath));
	const Asset::FAssetCatalogEntry V5Entry = Asset::FindAssetExact(AssetPath);
	ASSERT_TRUE(V5Entry);
	ASSERT_EQ(V5Entry->FormatVersion, 5u);
	Asset::FAssetPackageInspection V5Inspection;
	ASSERT_TRUE(Asset::InspectAssetPackage(V5Entry->PhysicalPath, V5Inspection));
	std::vector<Asset::FEditorBulkDataStorageDescriptor> V5Descriptors;
	ASSERT_TRUE(Asset::InspectEditorBulkDataStorageDescriptors(
		V5Inspection, V5Descriptors, &Error)) << Error;
	ASSERT_EQ(V5Descriptors.size(), 1u);
	EXPECT_EQ(V5Descriptors.front().StorageKind,
		Asset::EEditorBulkDataStorageKind::External);
	std::vector<std::filesystem::path> V5Companions;
	ASSERT_TRUE(Asset::InspectEditorBulkDataCompanionPaths(
		V5Entry->PhysicalPath, V5Inspection, V5Companions, &Error)) << Error;
	ASSERT_EQ(V5Companions.size(), 1u);

	const auto Reimported = ReimportVolumeTexture(*Imported.Asset, Settings,
		{.WriterSelection = Asset::EAssetPackageWriterSelection::DastV5});
	ASSERT_EQ(Reimported.Outcome.State, Asset::EImportOperationState::Succeeded)
		<< Reimported.Outcome.Diagnostic;
	EXPECT_EQ(Imported.Asset->GetDerivedDataKey(), V5DerivedDataKey);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Testing::GetTestWorkDirectory() / "VolumeTextureProductionAtlasCook");
	Testing::RemoveTestWorkDirectory(CookRoot);
	Asset::FCookContext Cook(CookRoot, Asset::ECookTargetPlatform::Win64,
		Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Imported.Asset->AddToCook(Cook, "/Game/ProductionVolume", Error)) << Error;
	ASSERT_TRUE(Cook.Publish(&Error)) << Error;
	EXPECT_TRUE(std::filesystem::exists(CookRoot / "Game/ProductionVolume.dasset"));
	EXPECT_TRUE(std::filesystem::exists(CookRoot / "Game/ProductionVolume.dbulk"));
	std::vector<std::byte> V5CookedPackage;
	std::vector<std::byte> V5CookedBulk;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(
		V5CookedPackage, CookRoot / "Game/ProductionVolume.dasset"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(
		V5CookedBulk, CookRoot / "Game/ProductionVolume.dbulk"));

	ASSERT_TRUE(Asset::UnloadPackage(AssetPath));
	std::vector<std::byte> CompanionBytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(CompanionBytes, V5Companions.front()));
	ASSERT_FALSE(CompanionBytes.empty());
	CompanionBytes.back() ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(CompanionBytes, V5Companions.front()));
	DVolumeTexture* CorruptLoad = nullptr;
	EXPECT_FALSE(Asset::LoadAsset(AssetPath, CorruptLoad));
	CompanionBytes.back() ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(CompanionBytes, V5Companions.front()));
	DVolumeTexture* Reloaded = nullptr;
	const Asset::FAssetResult Loaded = Asset::LoadAsset(AssetPath, Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetSourceData().GetVoxelBytes().size(), 128ull * 128 * 128);
	EXPECT_TRUE(std::ranges::equal(
		Reloaded->GetSourceData().GetVoxelBytes(), V5SourceBytes));
	EXPECT_EQ(Reloaded->GetDerivedDataKey(), V5DerivedDataKey);
	ASSERT_TRUE(Asset::SavePackage(Reloaded->GetPackage(),
		{.WriterSelection = Asset::EAssetPackageWriterSelection::DastV4}));
	ASSERT_EQ(Asset::FindAssetExact(AssetPath)->FormatVersion,
		4u);
	EXPECT_TRUE(std::filesystem::is_regular_file(V5Companions.front()));

	const std::filesystem::path RollbackCookRoot = std::filesystem::absolute(
		Testing::GetTestWorkDirectory() / "VolumeTextureProductionAtlasRollbackCook");
	Testing::RemoveTestWorkDirectory(RollbackCookRoot);
	Asset::FCookContext RollbackCook(RollbackCookRoot,
		Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Reloaded->AddToCook(
		RollbackCook, "/Game/ProductionVolume", Error)) << Error;
	ASSERT_TRUE(RollbackCook.Publish(&Error)) << Error;
	std::vector<std::byte> V4CookedPackage;
	std::vector<std::byte> V4CookedBulk;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(
		V4CookedPackage, RollbackCookRoot / "Game/ProductionVolume.dasset"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(
		V4CookedBulk, RollbackCookRoot / "Game/ProductionVolume.dbulk"));
	EXPECT_EQ(V4CookedPackage, V5CookedPackage);
	EXPECT_EQ(V4CookedBulk, V5CookedBulk);
	ASSERT_TRUE(Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(AssetPath));
	EXPECT_FALSE(std::filesystem::exists(V5Companions.front()));
}
