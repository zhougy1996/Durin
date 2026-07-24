#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorTransaction.h"
#include "Editor/ReflectedPropertyView.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "Texture/Texture2D.h"

#include <bc7decomp.h>
#include <gtest/gtest.h>
#include <rgbcx.h>

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

	auto WriteNpotTextureFixture(const std::filesystem::path& Path) -> void
	{
		constexpr Durin::uint16 Width = 5;
		constexpr Durin::uint16 Height = 3;
		std::array<Durin::uint8, 18> Header{};
		Header[2] = 2;
		Header[12] = static_cast<Durin::uint8>(Width);
		Header[14] = static_cast<Durin::uint8>(Height);
		Header[16] = 32;
		Header[17] = 0x28;
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Header.data()), Header.size());
		for (Durin::uint16 Y = 0; Y < Height; ++Y)
		{
			for (Durin::uint16 X = 0; X < Width; ++X)
			{
				const Durin::uint8 Value = X == Width - 1 ? 255 : 0;
				const std::array<Durin::uint8, 4> Pixel = {Value, Value, Value, 255};
				Stream.write(reinterpret_cast<const char*>(Pixel.data()), Pixel.size());
			}
		}
	}

	auto DecodeFirstCompressedPixel(Durin::EPixelFormat Format, const std::vector<Durin::uint8>& Block)
		-> std::array<Durin::uint8, 4>
	{
		std::array<Durin::uint8, 64> Pixels{};
		switch (Format)
		{
		case Durin::EPixelFormat::BC1_UNORM:
		case Durin::EPixelFormat::BC1_UNORM_SRGB:
			EXPECT_FALSE(rgbcx::unpack_bc1(Block.data(), Pixels.data()));
			break;
		case Durin::EPixelFormat::BC3_UNORM:
		case Durin::EPixelFormat::BC3_UNORM_SRGB:
			EXPECT_TRUE(rgbcx::unpack_bc3(Block.data(), Pixels.data()));
			break;
		case Durin::EPixelFormat::BC5_UNORM:
			rgbcx::unpack_bc5(Block.data(), Pixels.data());
			break;
		case Durin::EPixelFormat::BC7_UNORM:
		case Durin::EPixelFormat::BC7_UNORM_SRGB:
			static_assert(sizeof(bc7decomp::color_rgba) == 4);
			EXPECT_TRUE(bc7decomp::unpack_bc7(Block.data(), reinterpret_cast<bc7decomp::color_rgba*>(Pixels.data())));
			break;
		default:
			ADD_FAILURE() << "Unsupported compressed test format";
			break;
		}
		return {Pixels[0], Pixels[1], Pixels[2], Pixels[3]};
	}

	auto ExpectPixelNear(const std::array<Durin::uint8, 4>& Actual,
		const std::array<Durin::uint8, 4>& Expected, int Tolerance = 24) -> void
	{
		for (size_t Channel = 0; Channel < Expected.size(); ++Channel)
			EXPECT_NEAR(Actual[Channel], Expected[Channel], Tolerance) << "channel " << Channel;
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
	ASSERT_NE(Loaded->GetSourceData(), nullptr);
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_TRUE(Loaded->GetSourceData()->IsValid());
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

TEST(FTexture2DTests, UsagePresetsChooseColorSpaceAndMipFilter)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "UsagePresetSource.png";
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
		Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), AssetPathString, Settings);
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
		ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
	}
}

TEST(FTexture2DTests, BuildsCompleteNpotMipChainWithoutDroppingEdges)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "NpotTextureSource.tga";
	WriteNpotTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.Usage = Durin::ETextureUsage::DataMask;
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureImportTests/Npot", Settings);
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
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));

	Durin::FTexture2DImportResult ColorResult = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureImportTests/NpotColor");
	ASSERT_TRUE(ColorResult) << ColorResult.Message;
	ASSERT_NE(ColorResult.Asset, nullptr);
	ASSERT_NE(ColorResult.Asset->GetPlatformData(), nullptr);
	EXPECT_EQ(ColorResult.Asset->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC1_UNORM_SRGB);
	EXPECT_TRUE(ColorResult.Asset->GetPlatformData()->IsValid());
	Durin::FAssetPath ColorAssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/NpotColor", ColorAssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ColorAssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(ColorAssetPath));
}

