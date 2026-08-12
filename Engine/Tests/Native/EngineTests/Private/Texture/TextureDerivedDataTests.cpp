#include "TextureTestSupport.h"

#include "Texture/TextureDerivedData.h"
#include "Texture/TextureDerivedDataWriter.h"
#include "Texture/TextureCube.h"
#include "Serialization/Archive.h"

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

	auto MakeCubePlatformData() -> Durin::FTextureCubePlatformData
	{
		Durin::FTextureCubePlatformData Result;
		Result.PixelFormat = Durin::EPixelFormat::BC3_UNORM_SRGB;
		for (size_t FaceIndex = 0; FaceIndex < Result.Faces.size(); ++FaceIndex)
		{
			Result.Faces[FaceIndex] = MakePlatformData(Result.PixelFormat);
			for (Durin::FTexture2DMipData& Mip : Result.Faces[FaceIndex].Mips)
				std::ranges::fill(Mip.Pixels, static_cast<Durin::uint8>(FaceIndex + 1));
		}
		return Result;
	}

	auto StorePlatformDataValue(
		const Durin::FTexturePlatformData& PlatformData,
		std::vector<Durin::uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		Durin::FCanonicalMemoryWriter Ar(
			OutBytes, Durin::EArchivePurpose::DerivedDataPayload);
		const_cast<Durin::FTexturePlatformData&>(PlatformData).Serialize(Ar, {
			.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Durin::Asset::ECookTargetProfile::Game});
		OutError = Ar.GetError();
		return !Ar.HasError();
	}

	auto LoadPlatformDataValue(
		std::span<const Durin::uint8> Bytes,
		std::unique_ptr<Durin::FTexturePlatformData>& OutPlatformData)
		-> Durin::FPayloadDecodeResult
	{
		auto Candidate = std::make_unique<Durin::FTexturePlatformData>();
		Durin::FCanonicalMemoryReader Ar(
			Bytes, Durin::EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(Ar, {
			.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Durin::Asset::ECookTargetProfile::Game});
		if (!Ar.HasError()) Durin::RequireArchiveEnd(Ar);
		if (Ar.HasError())
		{
			return {
				.Code = Ar.GetFailure()
					&& Ar.GetFailure()->Code == Durin::EArchiveFailureCode::UnsupportedVersion
					? Durin::EPayloadDecodeError::Incompatible
					: Durin::EPayloadDecodeError::Corrupt,
				.Message = std::string(Ar.GetError())};
		}
		OutPlatformData = std::move(Candidate);
		return {};
	}
}

TEST(FTextureDerivedDataTests, CanonicalKeyCoversEverySemanticInput)
{
	Durin::AssetBuild::FTexture2DBuildKeyInput Input{
		.SourceContentHash = {0x0123456789abcdefull, 0xfedcba9876543210ull},
		.Usage = Durin::ETextureUsage::Color,
		.bSRGB = true,
		.CompressionQuality = Durin::ETextureCompressionQuality::Normal,
		.AlphaMipMode = Durin::ETextureAlphaMipMode::Average,
		.MaximumResolution = 2048,
		.AlphaCoverageThreshold = 0.5f,
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game};
	const std::string Baseline =
		Durin::AssetBuild::TextureDerivedDataWriter::BuildTexture2DDerivedDataKey(Input);
	EXPECT_EQ(Baseline, "ceabc87aee9e8db676c2f6c13020593f");
	EXPECT_EQ(Baseline.size(), 32u);

	auto ExpectChange = [&Baseline](const Durin::AssetBuild::FTexture2DBuildKeyInput& Changed) {
		EXPECT_NE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTexture2DDerivedDataKey(
			Changed), Baseline);
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
	constexpr std::array<std::string_view, 7> ExpectedPayloadHashes{
		"77bdfeb3d6ca79944202b1c8313f9f23",
		"5d8d35cf6e3bd1310adfcf8be175af58",
		"ca544d4a8eba2254722aeea249712e45",
		"5eb1ce657248bc93efeaac279a0720bd",
		"db4d2a5ecf8cf92991ff04c0f95b4b53",
		"20293487d2903b76c4ae42cc49e69cee",
		"01efb428f4563742aeeaaa3073a62d36"};
	for (size_t FormatIndex = 0; FormatIndex < Formats.size(); ++FormatIndex)
	{
		const Durin::EPixelFormat Format = Formats[FormatIndex];
		const Durin::FTexturePlatformData Expected = MakePlatformData(Format);
		std::vector<Durin::uint8> First;
		std::vector<Durin::uint8> Second;
		std::string Error;
		ASSERT_TRUE(StorePlatformDataValue(Expected, First, Error)) << Error;
		ASSERT_TRUE(StorePlatformDataValue(Expected, Second, Error)) << Error;
		EXPECT_EQ(First, Second);
		EXPECT_EQ(Durin::FXxHash128::HashBuffer(First).ToString(),
			ExpectedPayloadHashes[FormatIndex])
			<< "format index " << FormatIndex;
		ASSERT_GE(First.size(), Durin::TexturePayloadHeaderSize);

		std::unique_ptr<Durin::FTexturePlatformData> Actual;
		const Durin::FPayloadDecodeResult DecodeResult =
			LoadPlatformDataValue(First, Actual);
		ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
		ASSERT_NE(Actual, nullptr);
		ExpectPlatformDataEqual(*Actual, Expected);
	}
}

