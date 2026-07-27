#include "TextureTestSupport.h"

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
	Settings.AlphaMipMode = Durin::ETextureAlphaMipMode::PreserveCoverage;
	Settings.AlphaCoverageThreshold = 0.4f;
	Durin::FTexture2DImportResult Result = Durin::DTexture2D::ImportAsset(
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
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
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
	ASSERT_TRUE(Durin::TextureBuild::BuildMipChain(Source, Durin::ETextureUsage::Color, false,
		Average, Error, 0, Durin::ETextureCompressionQuality::High,
		Durin::ETextureAlphaMipMode::Average, 0.5f)) << Error;
	ASSERT_TRUE(Durin::TextureBuild::BuildMipChain(Source, Durin::ETextureUsage::Color, false,
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
	Durin::FProperty* AlphaMipModeProperty = Texture->GetClass()->FindPropertyByName("AlphaMipMode");
	Durin::FProperty* AlphaCoverageThresholdProperty = Texture->GetClass()->FindPropertyByName("AlphaCoverageThreshold");
	ASSERT_NE(UsageProperty, nullptr);
	ASSERT_NE(SRGBProperty, nullptr);
	ASSERT_NE(MaxResolutionProperty, nullptr);
	ASSERT_NE(CompressionQualityProperty, nullptr);
	ASSERT_NE(AlphaMipModeProperty, nullptr);
	ASSERT_NE(AlphaCoverageThresholdProperty, nullptr);
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
	const auto SubmitAlphaMipMode = [&](Durin::ETextureAlphaMipMode Mode) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, AlphaMipModeProperty),
			[Mode](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				static_cast<Durin::FEnumProperty*>(Property)->SetValueFromUInt64(
					Container, static_cast<Durin::uint64>(Mode), ArrayIndex);
			}, false);
	};
	const auto SubmitAlphaCoverageThreshold = [&](float Threshold) {
		return PropertyView.SubmitPropertyValueEdit(Context,
			Durin::FReflectedPropertyEditTarget::ForMember(Texture, AlphaCoverageThresholdProperty),
			[Threshold](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*Property->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = Threshold;
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
	ASSERT_TRUE(SubmitAlphaMipMode(Durin::ETextureAlphaMipMode::PreserveCoverage)) << Error;
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	ASSERT_TRUE(SubmitAlphaCoverageThreshold(0.4f)) << Error;
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.4f);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FLOAT_EQ(Texture->GetAlphaCoverageThreshold(), 0.5f);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::Average);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Texture->GetAlphaMipMode(), Durin::ETextureAlphaMipMode::PreserveCoverage);
	ASSERT_TRUE(Transactions.Redo());
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
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}
