#include "TextureTestSupport.h"

#include "Texture/TextureDerivedData.h"
#include "Runtime/Engine/Private/Texture/TextureDerivedDataKey.h"
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

	auto WriteU32(Durin::FByteArray& Bytes, size_t Offset, uint32 Value) -> void
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
		Durin::FByteArray& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		Durin::FCanonicalMemoryWriter Ar(
			OutBytes, Durin::EArchivePurpose::DerivedDataPayload);
		const_cast<Durin::FTexturePlatformData&>(PlatformData).Serialize(Ar, {
			.TargetPlatform = Durin::ECookTargetPlatform::Win64,
			.TargetProfile = Durin::ECookTargetProfile::Game});
		OutError = Ar.GetError();
		return !Ar.HasError();
	}

	auto LoadPlatformDataValue(
		std::span<const std::byte> Bytes,
		std::unique_ptr<Durin::FTexturePlatformData>& OutPlatformData)
		-> Durin::FDecodeResult
	{
		auto Candidate = std::make_unique<Durin::FTexturePlatformData>();
		Durin::FCanonicalMemoryReader Ar(
			Bytes, Durin::EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(Ar, {
			.TargetPlatform = Durin::ECookTargetPlatform::Win64,
			.TargetProfile = Durin::ECookTargetProfile::Game});
		if (!Ar.HasError()) Durin::RequireArchiveEnd(Ar);
		if (Ar.HasError())
		{
			return {
				.Code = Ar.GetFailure()
					&& Ar.GetFailure()->Code == Durin::EArchiveFailureCode::UnsupportedVersion
					? Durin::EDecodeError::Incompatible
					: Durin::EDecodeError::Corrupt,
				.Message = std::string(Ar.GetError())};
		}
		OutPlatformData = std::move(Candidate);
		return {};
	}
}

TEST(FTextureDerivedDataTests, CanonicalKeyCoversEverySemanticInput)
{
	Durin::FTexture2DBuildKeyInput Input{
		.ImportedDataIdentity = {0x0123456789abcdefull, 0xfedcba9876543210ull},
		.Usage = Durin::ETextureUsage::Color,
		.bSRGB = true,
		.CompressionQuality = Durin::ETextureCompressionQuality::Normal,
		.AlphaMipMode = Durin::ETextureAlphaMipMode::Average,
		.MaximumResolution = 2048,
		.AlphaCoverageThreshold = 0.5f,
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game};
	const Durin::FCacheKeyProxy Baseline =
		Durin::BuildTexture2DDerivedDataKey(Input);
	EXPECT_EQ(Baseline.ToString(), "cdd3bcbe366a71238c7d41125720ceb0");
	EXPECT_EQ(Baseline.ToString().size(), 32u);

	auto ExpectChange = [&Baseline](const Durin::FTexture2DBuildKeyInput& Changed) {
		EXPECT_NE(Durin::BuildTexture2DDerivedDataKey(
			Changed), Baseline);
	};
	auto Changed = Input;
	Changed.ImportedDataIdentity.HashLow ^= 1;
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
	Changed.TargetProfile = Durin::ECookTargetProfile::EditorValidation;
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
		"54385c3a0cb5b3a1f3f826f4405e8296",
		"5430c7ce42654d5444a8cd715c48c42a",
		"1c767b7f843e08dc041676d001e2398f",
		"9fd75a4d3107d7fca52ea4a73baa5eb2",
		"89086e0fa07650ad7bdc6390c873873b",
		"4d53f58d9c8a4c8db6ac7e6a7fc0fc00",
		"55e8358b0d284ca4c6be60939edf97dd"};
	for (size_t FormatIndex = 0; FormatIndex < Formats.size(); ++FormatIndex)
	{
		const Durin::EPixelFormat Format = Formats[FormatIndex];
		const Durin::FTexturePlatformData Expected = MakePlatformData(Format);
		Durin::FByteArray First;
		Durin::FByteArray Second;
		std::string Error;
		ASSERT_TRUE(StorePlatformDataValue(Expected, First, Error)) << Error;
		ASSERT_TRUE(StorePlatformDataValue(Expected, Second, Error)) << Error;
		EXPECT_EQ(First, Second);
		EXPECT_EQ(Durin::FXxHash128::HashBuffer(First).ToString(),
			ExpectedPayloadHashes[FormatIndex])
			<< "format index " << FormatIndex;
		if (FormatIndex == 0) EXPECT_EQ(First.size(), 264u);
		ASSERT_GE(First.size(), Durin::TexturePayloadHeaderSize);

		std::unique_ptr<Durin::FTexturePlatformData> Actual;
		const Durin::FDecodeResult DecodeResult =
			LoadPlatformDataValue(First, Actual);
		ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
		ASSERT_NE(Actual, nullptr);
		ExpectPlatformDataEqual(*Actual, Expected);
	}
}

