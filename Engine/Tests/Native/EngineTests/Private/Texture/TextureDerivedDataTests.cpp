#include "Runtime/Engine/Private/Texture/TextureBuildDiagnostics.h"
#include "Runtime/Engine/Private/Texture/TextureCubeBuildFunction.h"
#include "Runtime/Engine/Private/Texture/VolumeTextureBuildFunction.h"
#include "Runtime/Engine/Private/Texture/Texture2DBuildFunction.h"
#include "DerivedDataBuildSession.h"
#include "Runtime/Engine/Private/Asset/AssetBuildServicePrivate.h"
#include "Texture/ITextureBuildModule.h"
#include <expected>
#include "Texture/Texture2DBuildTypes.h"
#include "TextureTestSupport.h"

#include "Texture/TextureDerivedData.h"
#include "Runtime/Engine/Private/Texture/Texture2DSharedOutput.h"
#include "Runtime/Engine/Private/Texture/TexturePlatformSharedOutput.h"
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
			Mip.Pixels = Durin::FSharedByteBuffer::Take(Durin::FByteBuffer(static_cast<size_t>(Layout.DataSize),
				static_cast<std::byte>(Dimension)));
		}
		return Result;
	}

	auto WriteU32(Durin::FByteBuffer& Bytes, size_t Offset, uint32 Value) -> void
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
				Mip.Pixels = Durin::FSharedByteBuffer::Take(Durin::FByteBuffer(Mip.Pixels.size(), static_cast<std::byte>(FaceIndex + 1)));
		}
		return Result;
	}

	auto StorePlatformDataValue(
		const Durin::FTexturePlatformData& PlatformData,
		Durin::FByteBuffer& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		Durin::FCanonicalMemoryWriter Ar(
			OutBytes, Durin::EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
		const_cast<Durin::FTexturePlatformData&>(PlatformData).Serialize(Ar);
		OutError = Ar.GetError();
		return !Ar.IsError();
	}

	auto LoadPlatformDataValue(
		Durin::FByteView Bytes,
		std::unique_ptr<Durin::FTexturePlatformData>& OutPlatformData)
		-> std::expected<void, Durin::FArchiveFailure>
	{
		auto Candidate = std::make_unique<Durin::FTexturePlatformData>();
		Durin::FCanonicalMemoryReader Ar(
			Bytes, Durin::EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
		Candidate->Serialize(Ar);
		if (!Ar.IsError()) Durin::RequireArchiveEnd(Ar);
		if (Ar.IsError())
		{
			return std::unexpected(*Ar.GetFailure());
		}
		OutPlatformData = std::move(Candidate);
		return {};
	}
}

namespace
{
	struct FMutableTextureOutput
	{
		std::string Schema;
		uint32 SchemaVersion = 0;
		Durin::FSharedByteBuffer Metadata;
		std::vector<std::pair<Durin::DerivedData::FValueId, Durin::FSharedByteBuffer>> Values;
		auto Build() && -> std::expected<Durin::DerivedData::FBuildOutput, std::string>
		{
			using namespace Durin::DerivedData;
			FBuildOutputBuilder Builder(std::move(Schema), SchemaVersion);
			auto Object = MakeBuildMetadata(Metadata);
			if (!Object || !Builder.AddMeta(FValueId::FromName("Metadata"), std::move(*Object)))
				return std::unexpected("Invalid test metadata.");
			for (auto& [Id, Data] : Values) if (!Builder.AddValue(Id, std::move(Data)))
				return std::unexpected("Invalid test value.");
			return std::move(Builder).Build();
		}
	};
	auto CopyTextureOutput(const Durin::DerivedData::FBuildOutput& Output) -> FMutableTextureOutput
	{
		FMutableTextureOutput Copy{.Schema = std::string(Output.GetSchema()), .SchemaVersion = Output.GetSchemaVersion(),
			.Metadata = Durin::DerivedData::GetBuildMetadataPayload(Output)};
		for (const auto& Value : Output.GetValues()) Copy.Values.emplace_back(Value.Id, Value.Value.GetData());
		return Copy;
	}
}

TEST(FTextureDerivedDataTests, CanonicalKeyCoversEverySemanticInput)
{
	Durin::FTexture2DBuildKeyInput Input{
		.SourceIdentity = {0x0123456789abcdefull, 0xfedcba9876543210ull},
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
	// Schema-2 action and shared output schema invalidate the legacy cache key.
	EXPECT_EQ(Baseline.ToString(), "078ed01da2fbf2565e65dc6290734eda");
	EXPECT_EQ(Baseline.ToString().size(), 32u);

	auto ExpectChange = [&Baseline](const Durin::FTexture2DBuildKeyInput& Changed) {
		EXPECT_NE(Durin::BuildTexture2DDerivedDataKey(
			Changed), Baseline);
	};
	auto Changed = Input;
	Changed.SourceIdentity.HashLow ^= 1;
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
	++Changed.OutputSchemaVersion;
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
		Durin::FByteBuffer First;
		Durin::FByteBuffer Second;
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
		const std::expected<void, Durin::FArchiveFailure> DecodeResult =
			LoadPlatformDataValue(First, Actual);
		ASSERT_TRUE(DecodeResult) << DecodeResult.error().Message;
		ASSERT_NE(Actual, nullptr);
		ExpectPlatformDataEqual(*Actual, Expected);
	}
}

TEST(FTextureDerivedDataTests, PlatformDataOwnsCanonicalSerialization)
{
	Durin::FTexturePlatformData Expected = MakePlatformData();
	Durin::FByteBuffer Bytes;
	Durin::FCanonicalMemoryWriter Writer(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Expected.Serialize(Writer);
	ASSERT_FALSE(Writer.IsError()) << Writer.GetError();

	Durin::FTexturePlatformData Actual;
	Durin::FCanonicalMemoryReader Reader(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Actual.Serialize(Reader);
	ASSERT_FALSE(Reader.IsError()) << Reader.GetError();
	EXPECT_TRUE(Durin::RequireArchiveEnd(Reader));
	ExpectPlatformDataEqual(Actual, Expected);
}

TEST(FTextureDerivedDataTests, PayloadRejectsMalformedDataTransactionally)
{
	const Durin::FTexturePlatformData Expected = MakePlatformData();
	Durin::FByteBuffer Bytes;
	std::string Error;
	ASSERT_TRUE(StorePlatformDataValue(Expected, Bytes, Error)) << Error;
	auto Existing = std::make_unique<Durin::FTexturePlatformData>(Expected);
	Durin::FTexturePlatformData* ExistingAddress = Existing.get();

	auto WrongProfile = Bytes;
	WriteU32(WrongProfile, 16, static_cast<uint32>(Durin::ECookTargetProfile::EditorValidation));
	std::expected<void, Durin::FArchiveFailure> DecodeResult = LoadPlatformDataValue(WrongProfile, Existing);
	ASSERT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.error().Code, Durin::EArchiveFailureCode::UnsupportedTarget);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto Corrupt = Bytes;
	Corrupt.back() ^= std::byte{0xff};
	DecodeResult = LoadPlatformDataValue(Corrupt, Existing);
	ASSERT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.error().Code, Durin::EArchiveFailureCode::InvalidData);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto WrongRange = Bytes;
	WriteU32(WrongRange, Durin::TexturePayloadHeaderSize + 16, 1);
	DecodeResult = LoadPlatformDataValue(WrongRange, Existing);
	ASSERT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.error().Code, Durin::EArchiveFailureCode::InvalidData);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto UnsupportedSchema = Bytes;
	WriteU32(UnsupportedSchema, 4, Durin::TexturePayloadSchemaVersion + 1);
	DecodeResult = LoadPlatformDataValue(UnsupportedSchema, Existing);
	ASSERT_FALSE(DecodeResult);
	EXPECT_EQ(DecodeResult.error().Code, Durin::EArchiveFailureCode::UnsupportedVersion);
	EXPECT_EQ(Existing.get(), ExistingAddress);

	auto DifferentBuilder = Bytes;
	WriteU32(DifferentBuilder, 8, Durin::Texture2DPayloadProducerVersion + 17);
	DecodeResult = LoadPlatformDataValue(DifferentBuilder, Existing);
	EXPECT_TRUE(DecodeResult) << DecodeResult.error().Message;
	EXPECT_NE(Existing.get(), ExistingAddress);
}

