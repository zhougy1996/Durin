#include "AssetImportCore.h"
#include "AssetSystem.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DerivedDataObjectStore.h"
#include "EngineTestSupport.h"
#include "ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StandardAssetImportProviders.h"
#include "Source/SourceReferenceIndex.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "TerrainHeightmapSourceTranslation.h"
#include "Terrain/TerrainHeightmapBuildKey.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"

#include <gtest/gtest.h>

namespace
{
	class FTerrainHeightmapTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			std::string Error;
			ASSERT_TRUE(Durin::Asset::Import::RegisterStandardAssetImportProviders(Error)) << Error;
		}

		auto TearDown() -> void override
		{
			Durin::Asset::Import::UnregisterStandardAssetImportProviders();
		}
	};

	[[maybe_unused]] testing::Environment* GTerrainHeightmapTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTerrainHeightmapTestEnvironment);

	auto AppendBigU32(std::vector<Durin::uint8>& Bytes, Durin::uint32 Value) -> void
	{
		Bytes.push_back(static_cast<Durin::uint8>(Value >> 24));
		Bytes.push_back(static_cast<Durin::uint8>(Value >> 16));
		Bytes.push_back(static_cast<Durin::uint8>(Value >> 8));
		Bytes.push_back(static_cast<Durin::uint8>(Value));
	}

	auto Crc32(std::span<const Durin::uint8> Bytes) -> Durin::uint32
	{
		Durin::uint32 Crc = 0xffffffffu;
		for (Durin::uint8 Byte : Bytes)
		{
			Crc ^= Byte;
			for (Durin::uint32 Bit = 0; Bit < 8; ++Bit)
				Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
		}
		return ~Crc;
	}

	auto AppendChunk(
		std::vector<Durin::uint8>& Png,
		std::string_view Type,
		std::span<const Durin::uint8> Data) -> void
	{
		AppendBigU32(Png, static_cast<Durin::uint32>(Data.size()));
		const size_t CrcBegin = Png.size();
		Png.insert(Png.end(), Type.begin(), Type.end());
		Png.insert(Png.end(), Data.begin(), Data.end());
		AppendBigU32(Png, Crc32(std::span(Png).subspan(CrcBegin)));
	}

	auto MakeGrayscale16Png(
		Durin::uint32 Width,
		Durin::uint32 Height,
		std::span<const Durin::uint16> Samples,
		Durin::uint8 BitDepth = 16,
		Durin::uint8 ColorType = 0,
		Durin::uint8 Interlace = 0) -> std::vector<Durin::uint8>
	{
		std::vector<Durin::uint8> Raw;
		Raw.reserve(static_cast<size_t>(Height) * (1 + static_cast<size_t>(Width) * 2));
		for (Durin::uint32 Y = 0; Y < Height; ++Y)
		{
			Raw.push_back(0);
			for (Durin::uint32 X = 0; X < Width; ++X)
			{
				const Durin::uint16 Sample = Samples[static_cast<size_t>(Y) * Width + X];
				Raw.push_back(static_cast<Durin::uint8>(Sample >> 8));
				Raw.push_back(static_cast<Durin::uint8>(Sample));
			}
		}
		std::vector<Durin::uint8> Deflate{0x78, 0x01};
		size_t Offset = 0;
		while (Offset < Raw.size())
		{
			const Durin::uint16 Count = static_cast<Durin::uint16>(
				std::min<size_t>(65'535, Raw.size() - Offset));
			Deflate.push_back(Offset + Count == Raw.size() ? 1 : 0);
			Deflate.push_back(static_cast<Durin::uint8>(Count));
			Deflate.push_back(static_cast<Durin::uint8>(Count >> 8));
			const Durin::uint16 Inverse = static_cast<Durin::uint16>(~Count);
			Deflate.push_back(static_cast<Durin::uint8>(Inverse));
			Deflate.push_back(static_cast<Durin::uint8>(Inverse >> 8));
			Deflate.insert(Deflate.end(), Raw.begin() + Offset, Raw.begin() + Offset + Count);
			Offset += Count;
		}
		Durin::uint32 A = 1;
		Durin::uint32 B = 0;
		for (Durin::uint8 Byte : Raw)
		{
			A = (A + Byte) % 65'521;
			B = (B + A) % 65'521;
		}
		AppendBigU32(Deflate, (B << 16) | A);

		std::vector<Durin::uint8> Png{137, 80, 78, 71, 13, 10, 26, 10};
		std::vector<Durin::uint8> Ihdr;
		AppendBigU32(Ihdr, Width);
		AppendBigU32(Ihdr, Height);
		Ihdr.insert(Ihdr.end(), {BitDepth, ColorType, 0, 0, Interlace});
		AppendChunk(Png, "IHDR", Ihdr);
		AppendChunk(Png, "IDAT", Deflate);
		AppendChunk(Png, "IEND", {});
		return Png;
	}

	auto WritePng(
		const std::filesystem::path& Path,
		Durin::uint32 Width,
		Durin::uint32 Height,
		std::span<const Durin::uint16> Samples) -> void
	{
		std::filesystem::create_directories(Path.parent_path());
		const std::vector<Durin::uint8> Bytes =
			MakeGrayscale16Png(Width, Height, Samples);
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
			std::as_bytes(std::span(Bytes)), Path));
	}

	auto MakeRaw16(std::span<const Durin::uint16> Samples) -> std::vector<Durin::uint8>
	{
		std::vector<Durin::uint8> Bytes;
		Bytes.reserve(Samples.size() * sizeof(Durin::uint16));
		for (Durin::uint16 Sample : Samples)
		{
			Bytes.push_back(static_cast<Durin::uint8>(Sample));
			Bytes.push_back(static_cast<Durin::uint8>(Sample >> 8));
		}
		return Bytes;
	}

	auto WriteRaw16(
		const std::filesystem::path& Path,
		std::span<const Durin::uint16> Samples) -> void
	{
		std::filesystem::create_directories(Path.parent_path());
		const std::vector<Durin::uint8> Bytes = MakeRaw16(Samples);
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
			std::as_bytes(std::span(Bytes)), Path));
	}

	struct FScopedDdcRoot
	{
		explicit FScopedDdcRoot(const std::filesystem::path& Root)
			: Previous(Durin::FPaths::DerivedDataCacheDir())
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}
		~FScopedDdcRoot() { Durin::FPaths::SetDerivedDataCacheDirForTests(Previous); }
		std::string Previous;
	};

	auto InitializeHeightmapTests() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "TerrainHeightmap";
		static const bool Initialized = [&] {
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::PathUtilities::RegisterMountPointForTests(
				"/TerrainHeightmap/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
		return Root;
	}
}