TEST(FTextureDerivedDataTests, PlatformDataOwnsCanonicalSerialization)
{
	Durin::FTexturePlatformData Expected = MakePlatformData();
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Expected.Serialize(Writer, {
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game});
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();

	Durin::FTexturePlatformData Actual;
	Durin::FCanonicalMemoryReader Reader(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Actual.Serialize(Reader, {
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game});
	ASSERT_FALSE(Reader.HasError()) << Reader.GetError();
	EXPECT_TRUE(Durin::RequireArchiveEnd(Reader));
	ExpectPlatformDataEqual(Actual, Expected);
}

TEST(FTextureDerivedDataTests, PayloadRejectsMalformedDataTransactionally)
{
	const Durin::FTexturePlatformData Expected = MakePlatformData();
	std::vector<Durin::uint8> Bytes;
	std::string Error;
	ASSERT_TRUE(StorePlatformDataValue(Expected, Bytes, Error)) << Error;
	auto Existing = std::make_unique<Durin::FTexturePlatformData>(Expected);
	Durin::FTexturePlatformData* ExistingAddress = Existing.get();

	auto WrongProfile = Bytes;
	WriteU32(WrongProfile, 16, static_cast<Durin::uint32>(Durin::Asset::ECookTargetProfile::EditorValidation));
	Durin::FPayloadDecodeResult DecodeResult = LoadPlatformDataValue(WrongProfile, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EPayloadDecodeError::Corrupt);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto Corrupt = Bytes;
	Corrupt.back() ^= 0xff;
	DecodeResult = LoadPlatformDataValue(Corrupt, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EPayloadDecodeError::Corrupt);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto WrongRange = Bytes;
	WriteU32(WrongRange, Durin::TexturePayloadHeaderSize + 16, 1);
	DecodeResult = LoadPlatformDataValue(WrongRange, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EPayloadDecodeError::Corrupt);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto UnsupportedSchema = Bytes;
	WriteU32(UnsupportedSchema, 4, Durin::TexturePayloadSchemaVersion + 1);
	DecodeResult = LoadPlatformDataValue(UnsupportedSchema, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EPayloadDecodeError::Incompatible);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto DifferentBuilder = Bytes;
	WriteU32(DifferentBuilder, 8, Durin::Texture2DPayloadProducerVersion + 17);
	DecodeResult = LoadPlatformDataValue(DifferentBuilder, Existing);
	EXPECT_TRUE(DecodeResult) << DecodeResult.Message;
	EXPECT_NE(Existing.get(), ExistingAddress);
}

TEST(FTextureDerivedDataTests, CubeKeysCoverFaceOrderLayoutAndProjectionInputs)
{
	Durin::FTextureCubeDerivedDataKeyInput Input{
		.SourceLayout = Durin::ETextureCubeDerivedDataSourceLayout::SixFaces,
		.bSRGB = true,
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game};
	for (size_t Index = 0; Index < Input.FaceContentHashes.size(); ++Index)
		Input.FaceContentHashes[Index] = {Index + 1, Index + 101};
	std::string Baseline;
	std::string Error;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		Input, Baseline, Error)) << Error;
	EXPECT_EQ(Baseline, "61dec1a0575878952e205558f058bd2d");
	EXPECT_EQ(Baseline.size(), 32u);

	auto Changed = Input;
	std::swap(Changed.FaceContentHashes[0], Changed.FaceContentHashes[1]);
	std::string Key;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		Changed, Key, Error)) << Error;
	EXPECT_NE(Key, Baseline);
	Changed = Input;
	Changed.bSRGB = false;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		Changed, Key, Error)) << Error;
	EXPECT_NE(Key, Baseline);
	Changed = Input;
	++Changed.ProjectionVersion;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		Changed, Key, Error)) << Error;
	EXPECT_NE(Key, Baseline);

	Changed = {};
	Changed.SourceLayout = Durin::ETextureCubeDerivedDataSourceLayout::EquirectangularPanorama;
	Changed.PanoramaContentHash = {7, 11};
	Changed.FaceDimension = 512;
	Changed.ExposureEV = 1.0f;
	Changed.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64;
	Changed.TargetProfile = Durin::Asset::ECookTargetProfile::Game;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		Changed, Baseline, Error)) << Error;
	auto ChangedPanorama = Changed;
	ChangedPanorama.FaceDimension = 256;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		ChangedPanorama, Key, Error)) << Error;
	EXPECT_NE(Key, Baseline);
	ChangedPanorama = Changed;
	ChangedPanorama.ExposureEV = 2.0f;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		ChangedPanorama, Key, Error)) << Error;
	EXPECT_NE(Key, Baseline);
	ChangedPanorama = Changed;
	ChangedPanorama.ExposureEV = -0.0f;
	EXPECT_FALSE(Durin::AssetBuild::TextureDerivedDataWriter::BuildTextureCubeDerivedDataKey(
		ChangedPanorama, Key, Error));
}