TEST(FTextureDerivedDataTests, CubeKeysCoverCanonicalSourceLayoutAndProjectionInputs)
{
	Durin::FTextureCubeBuildKeyInput Input{
		.SourceLayout = Durin::ETextureCubeBuildSourceLayout::SixFaces,
		.CanonicalSourceIdentity = {1, 101},
		.bSRGB = true,
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game};
	Durin::FCacheKeyProxy Baseline;
	std::string Error;
	Baseline = Durin::BuildTextureCubeDerivedDataKey(Input, Error);
	ASSERT_TRUE(Baseline.IsValid()) << Error;
	EXPECT_EQ(Baseline.ToString(), "08625bbecab440c991b2ebf28040bb51");
	EXPECT_EQ(Baseline.ToString().size(), 32u);

	auto Changed = Input;
	Changed.CanonicalSourceIdentity.HashLow++;
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
	Changed.CanonicalSourceIdentity = {7, 11};
	Changed.FaceDimension = 512;
	Changed.ExposureEV = 1.0f;
	Changed.TargetPlatform = Durin::ECookTargetPlatform::Win64;
	Changed.TargetProfile = Durin::ECookTargetProfile::Game;
	Baseline = Durin::BuildTextureCubeDerivedDataKey(Changed, Error);
	ASSERT_TRUE(Baseline.IsValid()) << Error;
	EXPECT_EQ(Baseline.ToString(), "cf06d2f8cf0bc5ed65421b0f97c96885");
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

TEST(FTextureDerivedDataTests, TextureKeyValidationRejectsInvalidInputs)
{
	using namespace Durin;
	auto ExpectInvalid = [](auto Input) {
		FArchiveFailure Failure;
		ASSERT_FALSE(Input.IsValid(&Failure));
		if constexpr (std::is_same_v<decltype(Input), FTexture2DBuildKeyInput>)
		{
			EXPECT_TRUE(BuildTexture2DDerivedDataKeyBytes(Input).empty());
		}
		else
		{
			std::string Error;
			EXPECT_TRUE(BuildVolumeTextureDerivedDataKeyBytes(Input, Error).empty());
			EXPECT_FALSE(Error.empty());
		}
	};
	FTexture2DBuildKeyInput Texture{
		.TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game};
	EXPECT_TRUE(Texture.IsValid());
	auto InvalidTexture = Texture;
	InvalidTexture.Usage = static_cast<ETextureUsage>(255);
	ExpectInvalid(InvalidTexture);
	EXPECT_FALSE(BuildTexture2DDerivedDataKey(InvalidTexture).IsValid());
	InvalidTexture = Texture;
	InvalidTexture.AlphaCoverageThreshold = 1.0f;
	ExpectInvalid(InvalidTexture);
	InvalidTexture = Texture;
	InvalidTexture.TargetPlatform = ECookTargetPlatform::Invalid;
	ExpectInvalid(InvalidTexture);
	FVolumeTextureBuildKeyInput Volume{
		.Width = 4, .Height = 4, .Depth = 4,
		.TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game};
	EXPECT_TRUE(Volume.IsValid());
	auto InvalidVolume = Volume;
	InvalidVolume.Depth = 0;
	ExpectInvalid(InvalidVolume);
	InvalidVolume = Volume;
	++InvalidVolume.SourcePayloadSchemaVersion;
	ExpectInvalid(InvalidVolume);
}

TEST(FTextureDerivedDataTests, InvalidCubeKeyInputsFailBeforeWriting)
{
	using namespace Durin;
	FTextureCubeBuildKeyInput Input{
		.TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game};
	FArchiveFailure Failure{.Message = "previous failure"};
	EXPECT_TRUE(Input.IsValid(&Failure));
	EXPECT_TRUE(Failure.Message.empty());
	auto ExpectInvalid = [](const FTextureCubeBuildKeyInput& Invalid,
		EArchiveFailureCode ExpectedCode) {
		FArchiveFailure Expected;
		EXPECT_FALSE(Invalid.IsValid());
		ASSERT_FALSE(Invalid.IsValid(&Expected));
		EXPECT_EQ(Expected.Code, ExpectedCode);
		EXPECT_FALSE(Expected.Message.empty());
		std::string Error;
		EXPECT_TRUE(BuildTextureCubeDerivedDataKeyBytes(Invalid, Error).empty());
		EXPECT_FALSE(Error.empty());
	};
	auto Invalid = Input;
	Invalid.TargetPlatform = ECookTargetPlatform::Invalid;
	ExpectInvalid(Invalid, EArchiveFailureCode::InvalidData);
	Invalid = Input;
	Invalid.ExposureEV = -0.0f;
	ExpectInvalid(Invalid, EArchiveFailureCode::InvalidData);
	Invalid = Input;
	Invalid.FaceDimension = MaximumTextureCubeDimension + 1;
	ExpectInvalid(Invalid, EArchiveFailureCode::LimitExceeded);
	Invalid = Input;
	Invalid.SourceLayout = static_cast<ETextureCubeBuildSourceLayout>(99);
	ExpectInvalid(Invalid, EArchiveFailureCode::InvalidData);
}

TEST(FTextureDerivedDataTests, CubePayloadRoundTripsDirectionalSlicesDeterministically)
{
	const Durin::FTextureCubePlatformData Expected = MakeCubePlatformData();
	Durin::FByteBuffer First;
	Durin::FByteBuffer Second;
	const Durin::FArchiveState Context{.Target = {"Win64", "Game"}};
	Durin::FCanonicalMemoryWriter FirstWriter(
		First, Durin::EArchivePurpose::DerivedDataPayload, Context);
	const_cast<Durin::FTextureCubePlatformData&>(Expected).Serialize(FirstWriter);
	ASSERT_FALSE(FirstWriter.IsError());
	Durin::FCanonicalMemoryWriter SecondWriter(
		Second, Durin::EArchivePurpose::DerivedDataPayload, Context);
	const_cast<Durin::FTextureCubePlatformData&>(Expected).Serialize(SecondWriter);
	ASSERT_FALSE(SecondWriter.IsError());
	EXPECT_EQ(First, Second);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(First).ToString(),
		"7ce2cb929232337de973aeddcfbb32d5");
	EXPECT_EQ(First.size(), 1376u);

	Durin::FTextureCubePlatformData Actual;
	Durin::FCanonicalMemoryReader Reader(First, Durin::EArchivePurpose::DerivedDataPayload, Context);
	Actual.Serialize(Reader);
	ASSERT_FALSE(Reader.IsError());
	for (size_t FaceIndex = 0; FaceIndex < Expected.Faces.size(); ++FaceIndex)
		ExpectPlatformDataEqual(Actual.Faces[FaceIndex], Expected.Faces[FaceIndex]);

	auto DifferentProducer = First;
	WriteU32(DifferentProducer, 8, Durin::TextureCubeBuilderVersion + 17);
	Durin::FCanonicalMemoryReader CompatibleReader(
		DifferentProducer, Durin::EArchivePurpose::DerivedDataPayload, Context);
	Actual.Serialize(CompatibleReader);
	EXPECT_FALSE(CompatibleReader.IsError()) << CompatibleReader.GetError();

	auto WrongOrder = First;
	WriteU32(WrongOrder, Durin::TexturePayloadHeaderSize, 1);
	const auto Existing = Actual;
	Durin::FCanonicalMemoryReader CorruptReader(
		WrongOrder, Durin::EArchivePurpose::DerivedDataPayload, Context);
	Actual.Serialize(CorruptReader);
	EXPECT_TRUE(CorruptReader.IsError());
	for (size_t FaceIndex = 0; FaceIndex < Existing.Faces.size(); ++FaceIndex)
		ExpectPlatformDataEqual(Actual.Faces[FaceIndex], Existing.Faces[FaceIndex]);
}