TEST(FTextureDerivedDataTests, PlatformDataOwnsCanonicalSerialization)
{
	Durin::FTexturePlatformData Expected = MakePlatformData();
	Durin::FByteArray Bytes;
	Durin::FCanonicalMemoryWriter Writer(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Expected.Serialize(Writer, {
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game});
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();

	Durin::FTexturePlatformData Actual;
	Durin::FCanonicalMemoryReader Reader(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Actual.Serialize(Reader, {
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game});
	ASSERT_FALSE(Reader.HasError()) << Reader.GetError();
	EXPECT_TRUE(Durin::RequireArchiveEnd(Reader));
	ExpectPlatformDataEqual(Actual, Expected);
}

TEST(FTextureDerivedDataTests, PayloadRejectsMalformedDataTransactionally)
{
	const Durin::FTexturePlatformData Expected = MakePlatformData();
	Durin::FByteArray Bytes;
	std::string Error;
	ASSERT_TRUE(StorePlatformDataValue(Expected, Bytes, Error)) << Error;
	auto Existing = std::make_unique<Durin::FTexturePlatformData>(Expected);
	Durin::FTexturePlatformData* ExistingAddress = Existing.get();

	auto WrongProfile = Bytes;
	WriteU32(WrongProfile, 16, static_cast<uint32>(Durin::ECookTargetProfile::EditorValidation));
	Durin::FDecodeResult DecodeResult = LoadPlatformDataValue(WrongProfile, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EDecodeError::Incompatible);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto Corrupt = Bytes;
	Corrupt.back() ^= std::byte{0xff};
	DecodeResult = LoadPlatformDataValue(Corrupt, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EDecodeError::Corrupt);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto WrongRange = Bytes;
	WriteU32(WrongRange, Durin::TexturePayloadHeaderSize + 16, 1);
	DecodeResult = LoadPlatformDataValue(WrongRange, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EDecodeError::Corrupt);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto UnsupportedSchema = Bytes;
	WriteU32(UnsupportedSchema, 4, Durin::TexturePayloadSchemaVersion + 1);
	DecodeResult = LoadPlatformDataValue(UnsupportedSchema, Existing);
	EXPECT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.Code, Durin::EDecodeError::Incompatible);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto DifferentBuilder = Bytes;
	WriteU32(DifferentBuilder, 8, Durin::Texture2DPayloadProducerVersion + 17);
	DecodeResult = LoadPlatformDataValue(DifferentBuilder, Existing);
	EXPECT_TRUE(DecodeResult) << DecodeResult.Message;
	EXPECT_NE(Existing.get(), ExistingAddress);
}

TEST(FTextureDerivedDataTests, CubeKeysCoverFaceOrderLayoutAndProjectionInputs)
{
	Durin::FTextureCubeBuildKeyInput Input{
		.SourceLayout = Durin::ETextureCubeBuildSourceLayout::SixFaces,
		.bSRGB = true,
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game};
	for (size_t Index = 0; Index < Input.FaceContentHashes.size(); ++Index)
		Input.FaceContentHashes[Index] = {Index + 1, Index + 101};
	Durin::FCacheKeyProxy Baseline;
	std::string Error;
	Baseline = Durin::BuildTextureCubeDerivedDataKey(Input, Error);
	ASSERT_TRUE(Baseline.IsValid()) << Error;
	EXPECT_EQ(Baseline.ToString(), "7a2da53a236b7527a36561b24ea3ef5f");
	EXPECT_EQ(Baseline.ToString().size(), 32u);

	auto Changed = Input;
	std::swap(Changed.FaceContentHashes[0], Changed.FaceContentHashes[1]);
	Durin::FCacheKeyProxy Key;
	Key = Durin::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_TRUE(Key.IsValid()) << Error;
	EXPECT_NE(Key, Baseline);
	Changed = Input;
	Changed.bSRGB = false;
	Key = Durin::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_TRUE(Key.IsValid()) << Error;
	EXPECT_NE(Key, Baseline);
	Changed = Input;
	++Changed.ProjectionVersion;
	Key = Durin::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_TRUE(Key.IsValid()) << Error;
	EXPECT_NE(Key, Baseline);

	Changed = {};
	Changed.SourceLayout = Durin::ETextureCubeBuildSourceLayout::EquirectangularPanorama;
	Changed.PanoramaContentHash = {7, 11};
	Changed.FaceDimension = 512;
	Changed.ExposureEV = 1.0f;
	Changed.TargetPlatform = Durin::ECookTargetPlatform::Win64;
	Changed.TargetProfile = Durin::ECookTargetProfile::Game;
	Baseline = Durin::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_TRUE(Baseline.IsValid()) << Error;
	EXPECT_EQ(Baseline.ToString(), "1abd0937fc15c134e3b1479d853916f7");
	auto ChangedPanorama = Changed;
	ChangedPanorama.FaceDimension = 256;
	Key = Durin::BuildTextureCubeDerivedDataKey(ChangedPanorama, Error);
	ASSERT_TRUE(Key.IsValid()) << Error;
	EXPECT_NE(Key, Baseline);
	ChangedPanorama = Changed;
	ChangedPanorama.ExposureEV = 2.0f;
	Key = Durin::BuildTextureCubeDerivedDataKey(ChangedPanorama, Error);
	ASSERT_TRUE(Key.IsValid()) << Error;
	EXPECT_NE(Key, Baseline);
	ChangedPanorama = Changed;
	ChangedPanorama.ExposureEV = -0.0f;
	EXPECT_FALSE(Durin::BuildTextureCubeDerivedDataKey(
		ChangedPanorama, Error).IsValid());
}

TEST(FTextureDerivedDataTests, CubePayloadRoundTripsDirectionalSlicesDeterministically)
{
	const Durin::FTextureCubePlatformData Expected = MakeCubePlatformData();
	Durin::FByteArray First;
	Durin::FByteArray Second;
	const Durin::FTexturePlatformSerializationContext Context{
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game};
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
		"8e22a84dac1195860e4e3860199b8dda");
	EXPECT_EQ(First.size(), 1376u);

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