TEST(FTerrainHeightmapPayloadTests, PreservesAsymmetricTopLeftRowMajorSamplesAndExactQueries)
{
	const std::array<Durin::uint16, 15> Samples{
		0, 1, 2, 3, 4,
		100, 200, 300, 400, 500,
		65'535, 900, 800, 700, 600};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Payload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(5, 3, Samples, Payload, Error)) << Error;
	ASSERT_NE(Payload, nullptr);
	Durin::uint16 Value = 0;
	EXPECT_TRUE(Payload->GetSample(0, 0, Value));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Payload->GetSample(4, 2, Value));
	EXPECT_EQ(Value, 600);
	Durin::uint16 Minimum = 0;
	Durin::uint16 Maximum = 0;
	EXPECT_TRUE(Payload->QueryMinMax(1, 1, 4, 3, Minimum, Maximum));
	EXPECT_EQ(Minimum, 200);
	EXPECT_EQ(Maximum, 900);
	EXPECT_EQ(Payload->Minimum, 0);
	EXPECT_EQ(Payload->Maximum, 65'535);
	ASSERT_EQ(Payload->Levels.size(), 1);
	EXPECT_EQ(Payload->Nodes.front(), (Durin::FTerrainHeightmapMinMaxNode{0, 65'535}));
}

TEST(FTerrainHeightmapPayloadTests, FreezesGolden257By129OrientationAndFacts)
{
	constexpr Durin::uint32 Width = 257;
	constexpr Durin::uint32 Height = 129;
	std::vector<Durin::uint16> Samples(static_cast<size_t>(Width) * Height, 7'000);
	Samples[0] = 101;
	Samples[Width - 1] = 202;
	Samples[static_cast<size_t>(Height - 1) * Width] = 303;
	Samples.back() = 404;
	Samples[static_cast<size_t>(37) * Width + 91] = 65'535;
	const std::filesystem::path Source = InitializeHeightmapTests() / "Sources/Golden257x129.png";
	WritePng(Source, Width, Height, Samples);
	Durin::Asset::FDecodedImage Decoded;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::DecodeImageFromFile(Source.generic_string(), Decoded, Error)) << Error;
	EXPECT_EQ(Decoded.Width, Width);
	EXPECT_EQ(Decoded.Height, Height);
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Payload;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(Width, Height, Samples, Payload, Error)) << Error;
	ASSERT_NE(Payload, nullptr);
	EXPECT_EQ(Payload->Minimum, 101);
	EXPECT_EQ(Payload->Maximum, 65'535);
	EXPECT_EQ(Payload->Samples.front(), 101);
	EXPECT_EQ(Payload->Samples[Width - 1], 202);
	EXPECT_EQ(Payload->Samples[static_cast<size_t>(Height - 1) * Width], 303);
	EXPECT_EQ(Payload->Samples.back(), 404);
	EXPECT_EQ(Payload->Samples[static_cast<size_t>(37) * Width + 91], 65'535);
	EXPECT_EQ(Payload->GetSampleBytes(), static_cast<Durin::uint64>(Width) * Height * sizeof(Durin::uint16));
}

TEST(FTerrainHeightmapPayloadTests, BuildsDeterministicOddEdgeHierarchyAndRejectsLimits)
{
	std::vector<Durin::uint16> Samples(65 * 67, 42);
	Samples[64] = 0;
	Samples.back() = 65'535;
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> First;
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Second;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(65, 67, Samples, First, Error)) << Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(65, 67, Samples, Second, Error)) << Error;
	ASSERT_EQ(First->Levels.size(), 2);
	EXPECT_EQ(First->Levels[0].Width, 2);
	EXPECT_EQ(First->Levels[0].Height, 2);
	EXPECT_EQ(First->Levels[1].Width, 1);
	EXPECT_EQ(First->Levels[1].Height, 1);
	EXPECT_EQ(First->Levels, Second->Levels);
	EXPECT_EQ(First->Nodes, Second->Nodes);
	EXPECT_FALSE(Durin::BuildTerrainHeightmapPayload(1, 2, Samples, Second, Error));
	EXPECT_EQ(First->GetSampleBytes(), Samples.size() * sizeof(Durin::uint16));
}

TEST(FTerrainHeightmapDerivedDataTests, KeyAndPayloadRoundTripAreStableAndCorruptionSafe)
{
	const std::array<Durin::uint16, 12> Samples{
		1, 2, 3, 4, 10, 20, 30, 40, 100, 200, 300, 400};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Payload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(4, 3, Samples, Payload, Error)) << Error;
	Durin::Asset::Build::FTerrainHeightmapBuildKeyInput KeyInput{
		.SourceContentHash = Durin::FXxHash128::HashBuffer(std::as_bytes(std::span(Samples))),
		.DecoderId = "DurinImage.Png16",
		.DecoderVersion = 1,
		.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Png16,
		.SourceProfileVersion = 1,
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game};
	std::string FirstKey;
	std::string SecondKey;
	FirstKey = Durin::Asset::Build::BuildTerrainHeightmapDerivedDataKey(KeyInput, Error);
	ASSERT_FALSE(FirstKey.empty()) << Error;
	SecondKey = Durin::Asset::Build::BuildTerrainHeightmapDerivedDataKey(KeyInput, Error);
	ASSERT_FALSE(SecondKey.empty()) << Error;
	EXPECT_EQ(FirstKey, SecondKey);
	EXPECT_EQ(FirstKey.size(), 32);
	KeyInput.DecoderId = "DurinTerrainRaw16";
	KeyInput.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Raw16;
	const std::string RawKey =
		Durin::Asset::Build::BuildTerrainHeightmapDerivedDataKey(KeyInput, Error);
	EXPECT_FALSE(RawKey.empty()) << Error;
	EXPECT_NE(RawKey, FirstKey);
	Durin::Asset::Build::FTerrainHeightmapBuildProduct PngProduct;
	Durin::Asset::Build::FTerrainHeightmapBuildProduct RawProduct;
	ASSERT_TRUE(Durin::Asset::Build::BuildTerrainHeightmap({
		.Samples = std::vector<Durin::uint16>(Samples.begin(), Samples.end()),
		.Width = 4,
		.Height = 3,
		.SourceContentHashLow = 1,
		.DecoderId = "DurinImage.Png16",
		.DecoderVersion = 1,
		.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Png16,
		.SourceProfileVersion = 1,
		.bPersistDerivedData = false}, PngProduct, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::BuildTerrainHeightmap({
		.Samples = std::vector<Durin::uint16>(Samples.begin(), Samples.end()),
		.Width = 4,
		.Height = 3,
		.SourceContentHashLow = 2,
		.DecoderId = "DurinTerrainRaw16",
		.DecoderVersion = 1,
		.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Raw16,
		.SourceProfileVersion = 1,
		.bPersistDerivedData = false}, RawProduct, Error)) << Error;
	EXPECT_NE(PngProduct.DerivedDataKey, RawProduct.DerivedDataKey);
	std::vector<Durin::uint8> PngPayloadBytes;
	std::vector<Durin::uint8> RawPayloadBytes;
	Durin::FCanonicalMemoryWriter PngPayloadWriter(
		PngPayloadBytes, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::FCanonicalMemoryWriter RawPayloadWriter(
		RawPayloadBytes, Durin::EArchivePurpose::DerivedDataPayload);
	const_cast<Durin::FTerrainHeightmapPayload&>(*PngProduct.Payload).Serialize(
		PngPayloadWriter, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	const_cast<Durin::FTerrainHeightmapPayload&>(*RawProduct.Payload).Serialize(
		RawPayloadWriter, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	EXPECT_EQ(PngPayloadBytes, RawPayloadBytes);
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	const_cast<Durin::FTerrainHeightmapPayload&>(*Payload).Serialize(
		Writer, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_FALSE(Writer.HasError());
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Bytes).ToString(),
		"56caac409ccb3ee8fbd5690626d85273");
	Durin::FTerrainHeightmapPayload Decoded;
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Decoded.Serialize(Reader, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_FALSE(Reader.HasError());
	EXPECT_EQ(Decoded.Samples, Payload->Samples);
	EXPECT_EQ(Decoded.Nodes, Payload->Nodes);
	Bytes.back() ^= 0x80;
	Durin::FCanonicalMemoryReader CorruptReader(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Decoded.Serialize(CorruptReader, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	EXPECT_TRUE(CorruptReader.HasError());
}

TEST(FTerrainHeightmapDecoderTests, AcceptsOnlyNonInterlacedGrayscale16Png)
{
	const std::array<Durin::uint16, 6> Samples{0, 1, 257, 4096, 32'768, 65'535};
	Durin::Asset::FDecodedGrayscale16Image Decoded;
	std::string Error;
	const std::vector<Durin::uint8> Valid = MakeGrayscale16Png(3, 2, Samples);
	ASSERT_TRUE(Durin::Asset::DecodeGrayscale16PngFromMemory(Valid, Decoded, Error)) << Error;
	EXPECT_EQ(Decoded.Width, 3);
	EXPECT_EQ(Decoded.Height, 2);
	EXPECT_EQ(Decoded.Samples, std::vector<Durin::uint16>(Samples.begin(), Samples.end()));
	EXPECT_FALSE(Durin::Asset::DecodeGrayscale16PngFromMemory(
		MakeGrayscale16Png(3, 2, Samples, 8), Decoded, Error));
	EXPECT_FALSE(Durin::Asset::DecodeGrayscale16PngFromMemory(
		MakeGrayscale16Png(3, 2, Samples, 16, 2), Decoded, Error));
	EXPECT_FALSE(Durin::Asset::DecodeGrayscale16PngFromMemory(
		MakeGrayscale16Png(3, 2, Samples, 16, 0, 1), Decoded, Error));
	EXPECT_FALSE(Durin::Asset::DecodeGrayscale16PngFromMemory(
		std::span(Valid).first(20), Decoded, Error));
}

TEST(FTerrainHeightmapDecoderTests, DecodesExactSquareU16LeAndRejectsMalformedRaw)
{
	// Gaea 2.3 Unity RAW oracle: U16LE, first sample at top-left, rows top-to-bottom.
	constexpr Durin::uint32 Dimension = 513;
	std::vector<Durin::uint16> Samples(
		static_cast<size_t>(Dimension) * Dimension, 7'000);
	Samples[0] = 0x1234;
	Samples[Dimension - 1] = 0x5678;
	Samples[static_cast<size_t>(Dimension - 1) * Dimension] = 0x9abc;
	Samples.back() = 0xdef0;
	Samples[static_cast<size_t>(127) * Dimension + 311] = 65'535;
	const std::vector<Durin::uint8> Bytes = MakeRaw16(Samples);
	ASSERT_EQ(Bytes.size(), 526'338);
	EXPECT_EQ(Bytes[0], 0x34);
	EXPECT_EQ(Bytes[1], 0x12);

	Durin::Asset::Import::FTerrainHeightmapDecodedSource Decoded;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Import::DecodeTerrainHeightmapSource(
		".RAW", Bytes, Decoded, Error)) << Error;
	EXPECT_EQ(Decoded.Width, Dimension);
	EXPECT_EQ(Decoded.Height, Dimension);
	EXPECT_EQ(Decoded.Samples, Samples);
	EXPECT_EQ(Decoded.DecoderId, "DurinTerrainRaw16");
	EXPECT_EQ(Decoded.DecoderVersion, 1);
	EXPECT_EQ(Decoded.SourceFormat, Durin::ETerrainHeightmapSourceFormat::Raw16);
	EXPECT_EQ(Decoded.SourceProfileVersion, 1);

	EXPECT_FALSE(Durin::Asset::Import::DecodeTerrainHeightmapSource(
		".raw", std::span<const Durin::uint8>{}, Decoded, Error));
	EXPECT_EQ(Error, "RAW16 terrain heightmap must contain at least four samples (8 bytes).");
	const std::array<Durin::uint8, 9> Odd{};
	EXPECT_FALSE(Durin::Asset::Import::DecodeTerrainHeightmapSource(
		".raw", Odd, Decoded, Error));
	EXPECT_EQ(Error, "RAW16 terrain heightmap byte count must be even.");
	const std::array<Durin::uint8, 16> NonSquare{};
	EXPECT_FALSE(Durin::Asset::Import::DecodeTerrainHeightmapSource(
		".raw", NonSquare, Decoded, Error));
	EXPECT_EQ(Error,
		"RAW16 terrain heightmap sample count must be an exact square within dimensions 2..16384.");
	EXPECT_FALSE(Durin::Asset::Import::DecodeTerrainHeightmapSource(
		".r16", Bytes, Decoded, Error));
	EXPECT_EQ(Error, "Terrain heightmap source extension must be .png or .raw.");
}

TEST(FTerrainHeightmapImportTests, RawImportReimportRelocationAndWarmDdcPreserveContract)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "RawDDC");
	const std::filesystem::path Source = Root / "Sources/Asymmetric.raw";
	const std::array<Durin::uint16, 9> Initial{
		1, 2, 3,
		100, 200, 300,
		65'535, 900, 4};
	WriteRaw16(Source, Initial);

	const Durin::FTerrainHeightmapImportResult Imported =
		Durin::Asset::Import::ImportTerrainHeightmapAsset(
			Source.generic_string(), "/TerrainHeightmap/RawAsymmetric");
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_NE(Imported.Asset, nullptr);
	EXPECT_EQ(Imported.Asset->GetPayload()->Samples,
		std::vector<Durin::uint16>(Initial.begin(), Initial.end()));
	EXPECT_EQ(Imported.Asset->GetSourceImportData().DecoderId, "DurinTerrainRaw16");
	EXPECT_EQ(Imported.Asset->GetSourceImportData().SourceFormat,
		Durin::ETerrainHeightmapSourceFormat::Raw16);
	EXPECT_TRUE(Imported.Asset->GetSourceFile().ends_with(".raw"));

	const Durin::uint64 InitialRevision = Imported.Asset->GetRevision();
	auto Plan = Durin::Asset::Import::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::Asset::Import::GetProviderRegistry(),
		Durin::Asset::Import::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Noop = Durin::Asset::Import::ExecuteSingleAssetImport(Plan.Plan);
	ASSERT_TRUE(Noop) << Noop.Message;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision);
	const std::array<Durin::uint16, 9> Changed{
		9, 8, 7,
		6, 5, 4,
		3, 2, 1};
	WriteRaw16(Source, Changed);
	Plan = Durin::Asset::Import::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::Asset::Import::GetProviderRegistry(),
		Durin::Asset::Import::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Updated = Durin::Asset::Import::ExecuteSingleAssetImport(Plan.Plan);
	ASSERT_TRUE(Updated) << Updated.Message;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision + 1);
	EXPECT_EQ(Imported.Asset->GetPayload()->Samples,
		std::vector<Durin::uint16>(Changed.begin(), Changed.end()));

	const std::filesystem::path Relocated = Root / "Relocated/Asymmetric.raw";
	WriteRaw16(Relocated, Changed);
	std::string Error;
	const Durin::uint64 BeforeRelocationRevision = Imported.Asset->GetRevision();
	ASSERT_TRUE(Durin::Asset::Import::ChangeTerrainHeightmapSourceReference(
		*Imported.Asset, "/TerrainHeightmap/Relocated/Asymmetric.raw", Error)) << Error;
	EXPECT_EQ(Imported.Asset->GetSourceFile(),
		"/TerrainHeightmap/Relocated/Asymmetric.raw");
	EXPECT_EQ(Imported.Asset->GetRevision(), BeforeRelocationRevision);

	const auto WrongDestination = Durin::Asset::Import::ImportTerrainHeightmapAsset(
		Source.generic_string(), "/TerrainHeightmap/RawWrongDestination",
		{.SourceDestination = "TerrainHeightmaps/Wrong.png"});
	EXPECT_FALSE(WrongDestination);

	const std::string Key = Imported.Asset->GetDerivedDataKey();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TerrainHeightmap/RawAsymmetric", Path));
	ASSERT_TRUE(Durin::Asset::SavePackage(Imported.Asset->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	std::error_code ErrorCode;
	std::filesystem::remove(Relocated, ErrorCode);
	Durin::DTerrainHeightmap* Reloaded = nullptr;
	const Durin::Asset::FAssetResult Loaded = Durin::Asset::LoadAsset(Path, Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	EXPECT_TRUE(Reloaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Reloaded->GetDerivedDataKey(), Key);
	EXPECT_EQ(Reloaded->GetPayload()->Samples,
		std::vector<Durin::uint16>(Changed.begin(), Changed.end()));
}

TEST(FTerrainHeightmapImportTests, ExplicitImportReimportAndRollbackPreserveTheAssetContract)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "DDC");
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Import::RegisterStandardAssetImportProviders(Error)) << Error;
	const std::filesystem::path Source = Root / "Sources/Asymmetric.png";
	const std::array<Durin::uint16, 6> Initial{0, 100, 200, 300, 400, 65'535};
	WritePng(Source, 3, 2, Initial);
	const Durin::FTerrainHeightmapImportResult Imported =
		Durin::Asset::Import::ImportTerrainHeightmapAsset(
			Source.generic_string(), "/TerrainHeightmap/Asymmetric");
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_NE(Imported.Asset, nullptr);
	EXPECT_EQ(Imported.Asset->GetRevision(), 1);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());
	EXPECT_EQ(Imported.Asset->GetMinimum(), 0);
	EXPECT_EQ(Imported.Asset->GetMaximum(), 65'535);
	Durin::Editor::FSourceReferenceIndex SourceIndex;
	SourceIndex.Refresh();
	const auto References = SourceIndex.FindReferences(Imported.Asset->GetSourceFile());
	ASSERT_EQ(References.size(), 1);
	EXPECT_EQ(References.front().AssetPath.ToString(), "/TerrainHeightmap/Asymmetric");
	std::string DuplicateError;
	auto* Duplicate = Durin::Cast<Durin::DTerrainHeightmap>(Durin::DuplicateObjectGraph(
		Imported.Asset, nullptr, "AsymmetricDuplicate", &DuplicateError));
	ASSERT_NE(Duplicate, nullptr) << DuplicateError;
	ASSERT_NE(Duplicate->GetPayload(), nullptr);
	EXPECT_EQ(Duplicate->GetPayload()->Samples, Imported.Asset->GetPayload()->Samples);
	EXPECT_EQ(Duplicate->GetRevision(), Imported.Asset->GetRevision());
	const auto RetainedSnapshot = Duplicate->GetPayload();
	Duplicate->BeginDestroy();
	EXPECT_EQ(RetainedSnapshot->Samples, std::vector<Durin::uint16>(Initial.begin(), Initial.end()));

	// Ordinary PNG import remains the Texture2D path even for a 16-bit grayscale source.
	const Durin::FTexture2DImportResult TextureImport = Durin::Asset::Import::ImportTexture2DAsset(
		Source.generic_string(), "/TerrainHeightmap/DefaultPngTexture");
	ASSERT_TRUE(TextureImport) << TextureImport.Message;
	ASSERT_NE(TextureImport.Asset, nullptr);

	auto Plan = Durin::Asset::Import::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::Asset::Import::GetProviderRegistry(),
		Durin::Asset::Import::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const Durin::uint64 InitialRevision = Imported.Asset->GetRevision();
	const auto Noop = Durin::Asset::Import::ExecuteSingleAssetImport(Plan.Plan);
	ASSERT_TRUE(Noop) << Noop.Message;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());

	const std::array<Durin::uint16, 6> Changed{1, 2, 3, 4, 5, 6};
	WritePng(Source, 3, 2, Changed);
	Plan = Durin::Asset::Import::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::Asset::Import::GetProviderRegistry(),
		Durin::Asset::Import::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Updated = Durin::Asset::Import::ExecuteSingleAssetImport(Plan.Plan);
	ASSERT_TRUE(Updated) << Updated.Message;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision + 1);
	EXPECT_EQ(Imported.Asset->GetMinimum(), 1);
	EXPECT_EQ(Imported.Asset->GetMaximum(), 6);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());

	const Durin::uint64 BeforeFailureRevision = Imported.Asset->GetRevision();
	const auto BeforeFailurePayload = Imported.Asset->GetPayload();
	const std::array<Durin::uint16, 6> FailedChange{9, 9, 9, 9, 9, 9};
	WritePng(Source, 3, 2, FailedChange);
	Plan = Durin::Asset::Import::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::Asset::Import::GetProviderRegistry(),
		Durin::Asset::Import::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Failed = Durin::Asset::Import::ExecuteSingleAssetImport(
		Plan.Plan, {.SaveOptions = {.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
			return Phase == Durin::Asset::EAssetBundleSavePhase::StagePackage;
		}}});
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Imported.Asset->GetRevision(), BeforeFailureRevision);
	EXPECT_EQ(Imported.Asset->GetPayload()->Samples, BeforeFailurePayload->Samples);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());
}

TEST(FTerrainHeightmapImportTests, AuthoredReloadUsesWarmDdcWithoutReopeningSource)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "WarmReloadDDC");
	const std::filesystem::path Source = Root / "Sources/WarmReload.png";
	const std::array<Durin::uint16, 6> Samples{5, 4, 3, 2, 1, 0};
	WritePng(Source, 3, 2, Samples);
	const Durin::FTerrainHeightmapImportResult Imported =
		Durin::Asset::Import::ImportTerrainHeightmapAsset(
			Source.generic_string(), "/TerrainHeightmap/WarmReload");
	ASSERT_TRUE(Imported) << Imported.Message;
	const std::string Key = Imported.Asset->GetDerivedDataKey();
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TerrainHeightmap/WarmReload", Path));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	std::error_code ErrorCode;
	std::filesystem::remove(Source, ErrorCode);
	Durin::DTerrainHeightmap* Reloaded = nullptr;
	const Durin::Asset::FAssetResult Loaded = Durin::Asset::LoadAsset(Path, Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_TRUE(Reloaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Reloaded->GetDerivedDataKey(), Key);
	EXPECT_EQ(Reloaded->GetPayload()->Samples,
		std::vector<Durin::uint16>(Samples.begin(), Samples.end()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	Durin::Asset::FDerivedDataObjectStore Store(
		"TerrainHeightmap/Objects", Durin::MaximumTerrainHeightmapPayloadBytes);
	std::filesystem::path CachePath;
	std::string Error;
	ASSERT_TRUE(Store.GetObjectPath(Key, CachePath, &Error)) << Error;
	const std::array<Durin::uint8, 4> Corrupt{1, 2, 3, 4};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(Corrupt)), CachePath));
	Reloaded = nullptr;
	const Durin::Asset::FAssetResult Failed = Durin::Asset::LoadAsset(Path, Reloaded);
	EXPECT_FALSE(Failed);
}