TEST(FTextureDerivedDataTests, ArchivesReplaceSequencesAndBoundAdjacentPayloads)
{
	using namespace Durin;
	FArchiveState Context{.Target = {"Win64", "Game"}};
	auto Check = [&](auto Source) {
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::Discovery, Context);
		Source.Serialize(Writer);
		ASSERT_FALSE(Writer.IsError()) << Writer.GetError();
		FByteBuffer ContextBytes;
		FCanonicalMemoryWriter ContextWriter(ContextBytes, EArchivePurpose::DerivedDataPayload,
			Context);
		Source.Serialize(ContextWriter);
		EXPECT_FALSE(ContextWriter.IsError());
		EXPECT_EQ(ContextBytes, Bytes);
		FCanonicalMemoryReader MissingContext(Bytes);
		auto DiscardedContext = Source;
		DiscardedContext.Serialize(MissingContext);
		ASSERT_TRUE(MissingContext.IsError());
		EXPECT_EQ(MissingContext.GetFailure()->Code, EArchiveFailureCode::UnsupportedTarget);
		EXPECT_EQ(MissingContext.Tell(), 0u);
		for (const FArchiveTarget Target : {FArchiveTarget{}, FArchiveTarget{"Win64", ""},
			FArchiveTarget{"Other", "Game"}, FArchiveTarget{"Win64", "Unknown"}})
		{
			FCountingArchive InvalidCounter(EArchivePurpose::DerivedDataPayload, {.Target = Target});
			FHashingArchive InvalidHasher(EArchivePurpose::DerivedDataPayload, {.Target = Target});
			Source.Serialize(InvalidCounter);
			Source.Serialize(InvalidHasher);
			ASSERT_TRUE(InvalidCounter.IsError());
			ASSERT_TRUE(InvalidHasher.IsError());
			EXPECT_EQ(InvalidCounter.GetFailure()->Code, EArchiveFailureCode::UnsupportedTarget);
			EXPECT_EQ(InvalidHasher.GetFailure()->Code, EArchiveFailureCode::UnsupportedTarget);
			EXPECT_EQ(InvalidCounter.Tell(), 0u);
			EXPECT_EQ(InvalidHasher.Tell(), 0u);
		}
		FCountingArchive Counter(EArchivePurpose::DerivedDataPayload, Context);
		Source.Serialize(Counter);
		EXPECT_FALSE(Counter.IsError());
		EXPECT_EQ(Counter.Tell(), Bytes.size());
		FHashingArchive Hasher(EArchivePurpose::DerivedDataPayload, Context);
		Source.Serialize(Hasher);
		EXPECT_FALSE(Hasher.IsError());
		EXPECT_EQ(Hasher.Finalize(), FXxHash128::HashBuffer(Bytes));
		auto Loaded = Source;
		FCanonicalMemoryReader Reader(Bytes, EArchivePurpose::DerivedDataPayload, Context);
		Loaded.Serialize(Reader);
		ASSERT_FALSE(Reader.IsError());
		ASSERT_TRUE(RequireArchiveEnd(Reader));
		FByteBuffer RoundTrip;
		FCanonicalMemoryWriter Rewriter(RoundTrip, EArchivePurpose::DerivedDataPayload, Context);
		Loaded.Serialize(Rewriter);
		EXPECT_EQ(RoundTrip, Bytes);
		FByteBuffer Joined = Bytes;
		Joined.insert(Joined.end(), Bytes.begin(), Bytes.end());
		FCanonicalMemoryReader Parent(Joined, EArchivePurpose::DerivedDataPayload, Context);
		Loaded.Serialize(Parent);
		ASSERT_FALSE(Parent.IsError());
		EXPECT_EQ(Parent.GetRemainingPayloadBytes(), Bytes.size());
		EXPECT_FALSE(RequireArchiveEnd(Parent));
		for (size_t Size = 0; Size < Bytes.size(); ++Size)
		{
			FCanonicalMemoryReader Truncated(FByteView(Bytes).first(Size),
				EArchivePurpose::DerivedDataPayload, Context);
			decltype(Source) Discarded;
			Discarded.Serialize(Truncated);
			ASSERT_TRUE(Truncated.IsError()) << Size;
		}
		auto Excessive = Bytes;
		WriteU32(Excessive, 40, std::numeric_limits<uint32>::max());
		FCanonicalMemoryReader Limited(Excessive, EArchivePurpose::DerivedDataPayload, Context);
		Loaded.Serialize(Limited);
		ASSERT_TRUE(Limited.IsError());
		EXPECT_EQ(Limited.GetFailure()->Code, EArchiveFailureCode::LimitExceeded);
		EXPECT_EQ(Limited.Tell(), TexturePayloadHeaderSize);
	};
	for (const char* Profile : {"Game", "EditorValidation"})
	{
		Context.Target.Profile = Profile;
		Check(MakePlatformData());
		Check(MakeCubePlatformData());
	}
}