TEST(FTexture2DTests, MaximumResolutionSelectsMipAlignedBaseLevel)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "LimitedTextureSource.tga";
	WriteNpotTextureFixture(Source);
	Durin::FTexture2DImportSettings Settings;
	Settings.MaxResolution = 4;
	Settings.CompressionQuality = Durin::ETextureCompressionQuality::Low;
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureImportTests/Limited", Settings);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Result.Asset, nullptr);
	EXPECT_EQ(Result.Asset->GetMaxResolution(), 4u);
	EXPECT_EQ(Result.Asset->GetCompressionQuality(), Durin::ETextureCompressionQuality::Low);
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
	ASSERT_NE(Loaded->GetPlatformData(), nullptr);
	EXPECT_EQ(Loaded->GetPlatformData()->Mips.front().Width, 2u);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
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
	ASSERT_TRUE(Loaded->SetSRGB(true, Error)) << Error;
	EXPECT_TRUE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	EXPECT_NE(Loaded->GetPlatformData()->Mips.back().Pixels, LinearTail);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {188, 0, 0, 128});
	ASSERT_TRUE(Loaded->SetUsage(Durin::ETextureUsage::Normal, Error)) << Error;
	EXPECT_EQ(Loaded->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Loaded->IsSRGB());
	EXPECT_EQ(Loaded->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	ExpectPixelNear(DecodeFirstCompressedPixel(Loaded->GetPlatformData()->PixelFormat,
		Loaded->GetPlatformData()->Mips.back().Pixels), {128, 37, 0, 0});
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, ReflectedBuildSettingsRebuildTransactionallyAndSupportUndoRedo)
{
	InitializeDObjectSystem();
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TransactionalTextureSource.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
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
	ASSERT_NE(UsageProperty, nullptr);
	ASSERT_NE(SRGBProperty, nullptr);
	ASSERT_NE(MaxResolutionProperty, nullptr);
	ASSERT_NE(CompressionQualityProperty, nullptr);
	Durin::FReflectedPropertyView PropertyView;
	Durin::FEditorTransactionManager Transactions;
	std::string Error;
	const Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};

	const auto SubmitUsage = [&](Durin::ETextureUsage Usage) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, UsageProperty),
			[Usage](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Usage), ArrayIndex);
			}, false);
	};
	const auto SubmitSRGB = [&](bool bSRGB) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, SRGBProperty),
			[bSRGB](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<bool>(Container, ArrayIndex) = bSRGB;
			}, false);
	};
	const auto SubmitMaxResolution = [&](Durin::uint32 MaxResolution) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, MaxResolutionProperty),
			[MaxResolution](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<Durin::uint32>(Container, ArrayIndex) = MaxResolution;
			}, false);
	};
	const auto SubmitCompressionQuality = [&](Durin::ETextureCompressionQuality Quality) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, CompressionQualityProperty),
			[Quality](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Quality), ArrayIndex);
			}, false);
	};

	const Durin::uint64 InitialRevision = Texture->GetBuildRevision();
	ASSERT_TRUE(SubmitUsage(Durin::ETextureUsage::Normal)) << Error;
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	EXPECT_GT(Texture->GetBuildRevision(), InitialRevision);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Color);
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC3_UNORM_SRGB);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Texture->GetUsage(), Durin::ETextureUsage::Normal);
	EXPECT_FALSE(Texture->IsSRGB());

	ASSERT_TRUE(SubmitSRGB(true)) << Error;
	EXPECT_TRUE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_EQ(Texture->GetPlatformData()->PixelFormat, Durin::EPixelFormat::BC5_UNORM);

	ASSERT_TRUE(SubmitMaxResolution(1)) << Error;
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_EQ(Texture->GetPlatformData()->Mips.size(), 1u);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Width, 1u);
	ASSERT_TRUE(SubmitCompressionQuality(Durin::ETextureCompressionQuality::High)) << Error;
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::Normal);
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Texture->GetMaxResolution(), 0u);
	EXPECT_EQ(Texture->GetPlatformData()->Mips.front().Width, 2u);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Texture->GetMaxResolution(), 1u);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Texture->GetCompressionQuality(), Durin::ETextureCompressionQuality::High);

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

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Transactional", AssetPath));
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