TEST(FTextureDerivedDataTests, CubePayloadRoundTripsDirectionalSlicesDeterministically)
{
	const Durin::FTextureCubePlatformData Expected = MakeCubePlatformData();
	std::vector<Durin::uint8> First;
	std::vector<Durin::uint8> Second;
	std::string Error;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::EncodeTextureCubePayload(
		Expected, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, First, Error)) << Error;
	ASSERT_TRUE(Durin::AssetBuild::TextureDerivedDataWriter::EncodeTextureCubePayload(
		Expected, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Second, Error)) << Error;
	EXPECT_EQ(First, Second);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(First).ToString(),
		"d476639b4d52cee3e9b5db3b09e6c874");

	std::unique_ptr<Durin::FTextureCubePlatformData> Actual;
	Durin::FPayloadDecodeResult DecodeResult = Durin::DecodeTextureCubePayload(
		First, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Actual);
	ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
	ASSERT_NE(Actual, nullptr);
	for (size_t FaceIndex = 0; FaceIndex < Expected.Faces.size(); ++FaceIndex)
		ExpectPlatformDataEqual(Actual->Faces[FaceIndex], Expected.Faces[FaceIndex]);

	auto WrongOrder = First;
	WriteU32(WrongOrder, Durin::TexturePayloadHeaderSize, 1);
	Durin::FTextureCubePlatformData* Existing = Actual.get();
	DecodeResult = Durin::DecodeTextureCubePayload(
		WrongOrder, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Actual);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EPayloadDecodeError::Corrupt);
	EXPECT_EQ(Actual.get(), Existing);
}