TEST(FTextureDerivedDataTests, InputValidationRetainsSettingsAndMipContext)
{
	using namespace Durin;
	FTexture2DBuildSettings Settings;
	ASSERT_TRUE(ValidateTexture2DBuildSettings(Settings));
	Settings.Usage = static_cast<ETextureUsage>(255);
	const auto Usage = ValidateTexture2DBuildSettings(Settings);
	EXPECT_EQ(Usage.error().Code, ETexture2DInputError::InvalidUsage);
	Settings = {};
	EXPECT_EQ(static_cast<uint8>(Usage.error().Settings.Usage), 255);
	Settings.CompressionQuality = static_cast<ETextureCompressionQuality>(255);
	EXPECT_EQ(ValidateTexture2DBuildSettings(Settings).error().Code, ETexture2DInputError::InvalidCompressionQuality);
	Settings = {};
	Settings.AlphaMipMode = static_cast<ETextureAlphaMipMode>(255);
	EXPECT_EQ(ValidateTexture2DBuildSettings(Settings).error().Code, ETexture2DInputError::InvalidAlphaMipMode);
	Settings = {};
	Settings.AlphaCoverageThreshold = std::numeric_limits<float>::quiet_NaN();
	const auto Threshold = ValidateTexture2DBuildSettings(Settings);
	EXPECT_EQ(Threshold.error().Code, ETexture2DInputError::InvalidAlphaCoverageThreshold);
	EXPECT_TRUE(std::isnan(Threshold.error().Settings.AlphaCoverageThreshold));
	EXPECT_EQ(ValidateTexture2DSourceMips({}).error().Code, ETexture2DInputError::EmptyMips);
	std::vector<Image::FImage> Mips(1);
	EXPECT_EQ(ValidateTexture2DSourceMips(Mips).error().Code, ETexture2DInputError::InvalidImage);
	Image::FImageInfo Info{.Width = 2, .Height = 2, .Format = Image::ERawImageFormat::RGBA8,
		.GammaSpace = Image::EImageGammaSpace::Linear};
	FByteBuffer Pixels(16);
	auto ImageResult1 = Image::FImage::TryCreate(Info, Pixels);
	ASSERT_TRUE(ImageResult1);
	Mips.front() = std::move(*ImageResult1);
	Mips.push_back(Mips.front());
	const auto Dimensions = ValidateTexture2DSourceMips(Mips);
	EXPECT_EQ(Dimensions.error().Code, ETexture2DInputError::InvalidMipDimensions);
	Mips.clear();
	EXPECT_EQ(Dimensions.error().Index, 1u);
	EXPECT_EQ(Dimensions.error().Bytes, 32u);
	EXPECT_EQ(Dimensions.error().Base.Width, 2u);
	EXPECT_EQ(Dimensions.error().Actual.Width, 2u);
}