TEST(FTexture2DTests, FailureState_RecordsMissingSourceOnPostLoad)
{
	InitializeDObjectSystem();
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/FailureTestMissing", AssetPath));
	Durin::DTexture2D* Texture = nullptr;
	Durin::Asset::FAssetResult CreateResult = Durin::Asset::CreateAsset(AssetPath, Texture);
	ASSERT_TRUE(CreateResult) << CreateResult.Message;
	ASSERT_NE(Texture, nullptr);
	// At creation time, the build has not run.
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Unbuilt);
	// PostLoad with an empty source file.
	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	EXPECT_FALSE(Texture->GetLastBuildError().empty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
}

TEST(FTexture2DTests, FailureState_ReadyAfterSuccessfulPostLoad)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureFailureMount";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/TextureFailureTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "FailureReadySource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureFailureTests/Ready");
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.Asset->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_TRUE(Result.Asset->GetLastBuildError().empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureFailureTests/Ready", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, MissingSourceInvalidatesDerivedDataAndCanRecover)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureInvalidateMount";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/TextureInvalidateTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;

	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_WORK_DIR) / "InvalidateSource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(Source.generic_string(), "/TextureInvalidateTests/Invalid");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTexture2D* Texture = Result.Asset;
	ASSERT_NE(Texture, nullptr);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	ASSERT_NE(Texture->GetSourceData(), nullptr);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureInvalidateTests/Invalid", AssetPath));
	const std::filesystem::path CopiedSource =
		std::filesystem::path(DURIN_TEST_WORK_DIR) / "TextureInvalidateMount" / "Invalid.png";
	ASSERT_TRUE(std::filesystem::remove(CopiedSource));

	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_EQ(Texture->GetPlatformData(), nullptr);
	EXPECT_FALSE(Texture->GetLastBuildError().empty());

	WriteTextureFixture(CopiedSource);
	ASSERT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_NE(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(Texture->GetLastBuildError().empty());

	ASSERT_TRUE(Durin::Asset::SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}

TEST(FTexture2DTests, StatusEnumsExposeSharedDisplayMetadata)
{
	InitializeDObjectSystem();
	Durin::DEnum* BuildStatusEnum = Durin::FindEnumByQualifiedName("Durin::ETextureBuildStatus");
	Durin::DEnum* ResourceStateEnum = Durin::FindEnumByQualifiedName("Durin::ERenderResourceState");
	ASSERT_NE(BuildStatusEnum, nullptr);
	ASSERT_NE(ResourceStateEnum, nullptr);
	EXPECT_EQ(BuildStatusEnum->GetDisplayName(), "Texture Build Status");
	EXPECT_EQ(ResourceStateEnum->GetDisplayName(), "Render Resource State");

	const Durin::FEnumValue* Unbuilt = BuildStatusEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ETextureBuildStatus::Unbuilt));
	const Durin::FEnumValue* MissingSource = BuildStatusEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ETextureBuildStatus::MissingSource));
	const Durin::FEnumValue* Building = ResourceStateEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ERenderResourceState::Building));
	ASSERT_NE(Unbuilt, nullptr);
	ASSERT_NE(MissingSource, nullptr);
	ASSERT_NE(Building, nullptr);
	EXPECT_EQ(Unbuilt->DisplayName, "Not Built");
	EXPECT_EQ(MissingSource->DisplayName, "Missing Source");
	EXPECT_EQ(Building->DisplayName, "Building");
	EXPECT_EQ(BuildStatusEnum->FindValueRecordByValue(255), nullptr);
}
