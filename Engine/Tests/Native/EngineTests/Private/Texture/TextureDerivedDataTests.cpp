#include "TextureTestSupport.h"

#include "Texture/TextureDerivedData.h"
#include "Texture/Texture2DDerivedData.h"
#include "Texture/TextureCubeDerivedData.h"
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
		for (uint32 Dimension : {5u, 2u, 1u})
		{
			const Durin::FPixelFormatLayout Layout =
				Durin::GetPixelFormatLayout(Result.PixelFormat, Dimension, Dimension);
			Durin::FTexture2DMipData& Mip = Result.Mips.emplace_back();
			Mip.Width = Dimension;
			Mip.Height = Dimension;
			Mip.RowPitch = static_cast<uint32>(Layout.RowPitch);
			Mip.Pixels.resize(static_cast<size_t>(Layout.DataSize),
				static_cast<std::byte>(Dimension));
		}
		return Result;
	}

	auto WriteU32(std::vector<std::byte>& Bytes, size_t Offset, uint32 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto MakeCubePlatformData() -> Durin::FTextureCubePlatformData
	{
		Durin::FTextureCubePlatformData Result;
		Result.PixelFormat = Durin::EPixelFormat::BC3_UNORM_SRGB;
		for (size_t FaceIndex = 0; FaceIndex < Result.Faces.size(); ++FaceIndex)
		{
			Result.Faces[FaceIndex] = MakePlatformData(Result.PixelFormat);
			for (Durin::FTexture2DMipData& Mip : Result.Faces[FaceIndex].Mips)
				std::ranges::fill(Mip.Pixels, static_cast<std::byte>(FaceIndex + 1));
		}
		return Result;
	}

	auto StorePlatformDataValue(
		const Durin::FTexturePlatformData& PlatformData,
		std::vector<std::byte>& OutBytes,
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
		std::span<const std::byte> Bytes,
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
	Durin::Asset::Build::FTexture2DBuildKeyInput Input{
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
		Durin::Asset::Build::BuildTexture2DDerivedDataKey(Input);
	EXPECT_EQ(Baseline, "5b1aa80fd0348f7d01d88e7c7687f39e");
	EXPECT_EQ(Baseline.size(), 32u);

	auto ExpectChange = [&Baseline](const Durin::Asset::Build::FTexture2DBuildKeyInput& Changed) {
		EXPECT_NE(Durin::Asset::Build::BuildTexture2DDerivedDataKey(
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
		"d905d2d277bfb013cfb44f8f0c8d8096",
		"1a580f6e95f71c6ed63bead79491040a",
		"480a95182680fbd3af44e79dbea8c122",
		"1a367b53725ed95df885a28474222131",
		"71b478236dcc3779cce0143badf3a228",
		"1249003c62e807a0158e03081b4584ca",
		"e292eee438d02ae3b3f03a723e30c29c"};
	for (size_t FormatIndex = 0; FormatIndex < Formats.size(); ++FormatIndex)
	{
		const Durin::EPixelFormat Format = Formats[FormatIndex];
		const Durin::FTexturePlatformData Expected = MakePlatformData(Format);
		std::vector<std::byte> First;
		std::vector<std::byte> Second;
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
	std::vector<std::byte> Bytes;
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
	std::vector<std::byte> Bytes;
	std::string Error;
	ASSERT_TRUE(StorePlatformDataValue(Expected, Bytes, Error)) << Error;
	auto Existing = std::make_unique<Durin::FTexturePlatformData>(Expected);
	Durin::FTexturePlatformData* ExistingAddress = Existing.get();

	auto WrongProfile = Bytes;
	WriteU32(WrongProfile, 16, static_cast<uint32>(Durin::Asset::ECookTargetProfile::EditorValidation));
	Durin::FPayloadDecodeResult DecodeResult = LoadPlatformDataValue(WrongProfile, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EPayloadDecodeError::Incompatible);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto Corrupt = Bytes;
	Corrupt.back() ^= std::byte{0xff};
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
	Durin::Asset::Build::FTextureCubeBuildKeyInput Input{
		.SourceLayout = Durin::Asset::Build::ETextureCubeBuildSourceLayout::SixFaces,
		.bSRGB = true,
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game};
	for (size_t Index = 0; Index < Input.FaceContentHashes.size(); ++Index)
		Input.FaceContentHashes[Index] = {Index + 1, Index + 101};
	std::string Baseline;
	std::string Error;
	Baseline = Durin::Asset::Build::BuildTextureCubeDerivedDataKey(Input, Error);
	ASSERT_FALSE(Baseline.empty()) << Error;
	EXPECT_EQ(Baseline, "9b662f5ddca0399ae3bf02bda265d860");
	EXPECT_EQ(Baseline.size(), 32u);

	auto Changed = Input;
	std::swap(Changed.FaceContentHashes[0], Changed.FaceContentHashes[1]);
	std::string Key;
	Key = Durin::Asset::Build::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_FALSE(Key.empty()) << Error;
	EXPECT_NE(Key, Baseline);
	Changed = Input;
	Changed.bSRGB = false;
	Key = Durin::Asset::Build::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_FALSE(Key.empty()) << Error;
	EXPECT_NE(Key, Baseline);
	Changed = Input;
	++Changed.ProjectionVersion;
	Key = Durin::Asset::Build::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_FALSE(Key.empty()) << Error;
	EXPECT_NE(Key, Baseline);

	Changed = {};
	Changed.SourceLayout = Durin::Asset::Build::ETextureCubeBuildSourceLayout::EquirectangularPanorama;
	Changed.PanoramaContentHash = {7, 11};
	Changed.FaceDimension = 512;
	Changed.ExposureEV = 1.0f;
	Changed.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64;
	Changed.TargetProfile = Durin::Asset::ECookTargetProfile::Game;
	Baseline = Durin::Asset::Build::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_FALSE(Baseline.empty()) << Error;
	auto ChangedPanorama = Changed;
	ChangedPanorama.FaceDimension = 256;
	Key = Durin::Asset::Build::BuildTextureCubeDerivedDataKey(ChangedPanorama, Error);
	ASSERT_FALSE(Key.empty()) << Error;
	EXPECT_NE(Key, Baseline);
	ChangedPanorama = Changed;
	ChangedPanorama.ExposureEV = 2.0f;
	Key = Durin::Asset::Build::BuildTextureCubeDerivedDataKey(ChangedPanorama, Error);
	ASSERT_FALSE(Key.empty()) << Error;
	EXPECT_NE(Key, Baseline);
	ChangedPanorama = Changed;
	ChangedPanorama.ExposureEV = -0.0f;
	EXPECT_TRUE(Durin::Asset::Build::BuildTextureCubeDerivedDataKey(
		ChangedPanorama, Error).empty());
}

TEST(FTextureDerivedDataTests, CubePayloadRoundTripsDirectionalSlicesDeterministically)
{
	const Durin::FTextureCubePlatformData Expected = MakeCubePlatformData();
	std::vector<std::byte> First;
	std::vector<std::byte> Second;
	const Durin::FTexturePlatformSerializationContext Context{
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game};
	Durin::FCanonicalMemoryWriter FirstWriter(
		First, Durin::EArchivePurpose::DerivedDataPayload);
	const_cast<Durin::FTextureCubePlatformData&>(Expected).Serialize(FirstWriter, Context);
	ASSERT_FALSE(FirstWriter.HasError());
	Durin::FCanonicalMemoryWriter SecondWriter(
		Second, Durin::EArchivePurpose::DerivedDataPayload);
	const_cast<Durin::FTextureCubePlatformData&>(Expected).Serialize(SecondWriter, Context);
	ASSERT_FALSE(SecondWriter.HasError());
	EXPECT_EQ(First, Second);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(First).ToString(),
		"b58d569e2b733237bee9037ab4fab262");

	Durin::FTextureCubePlatformData Actual;
	Durin::FCanonicalMemoryReader Reader(First, Durin::EArchivePurpose::DerivedDataPayload);
	Actual.Serialize(Reader, Context);
	ASSERT_FALSE(Reader.HasError());
	for (size_t FaceIndex = 0; FaceIndex < Expected.Faces.size(); ++FaceIndex)
		ExpectPlatformDataEqual(Actual.Faces[FaceIndex], Expected.Faces[FaceIndex]);

	auto DifferentProducer = First;
	WriteU32(DifferentProducer, 8, Durin::TextureCubeBuilderVersion + 17);
	Durin::FCanonicalMemoryReader CompatibleReader(
		DifferentProducer, Durin::EArchivePurpose::DerivedDataPayload);
	Actual.Serialize(CompatibleReader, Context);
	EXPECT_FALSE(CompatibleReader.HasError()) << CompatibleReader.GetError();

	auto WrongOrder = First;
	WriteU32(WrongOrder, Durin::TexturePayloadHeaderSize, 1);
	const auto Existing = Actual;
	Durin::FCanonicalMemoryReader CorruptReader(
		WrongOrder, Durin::EArchivePurpose::DerivedDataPayload);
	Actual.Serialize(CorruptReader, Context);
	EXPECT_TRUE(CorruptReader.HasError());
	for (size_t FaceIndex = 0; FaceIndex < Existing.Faces.size(); ++FaceIndex)
		ExpectPlatformDataEqual(Actual.Faces[FaceIndex], Existing.Faces[FaceIndex]);
}