TEST(FTextureDerivedDataTests, CubeCanonicalSourceIdentityIncludesFaceOrder)
{
	using namespace Durin;
	std::array<Image::FImageView, TextureCubeFaceCount> Faces;
	for (size_t Index = 0; Index < Faces.size(); ++Index)
	{
		auto Face = Image::FImage::TryCreate({.Width = 1, .Height = 1,
			.Format = Image::ERawImageFormat::RGBA8, .GammaSpace = Image::EImageGammaSpace::SRGB},
			FByteBuffer{std::byte(Index), std::byte{0}, std::byte{0}, std::byte{255}});
		ASSERT_TRUE(Face); Faces[Index] = Face->GetView();
	}
	FTextureSource Original, Reordered;
	ASSERT_TRUE(Original.InitCube(Faces, 4));
	std::swap(Faces[0], Faces[1]);
	ASSERT_TRUE(Reordered.InitCube(Faces, 4));
	EXPECT_NE(Original.GetIdentity(), Reordered.GetIdentity());
	FTextureCubeBuildKeyInput Input{.CanonicalSourceIdentity = Original.GetIdentity(),
		.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game};
	std::string Error;
	const auto OriginalKey = BuildTextureCubeDerivedDataKey(Input, Error);
	ASSERT_TRUE(OriginalKey.IsValid()) << Error;
	Input.CanonicalSourceIdentity = Reordered.GetIdentity();
	const auto ReorderedKey = BuildTextureCubeDerivedDataKey(Input, Error);
	ASSERT_TRUE(ReorderedKey.IsValid()) << Error;
	EXPECT_NE(OriginalKey, ReorderedKey);
}

