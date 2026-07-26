#include "TextureTestSupport.h"

#include "Texture/TextureDerivedData.h"

namespace
{
	auto MakePlatformData(
		Durin::EPixelFormat PixelFormat = Durin::EPixelFormat::BC3_UNORM_SRGB)
		-> Durin::FTexturePlatformData
	{
		Durin::FTexturePlatformData Result;
		Result.PixelFormat = PixelFormat;
		for (Durin::uint32 Dimension : {5u, 2u, 1u})
		{
			const Durin::FPixelFormatLayout Layout =
				Durin::GetPixelFormatLayout(Result.PixelFormat, Dimension, Dimension);
			Durin::FTexture2DMipData& Mip = Result.Mips.emplace_back();
			Mip.Width = Dimension;
			Mip.Height = Dimension;
			Mip.RowPitch = static_cast<Durin::uint32>(Layout.RowPitch);
			Mip.Pixels.resize(static_cast<size_t>(Layout.DataSize), static_cast<Durin::uint8>(Dimension));
		}
		return Result;
	}

	auto WriteU32(std::vector<Durin::uint8>& Bytes, size_t Offset, Durin::uint32 Value) -> void
	{
		for (Durin::uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<Durin::uint8>(Value >> (Byte * 8));
	}
}

TEST(FTextureDerivedDataTests, CanonicalKeyCoversEverySemanticInput)
{
	Durin::FTexture2DDerivedDataKeyInput Input{
		.SourceContentHash = {0x0123456789abcdefull, 0xfedcba9876543210ull},
		.Usage = Durin::ETextureUsage::Color,
		.bSRGB = true,
		.CompressionQuality = Durin::ETextureCompressionQuality::Normal,
		.AlphaMipMode = Durin::ETextureAlphaMipMode::Average,
		.MaximumResolution = 2048,
		.AlphaCoverageThreshold = 0.5f,
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game};
	const std::string Baseline = Durin::BuildTexture2DDerivedDataKey(Input);
	EXPECT_EQ(Baseline.size(), 32u);

	auto ExpectChange = [&Baseline](const Durin::FTexture2DDerivedDataKeyInput& Changed) {
		EXPECT_NE(Durin::BuildTexture2DDerivedDataKey(Changed), Baseline);
	};
	auto Changed = Input;
	Changed.SourceContentHash.HashLow ^= 1;
	ExpectChange(Changed);
	Changed = Input;
	Changed.Usage = Durin::ETextureUsage::Normal;
	ExpectChange(Changed);
	Changed = Input;
	Changed.bSRGB = false;
	ExpectChange(Changed);
	Changed = Input;
	Changed.CompressionQuality = Durin::ETextureCompressionQuality::High;
	ExpectChange(Changed);
	Changed = Input;
	Changed.AlphaMipMode = Durin::ETextureAlphaMipMode::PreserveCoverage;
	ExpectChange(Changed);
	Changed = Input;
	Changed.MaximumResolution = 1024;
	ExpectChange(Changed);
	Changed = Input;
	Changed.AlphaCoverageThreshold = 0.25f;
	ExpectChange(Changed);
	Changed = Input;
	++Changed.BuilderVersion;
	ExpectChange(Changed);
	Changed = Input;
	++Changed.PayloadSchemaVersion;
	ExpectChange(Changed);
	Changed = Input;
	Changed.TargetProfile = Durin::Asset::ECookTargetProfile::EditorValidation;
	ExpectChange(Changed);
}

TEST(FTextureDerivedDataTests, PayloadRoundTripsDeterministically)
{
	constexpr std::array Formats = {
		Durin::EPixelFormat::BC1_UNORM,
		Durin::EPixelFormat::BC1_UNORM_SRGB,
		Durin::EPixelFormat::BC3_UNORM,
		Durin::EPixelFormat::BC3_UNORM_SRGB,
		Durin::EPixelFormat::BC5_UNORM,
		Durin::EPixelFormat::BC7_UNORM,
		Durin::EPixelFormat::BC7_UNORM_SRGB};
	for (Durin::EPixelFormat Format : Formats)
	{
		const Durin::FTexturePlatformData Expected = MakePlatformData(Format);
		std::vector<Durin::uint8> First;
		std::vector<Durin::uint8> Second;
		std::string Error;
		ASSERT_TRUE(Durin::EncodeTexture2DPayload(
			Expected, Durin::Asset::ECookTargetPlatform::Win64,
			Durin::Asset::ECookTargetProfile::Game, First, Error)) << Error;
		ASSERT_TRUE(Durin::EncodeTexture2DPayload(
			Expected, Durin::Asset::ECookTargetPlatform::Win64,
			Durin::Asset::ECookTargetProfile::Game, Second, Error)) << Error;
		EXPECT_EQ(First, Second);
		ASSERT_GE(First.size(), Durin::TexturePayloadHeaderSize);

		std::unique_ptr<Durin::FTexturePlatformData> Actual;
		ASSERT_TRUE(Durin::DecodeTexture2DPayload(
			First, Durin::Asset::ECookTargetPlatform::Win64,
			Durin::Asset::ECookTargetProfile::Game, Actual, Error)) << Error;
		ASSERT_NE(Actual, nullptr);
		ExpectPlatformDataEqual(*Actual, Expected);
	}
}

TEST(FTextureDerivedDataTests, PayloadRejectsMalformedDataTransactionally)
{
	const Durin::FTexturePlatformData Expected = MakePlatformData();
	std::vector<Durin::uint8> Bytes;
	std::string Error;
	ASSERT_TRUE(Durin::EncodeTexture2DPayload(
		Expected, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Bytes, Error)) << Error;
	auto Existing = std::make_unique<Durin::FTexturePlatformData>(Expected);
	Durin::FTexturePlatformData* ExistingAddress = Existing.get();

	auto WrongProfile = Bytes;
	WriteU32(WrongProfile, 16, static_cast<Durin::uint32>(Durin::Asset::ECookTargetProfile::EditorValidation));
	EXPECT_FALSE(Durin::DecodeTexture2DPayload(
		WrongProfile, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Existing, Error));
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto Corrupt = Bytes;
	Corrupt.back() ^= 0xff;
	EXPECT_FALSE(Durin::DecodeTexture2DPayload(
		Corrupt, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Existing, Error));
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto WrongRange = Bytes;
	WriteU32(WrongRange, Durin::TexturePayloadHeaderSize + 16, 1);
	EXPECT_FALSE(Durin::DecodeTexture2DPayload(
		WrongRange, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Existing, Error));
	EXPECT_EQ(Existing.get(), ExistingAddress);
}