TEST(FTextureDerivedDataTests, SharedOutputRetainsGroupedMipsAndPreservesCookBytes)
{
	using namespace Durin;
	auto Source = MakePlatformData();
	uint64 Total = 0;
	for (const auto& Mip : Source.Mips) Total += Mip.Pixels.size();
	auto Owner = std::make_shared<FByteBuffer>(Total, std::byte{7});
	std::weak_ptr<const FByteBuffer> Lifetime = Owner;
	auto Backing = FSharedByteBuffer::Share(Owner);
	uint64 Offset = 0;
	for (auto& Mip : Source.Mips)
	{
		const auto Size = Mip.Pixels.size();
		Mip.Pixels = Backing.MakeView(Offset, Size);
		Offset += Size;
	}
	const auto* Original = Source.Mips[0].Pixels.data();
	FByteBuffer Before, After;
	std::string Error;
	ASSERT_TRUE(StorePlatformDataValue(Source, Before, Error)) << Error;
	auto Output = TexturePrivate::MakeTexture2DSharedOutput(Source, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Output) << Output.error();
	Source = {}; Backing = {}; Owner.reset();
	EXPECT_FALSE(Lifetime.expired());
	auto Key = DerivedData::FCacheKey::FromHash(DerivedData::FCacheBucket::FromString("TextureOutputFixture"), FXxHash128::HashBuffer("key"));
	auto Record = DerivedData::FCacheRecord::FromOutput(Key, *Output);
	ASSERT_TRUE(Record);
	Output = DerivedData::FBuildOutput{};
	auto Loaded = Record->ToOutput(Key);
	ASSERT_TRUE(Loaded);
	auto Product = TexturePrivate::AssembleTexture2DSharedOutput(*Loaded, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Product) << Product.error();
	Record = DerivedData::FCacheRecord{}; Loaded = DerivedData::FBuildOutput{};
	EXPECT_EQ(Product->Mips[0].Pixels.data(), Original);
	EXPECT_TRUE(Product->Mips[0].Pixels.SharesStorageWith(Product->Mips[1].Pixels));
	ASSERT_TRUE(StorePlatformDataValue(*Product, After, Error)) << Error;
	EXPECT_EQ(Before, After);
	// Mutation is an explicit owned copy, leaving the published shared view intact.
	FByteBuffer Editable(Product->Mips[0].Pixels.begin(), Product->Mips[0].Pixels.end());
	Editable[0] = std::byte{9};
	EXPECT_EQ(Product->Mips[0].Pixels[0], std::byte{7});
	Product = FTexturePlatformData{};
	EXPECT_TRUE(Lifetime.expired());
}

TEST(FTextureDerivedDataTests, SharedOutputRejectsMalformedLayoutWithoutAssembly)
{
	using namespace Durin;
	auto Output = TexturePrivate::MakeTexture2DSharedOutput(MakePlatformData(), ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Output);
	auto Copy = [&] { return CopyTextureOutput(*Output); };
	for (uint32 Case = 0; Case < 8; ++Case)
	{
		auto Data = Copy();
		switch (Case)
		{
		case 0: Data.SchemaVersion = 3; break;
		case 1: Data.Values.pop_back(); break;
		case 2: Data.Values[0].first = DerivedData::FValueId::FromName("Invalid.Mip"); break;
		case 3: Data.Values[0].second = FSharedByteBuffer::Take(FByteBuffer(1)); break;
		default:
		{
			FByteBuffer Metadata(Data.Metadata.begin(), Data.Metadata.end());
			if (Case == 4) WriteU32(Metadata, 16, MaximumTexture2DDimension + 1);
			if (Case == 5) WriteU32(Metadata, 24, 1);
			if (Case == 6) Metadata.push_back(std::byte{0});
			if (Case == 7) WriteU32(Metadata, 12, MaximumTextureMipCount + 1);
			Data.Metadata = FSharedByteBuffer::Take(std::move(Metadata));
		}
		}
		auto Invalid = std::move(Data).Build();
		ASSERT_TRUE(Invalid);
		EXPECT_FALSE(TexturePrivate::ReadTexture2DOutputLayout(*Invalid, ECookTargetPlatform::Win64, ECookTargetProfile::Game)) << Case;
	}
	EXPECT_FALSE(TexturePrivate::ReadTexture2DOutputLayout(*Output, ECookTargetPlatform::Win64, ECookTargetProfile::EditorValidation));
}

TEST(FTextureDerivedDataTests, AbandonedOutputAndFailedPersistenceDoNotChangeBlocks)
{
	using namespace Durin;
	auto Source = MakePlatformData();
	auto Output = TexturePrivate::MakeTexture2DSharedOutput(Source, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Output);
	const auto* Address = Source.Mips[0].Pixels.data();
	const auto Key = DerivedData::FCacheKey::FromHash(DerivedData::FCacheBucket::FromString("TextureOutputFixture"), FXxHash128::HashBuffer("key"));
	EXPECT_FALSE(DerivedData::FCacheRecord::FromOutput(Key, *Output, {.MaximumTotalBytes = 1}));
	FByteBuffer RejectedBytes;
	FCanonicalMemoryWriter RejectedWriter(RejectedBytes, EArchivePurpose::DerivedDataPayload,
		{.Target = {"Unsupported", "Game"}});
	Source.Serialize(RejectedWriter);
	EXPECT_TRUE(RejectedWriter.IsError());
	Output = DerivedData::FBuildOutput{};
	EXPECT_EQ(Source.Mips[0].Pixels.data(), Address);
	EXPECT_TRUE(Source.IsValid());
}

TEST(FTextureDerivedDataTests, DecodedCacheRecordsAssembleSharedMipsWithIdenticalCookBytes)
{
	using namespace Durin;
	const auto Source = MakePlatformData();
	FByteBuffer Expected;
	std::string Error;
	ASSERT_TRUE(StorePlatformDataValue(Source, Expected, Error)) << Error;
	auto Output = TexturePrivate::MakeTexture2DSharedOutput(Source, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Output);
	const auto Key = DerivedData::FCacheKey::FromHash(DerivedData::FCacheBucket::FromString("TextureOutputFixture"), FXxHash128::HashBuffer("encoded"));
	auto Record = DerivedData::FCacheRecord::FromOutput(Key, *Output);
	ASSERT_TRUE(Record);
	auto Raw = Record->Encode();
	ASSERT_TRUE(Raw);
	for (bool bCompress : {false, true})
	{
		auto Stored = bCompress ? DerivedData::FCacheRecord::CompressEncoded(*Raw) : Raw;
		ASSERT_TRUE(Stored);
		auto LoadedRecord = DerivedData::FCacheRecord::Decode(Key, *Stored);
		ASSERT_TRUE(LoadedRecord);
		auto Loaded = LoadedRecord->ToOutput(Key);
		ASSERT_TRUE(Loaded);
		auto Product = TexturePrivate::AssembleTexture2DSharedOutput(*Loaded, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
		ASSERT_TRUE(Product);
		for (size_t Index = 0; Index < Product->Mips.size(); ++Index)
		{
			const auto* Value = Loaded->FindValue(DerivedData::FValueId::FromName("Durin.Texture2D.Mip").MakeIndexed(uint32(Index)));
			ASSERT_NE(Value, nullptr);
			EXPECT_EQ(Product->Mips[Index].Pixels.data(), Value->GetData().data());
			EXPECT_EQ(Product->Mips[Index].Pixels.SharesStorageWith(*Stored), !bCompress);
		}
		Stored = FSharedByteBuffer{}; LoadedRecord = DerivedData::FCacheRecord{}; Loaded = DerivedData::FBuildOutput{};
		FByteBuffer Actual;
		ASSERT_TRUE(StorePlatformDataValue(*Product, Actual, Error)) << Error;
		EXPECT_EQ(Actual, Expected);
	}
}


TEST(FTexturePlatformSharedOutputTests, CubeAndVolumeRetainBlocksAndPreserveCookBytes)
{
	using namespace Durin;
	using namespace Durin::TexturePrivate;
	auto Encode = [](auto& Product) {
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
		Product.Serialize(Writer); EXPECT_FALSE(Writer.IsError()); return Bytes;
	};
	auto Cube = MakeCubePlatformData();
	const auto CubeBytes = Encode(Cube);
	auto CubeOutput = MakeTextureCubeSharedOutput(Cube, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(CubeOutput);
	const auto CubeAddress = Cube.Faces[4].Mips[1].Pixels.data();
	Cube = {};
	auto CubeProduct = AssembleTextureCubeSharedOutput(*CubeOutput, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(CubeProduct);
	EXPECT_EQ((*CubeProduct)->Faces[4].Mips[1].Pixels.data(), CubeAddress);
	CubeOutput = std::unexpected("released");
	EXPECT_EQ(Encode(**CubeProduct), CubeBytes);

	FVolumeTexturePlatformData Volume;
	Volume.PixelFormat = EPixelFormat::R8_UNORM;
	auto Group = FSharedByteBuffer::Take(FByteBuffer(19, std::byte{42}));
	Volume.Mips = {{.Voxels = Group.MakeView(0, 16), .Width = 4, .Height = 2, .Depth = 2, .RowPitch = 4, .DepthPitch = 8},
		{.Voxels = Group.MakeView(16, 2), .Width = 2, .Height = 1, .Depth = 1, .RowPitch = 2, .DepthPitch = 2},
		{.Voxels = Group.MakeView(18, 1), .Width = 1, .Height = 1, .Depth = 1, .RowPitch = 1, .DepthPitch = 1}};
	const auto VolumeBytes = Encode(Volume);
	auto VolumeOutput = MakeVolumeTextureSharedOutput(Volume, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(VolumeOutput);
	const auto VolumeAddress = Group.data();
	Volume = {}; Group = {};
	auto VolumeProduct = AssembleVolumeTextureSharedOutput(*VolumeOutput, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(VolumeProduct);
	VolumeOutput = std::unexpected("released");
	EXPECT_EQ((*VolumeProduct)->Mips[0].Voxels.data(), VolumeAddress);
	EXPECT_EQ((*VolumeProduct)->Mips[1].Voxels.data(), VolumeAddress + 16);
	EXPECT_EQ(Encode(**VolumeProduct), VolumeBytes);
}

TEST(FTexturePlatformSharedOutputTests, RejectsInconsistentBlocksAndBoundedLayouts)
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	using namespace Durin::TexturePrivate;
	auto Cube = MakeCubePlatformData();
	auto CubeOutput = MakeTextureCubeSharedOutput(Cube, ECookTargetPlatform::Win64, ECookTargetProfile::Game).value();
	FVolumeTexturePlatformData Volume{.Mips = {{.Voxels = FSharedByteBuffer::Take(FByteBuffer(1, std::byte{7})),
		.Width = 1, .Height = 1, .Depth = 1, .RowPitch = 1, .DepthPitch = 1}}, .PixelFormat = EPixelFormat::R8_UNORM};
	auto VolumeOutput = MakeVolumeTextureSharedOutput(Volume, ECookTargetPlatform::Win64, ECookTargetProfile::Game).value();
	for (const bool IsCube : {true, false})
	{
		const auto& Original = IsCube ? CubeOutput : VolumeOutput;
		auto Data = [&] { return CopyTextureOutput(Original); };
		auto Reject = [&](FMutableTextureOutput Invalid) {
			auto Output = std::move(Invalid).Build(); ASSERT_TRUE(Output);
			EXPECT_FALSE(ReadTexturePlatformOutputLayout(*Output, IsCube, ECookTargetPlatform::Win64, ECookTargetProfile::Game));
		};
		auto Invalid = Data(); Invalid.Values.pop_back(); Reject(std::move(Invalid));
		Invalid = Data(); Invalid.Values[0].first = FValueId::FromName("Invalid.Block"); Reject(std::move(Invalid));
		Invalid = Data(); ++Invalid.SchemaVersion; Reject(std::move(Invalid));
		Invalid = Data(); Invalid.Metadata = Invalid.Metadata.MakeView(0, 15); Reject(std::move(Invalid));
		for (const auto Offset : {12u, 16u, 20u, 24u, 28u})
		{
			Invalid = Data(); FByteBuffer Bytes(Invalid.Metadata.begin(), Invalid.Metadata.end());
			WriteU32(Bytes, Offset, UINT32_MAX); Invalid.Metadata = FSharedByteBuffer::Take(std::move(Bytes)); Reject(std::move(Invalid));
		}
		EXPECT_FALSE(ReadTexturePlatformOutputLayout(Original, IsCube, ECookTargetPlatform::Win64, ECookTargetProfile::EditorValidation));
	}
}


TEST(FTexturePlatformSessionTests, PreparedCubeBlocksMustMatchCapturedCanonicalIdentity)
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	FTextureCubeCanonicalBuildInput Prepared;
	Prepared.DecodedFaces.SourceChannelCounts.fill(4);
	std::array<Image::FImageView, 6> Views;
	for (size_t Face = 0; Face < Views.size(); ++Face)
	{
		Prepared.DecodedFaces.Faces[Face] = Image::FImage::TryCreate({.Width = 4, .Height = 4,
			.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64, std::byte{42})).value();
		Views[Face] = Prepared.DecodedFaces.Faces[Face].GetView();
	}
	FTextureSource Source;
	ASSERT_TRUE(Source.InitCube(Views, 4));
	auto Definition = TexturePrivate::MakeTextureCubeSessionDefinition({.CanonicalSourceIdentity = Source.GetIdentity(),
			.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game}).value();
	auto Valid = TexturePrivate::MakeTextureCubeInputResolver(Source, &Prepared);
	auto Identities = Valid->Describe(Definition.GetSources(), {}).value();
	auto Resolved = Valid->Resolve(Identities, {});
	ASSERT_TRUE(Resolved);
	EXPECT_TRUE(Resolved->front().Values[0].Data.SharesStorageWith(Views[0].GetBuffer()));
	Prepared.DecodedFaces.Faces[0] = Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64, std::byte{43})).value();
	auto Invalid = TexturePrivate::MakeTextureCubeInputResolver(Source, &Prepared);
	ASSERT_TRUE(Invalid->Describe(Definition.GetSources(), {}));
	auto Rejected = Invalid->Resolve(Identities, {});
	ASSERT_FALSE(Rejected);
	EXPECT_FALSE(Rejected.error().Description.empty());
	// The first resolver retained the original blocks independently of the caller.
	EXPECT_TRUE(Valid->Resolve(Identities, {}));
}

TEST(FTexturePlatformSessionTests, CancellationPreservesOperationStatus)
{
	using namespace Durin;
	const auto Error = TexturePrivate::ReportBuildFailure({ETextureBuildFailure::Canceled, ETextureBuildStage::Build, "cancelled"});
	EXPECT_EQ(Error.Code, ETextureBuildOperationFailure::Canceled);
	EXPECT_TRUE(Error.InputReason.empty());
}
