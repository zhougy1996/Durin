#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "EngineTestSupport.h"
#include "Texture/TextureFactoryTestSupport.h"
#include "Image/ImageDecoder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Source/SourceReferenceIndex.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapFactoryTestSupport.h"
#include "Terrain/TerrainHeightmapPostLoad.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "EditorReimportHandler.h"
#include "Terrain/TerrainHeightmapBuildKey.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Texture/Texture2D.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Threading/Task.h"
#include "Modules/ModuleManager.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace
{
	struct FReimportResult
	{
		bool bSucceeded = false;
		bool bPersistenceFailed = false;
		std::string Diagnostic;
	};

	class FTerrainHeightmapTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
			Durin::FModuleManager::Get().LoadModuleChecked("TerrainBuild");
			Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
		}
	};

	[[maybe_unused]] testing::Environment* GTerrainHeightmapTestEnvironment =
		testing::AddGlobalTestEnvironment(new FTerrainHeightmapTestEnvironment);

	auto GetTerrainCachePath(std::string_view Key) -> std::filesystem::path
	{
		return std::filesystem::path(Durin::FPaths::DerivedDataCacheDir())
			/ "TerrainHeightmap" / "Objects" / std::string(Key.substr(0, 2))
			/ (std::string(Key) + ".bin");
	}

	auto AppendBigU32(Durin::FByteArray& Bytes, uint32 Value) -> void
	{
		Bytes.push_back(static_cast<std::byte>(Value >> 24));
		Bytes.push_back(static_cast<std::byte>(Value >> 16));
		Bytes.push_back(static_cast<std::byte>(Value >> 8));
		Bytes.push_back(static_cast<std::byte>(Value));
	}

	auto Crc32(std::span<const std::byte> Bytes) -> uint32
	{
		uint32 Crc = 0xffffffffu;
		for (std::byte Byte : Bytes)
		{
			Crc ^= std::to_integer<uint8>(Byte);
			for (uint32 Bit = 0; Bit < 8; ++Bit)
				Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
		}
		return ~Crc;
	}

	auto AppendChunk(
		Durin::FByteArray& Png,
		std::string_view Type,
		std::span<const std::byte> Data) -> void
	{
		AppendBigU32(Png, static_cast<uint32>(Data.size()));
		const size_t CrcBegin = Png.size();
		const std::span<const std::byte> TypeBytes =
			std::as_bytes(std::span{Type.data(), Type.size()});
		Png.insert(Png.end(), TypeBytes.begin(), TypeBytes.end());
		Png.insert(Png.end(), Data.begin(), Data.end());
		AppendBigU32(Png, Crc32(std::span(Png).subspan(CrcBegin)));
	}

	auto MakeGrayscale16Png(
		uint32 Width,
		uint32 Height,
		std::span<const uint16> Samples,
		uint8 BitDepth = 16,
		uint8 ColorType = 0,
		uint8 Interlace = 0) -> Durin::FByteArray
	{
		Durin::FByteArray Raw;
		Raw.reserve(static_cast<size_t>(Height) * (1 + static_cast<size_t>(Width) * 2));
		for (uint32 Y = 0; Y < Height; ++Y)
		{
			Raw.push_back(std::byte{0});
			for (uint32 X = 0; X < Width; ++X)
			{
				const uint16 Sample = Samples[static_cast<size_t>(Y) * Width + X];
				Raw.push_back(static_cast<std::byte>(Sample >> 8));
				Raw.push_back(static_cast<std::byte>(Sample));
			}
		}
		Durin::FByteArray Deflate{std::byte{0x78}, std::byte{0x01}};
		size_t Offset = 0;
		while (Offset < Raw.size())
		{
			const uint16 Count = static_cast<uint16>(
				std::min<size_t>(65'535, Raw.size() - Offset));
			Deflate.push_back(Offset + Count == Raw.size()
				? std::byte{1} : std::byte{0});
			Deflate.push_back(static_cast<std::byte>(Count));
			Deflate.push_back(static_cast<std::byte>(Count >> 8));
			const uint16 Inverse = static_cast<uint16>(~Count);
			Deflate.push_back(static_cast<std::byte>(Inverse));
			Deflate.push_back(static_cast<std::byte>(Inverse >> 8));
			Deflate.insert(Deflate.end(), Raw.begin() + Offset, Raw.begin() + Offset + Count);
			Offset += Count;
		}
		uint32 A = 1;
		uint32 B = 0;
		for (std::byte Byte : Raw)
		{
			A = (A + std::to_integer<uint8>(Byte)) % 65'521;
			B = (B + A) % 65'521;
		}
		AppendBigU32(Deflate, (B << 16) | A);

		Durin::FByteArray Png{
			std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
			std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
		Durin::FByteArray Ihdr;
		AppendBigU32(Ihdr, Width);
		AppendBigU32(Ihdr, Height);
		Ihdr.insert(Ihdr.end(), {static_cast<std::byte>(BitDepth),
			static_cast<std::byte>(ColorType), std::byte{0}, std::byte{0},
			static_cast<std::byte>(Interlace)});
		AppendChunk(Png, "IHDR", Ihdr);
		AppendChunk(Png, "IDAT", Deflate);
		AppendChunk(Png, "IEND", {});
		return Png;
	}

	auto WritePng(
		const std::filesystem::path& Path,
		uint32 Width,
		uint32 Height,
		std::span<const uint16> Samples) -> void
	{
		std::filesystem::create_directories(Path.parent_path());
		const Durin::FByteArray Bytes =
			MakeGrayscale16Png(Width, Height, Samples);
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
			std::as_bytes(std::span(Bytes)), Path));
	}

	auto MakeRaw16(std::span<const uint16> Samples) -> Durin::FByteArray
	{
		Durin::FByteArray Bytes;
		Bytes.reserve(Samples.size() * sizeof(uint16));
		for (uint16 Sample : Samples)
		{
			Bytes.push_back(static_cast<std::byte>(Sample));
			Bytes.push_back(static_cast<std::byte>(Sample >> 8));
		}
		return Bytes;
	}

	auto WriteRaw16(
		const std::filesystem::path& Path,
		std::span<const uint16> Samples) -> void
	{
		std::filesystem::create_directories(Path.parent_path());
		const Durin::FByteArray Bytes = MakeRaw16(Samples);
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
			Durin::Testing::RegisterMountPointForTests(
				"/TerrainHeightmap/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
		return Root;
	}

	auto ReimportHeightmap(Durin::DTerrainHeightmap& Heightmap,
		Durin::FAssetBundleSaveOptions SaveOptions = {})
		-> FReimportResult
	{
		std::string Error;
		if (Durin::AssetForge::Builtins::ReimportTerrainHeightmap(
			Heightmap, Error, SaveOptions))
			return {.bSucceeded = true};
		if (Heightmap.GetPackage() && Heightmap.GetPackage()->IsDirty())
			return {.bSucceeded = true, .bPersistenceFailed = true,
				.Diagnostic = std::move(Error)};
		return {.Diagnostic = std::move(Error)};
	}
}

TEST(FTerrainHeightmapPayloadTests, PreservesAsymmetricTopLeftRowMajorSamplesAndExactQueries)
{
	const std::array<uint16, 15> Samples{
		0, 1, 2, 3, 4,
		100, 200, 300, 400, 500,
		65'535, 900, 800, 700, 600};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Payload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(5, 3, Samples, Payload, Error)) << Error;
	ASSERT_NE(Payload, nullptr);
	uint16 Value = 0;
	EXPECT_TRUE(Payload->GetSample(0, 0, Value));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Payload->GetSample(4, 2, Value));
	EXPECT_EQ(Value, 600);
	uint16 Minimum = 0;
	uint16 Maximum = 0;
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
	constexpr uint32 Width = 257;
	constexpr uint32 Height = 129;
	std::vector<uint16> Samples(static_cast<size_t>(Width) * Height, 7'000);
	Samples[0] = 101;
	Samples[Width - 1] = 202;
	Samples[static_cast<size_t>(Height - 1) * Width] = 303;
	Samples.back() = 404;
	Samples[static_cast<size_t>(37) * Width + 91] = 65'535;
	const std::filesystem::path Source = InitializeHeightmapTests() / "Sources/Golden257x129.png";
	WritePng(Source, Width, Height, Samples);
	Durin::Image::FDecodedImage Decoded;
	std::string Error;
	ASSERT_TRUE(Durin::Image::DecodeImageFromFile(Source.generic_string(), Decoded, Error)) << Error;
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
	EXPECT_EQ(Payload->GetSampleBytes(), static_cast<uint64>(Width) * Height * sizeof(uint16));
}

TEST(FTerrainHeightmapPayloadTests, BuildsDeterministicOddEdgeHierarchyAndRejectsLimits)
{
	std::vector<uint16> Samples(65 * 67, 42);
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
	EXPECT_EQ(First->GetSampleBytes(), Samples.size() * sizeof(uint16));
}

TEST(FTerrainHeightmapPayloadTests, SeparatesTrustedLayoutFromExactCanonicalValidation)
{
	const std::array<uint16, 12> Samples{
		1, 2, 3, 4, 10, 20, 30, 40, 100, 200, 300, 400};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Canonical;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(4, 3, Samples, Canonical, Error)) << Error;
	ASSERT_TRUE(Canonical->HasValidLayout());
	ASSERT_TRUE(Canonical->IsValid());

	Durin::FTerrainHeightmapPayload ChangedSample = *Canonical;
	ChangedSample.Samples[0] = 500;
	EXPECT_TRUE(ChangedSample.HasValidLayout());
	EXPECT_FALSE(ChangedSample.IsValid());

	Durin::FTerrainHeightmapPayload BrokenLayout = *Canonical;
	BrokenLayout.Levels.front().NodeOffset = 1;
	EXPECT_FALSE(BrokenLayout.HasValidLayout());
	EXPECT_FALSE(BrokenLayout.IsValid());
}

TEST(FTerrainHeightmapDerivedDataTests, KeyAndPayloadRoundTripAreStableAndCorruptionSafe)
{
	const std::array<uint16, 12> Samples{
		1, 2, 3, 4, 10, 20, 30, 40, 100, 200, 300, 400};
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Payload;
	std::string Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapPayload(4, 3, Samples, Payload, Error)) << Error;
	Durin::FTerrainHeightmapBuildKeyInput KeyInput{
		.SourceContentHash = Durin::FXxHash128::HashBuffer(std::as_bytes(std::span(Samples))),
		.DecoderId = "DurinImage.Png16",
		.DecoderVersion = 1,
		.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Png16,
		.SourceProfileVersion = 1,
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game};
	std::string FirstKey;
	std::string SecondKey;
	FirstKey = Durin::BuildTerrainHeightmapDerivedDataKey(KeyInput, Error);
	ASSERT_FALSE(FirstKey.empty()) << Error;
	SecondKey = Durin::BuildTerrainHeightmapDerivedDataKey(KeyInput, Error);
	ASSERT_FALSE(SecondKey.empty()) << Error;
	EXPECT_EQ(FirstKey, SecondKey);
	EXPECT_EQ(FirstKey.size(), 32);
	KeyInput.DecoderId = "DurinTerrainRaw16";
	KeyInput.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Raw16;
	const std::string RawKey =
		Durin::BuildTerrainHeightmapDerivedDataKey(KeyInput, Error);
	EXPECT_FALSE(RawKey.empty()) << Error;
	EXPECT_NE(RawKey, FirstKey);
	Durin::FTerrainHeightmapBuildProduct PngProduct;
	Durin::FTerrainHeightmapBuildProduct RawProduct;
	ASSERT_TRUE(Durin::BuildTerrainHeightmap({
		.Samples = std::vector<uint16>(Samples.begin(), Samples.end()),
		.Width = 4,
		.Height = 3,
		.SourceContentHashLow = 1,
		.DecoderId = "DurinImage.Png16",
		.DecoderVersion = 1,
		.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Png16,
		.SourceProfileVersion = 1,
		.bPersistDerivedData = false}, PngProduct, Error)) << Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmap({
		.Samples = std::vector<uint16>(Samples.begin(), Samples.end()),
		.Width = 4,
		.Height = 3,
		.SourceContentHashLow = 2,
		.DecoderId = "DurinTerrainRaw16",
		.DecoderVersion = 1,
		.SourceFormat = Durin::ETerrainHeightmapSourceFormat::Raw16,
		.SourceProfileVersion = 1,
		.bPersistDerivedData = false}, RawProduct, Error)) << Error;
	EXPECT_EQ(PngProduct.DerivedDataKey, RawProduct.DerivedDataKey);
	Durin::FByteArray PngPayloadBytes;
	Durin::FByteArray RawPayloadBytes;
	Durin::FCanonicalMemoryWriter PngPayloadWriter(
		PngPayloadBytes, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::FCanonicalMemoryWriter RawPayloadWriter(
		RawPayloadBytes, Durin::EArchivePurpose::DerivedDataPayload);
	const_cast<Durin::FTerrainHeightmapPayload&>(*PngProduct.Payload).Serialize(
		PngPayloadWriter, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	const_cast<Durin::FTerrainHeightmapPayload&>(*RawProduct.Payload).Serialize(
		RawPayloadWriter, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	EXPECT_EQ(PngPayloadBytes, RawPayloadBytes);
	Durin::FByteArray Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	const_cast<Durin::FTerrainHeightmapPayload&>(*Payload).Serialize(
		Writer, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_FALSE(Writer.HasError());
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Bytes).ToString(),
		"bc8f85141d6502d40546657fbb6b9c15");
	Durin::FTerrainHeightmapPayload Decoded;
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Decoded.Serialize(Reader, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_FALSE(Reader.HasError());
	EXPECT_EQ(Decoded.Samples, Payload->Samples);
	EXPECT_EQ(Decoded.Nodes, Payload->Nodes);
	Bytes.back() ^= std::byte{0x80};
	Durin::FCanonicalMemoryReader CorruptReader(
		Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Decoded.Serialize(CorruptReader, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	EXPECT_TRUE(CorruptReader.HasError());
}

TEST(FTerrainHeightmapDecoderTests, AcceptsOnlyNonInterlacedGrayscale16Png)
{
	const std::array<uint16, 6> Samples{0, 1, 257, 4096, 32'768, 65'535};
	Durin::Image::FDecodedGrayscale16Image Decoded;
	std::string Error;
	const Durin::FByteArray Valid = MakeGrayscale16Png(3, 2, Samples);
	ASSERT_TRUE(Durin::Image::DecodeGrayscale16PngFromMemory(Valid, Decoded, Error)) << Error;
	EXPECT_EQ(Decoded.Width, 3);
	EXPECT_EQ(Decoded.Height, 2);
	EXPECT_EQ(Decoded.Samples, std::vector<uint16>(Samples.begin(), Samples.end()));
	EXPECT_FALSE(Durin::Image::DecodeGrayscale16PngFromMemory(
		MakeGrayscale16Png(3, 2, Samples, 8), Decoded, Error));
	EXPECT_FALSE(Durin::Image::DecodeGrayscale16PngFromMemory(
		MakeGrayscale16Png(3, 2, Samples, 16, 2), Decoded, Error));
	EXPECT_FALSE(Durin::Image::DecodeGrayscale16PngFromMemory(
		MakeGrayscale16Png(3, 2, Samples, 16, 0, 1), Decoded, Error));
	EXPECT_FALSE(Durin::Image::DecodeGrayscale16PngFromMemory(
		std::span(Valid).first(20), Decoded, Error));
}

TEST(FTerrainHeightmapDecoderTests, DecodesExactSquareU16LeAndRejectsMalformedRaw)
{
	// Gaea 2.3 Unity RAW oracle: U16LE, first sample at top-left, rows top-to-bottom.
	constexpr uint32 Dimension = 513;
	std::vector<uint16> Samples(
		static_cast<size_t>(Dimension) * Dimension, 7'000);
	Samples[0] = 0x1234;
	Samples[Dimension - 1] = 0x5678;
	Samples[static_cast<size_t>(Dimension - 1) * Dimension] = 0x9abc;
	Samples.back() = 0xdef0;
	Samples[static_cast<size_t>(127) * Dimension + 311] = 65'535;
	const Durin::FByteArray Bytes = MakeRaw16(Samples);
	ASSERT_EQ(Bytes.size(), 526'338);
	EXPECT_EQ(Bytes[0], std::byte{0x34});
	EXPECT_EQ(Bytes[1], std::byte{0x12});

	Durin::AssetForge::Builtins::FTerrainHeightmapSourceData Decoded;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::TranslateTerrainHeightmapSource(
		".RAW", Bytes, Decoded, Error)) << Error;
	EXPECT_EQ(Decoded.Width, Dimension);
	EXPECT_EQ(Decoded.Height, Dimension);
	EXPECT_EQ(Decoded.Samples, Samples);
	EXPECT_EQ(Decoded.DecoderId, "DurinTerrainRaw16");
	EXPECT_EQ(Decoded.DecoderVersion, 1);
	EXPECT_EQ(Decoded.SourceFormat, Durin::ETerrainHeightmapSourceFormat::Raw16);
	EXPECT_EQ(Decoded.SourceProfileVersion, 1);

	EXPECT_FALSE(Durin::AssetForge::Builtins::TranslateTerrainHeightmapSource(
		".raw", std::span<const std::byte>{}, Decoded, Error));
	EXPECT_EQ(Error, "RAW16 terrain heightmap must contain at least four samples (8 bytes).");
	const std::array<uint8, 9> Odd{};
	EXPECT_FALSE(Durin::AssetForge::Builtins::TranslateTerrainHeightmapSource(
		".raw", std::as_bytes(std::span{Odd}), Decoded, Error));
	EXPECT_EQ(Error, "RAW16 terrain heightmap byte count must be even.");
	const std::array<uint8, 16> NonSquare{};
	EXPECT_FALSE(Durin::AssetForge::Builtins::TranslateTerrainHeightmapSource(
		".raw", std::as_bytes(std::span{NonSquare}), Decoded, Error));
	EXPECT_EQ(Error,
		"RAW16 terrain heightmap sample count must be an exact square within dimensions 2..16384.");
	EXPECT_FALSE(Durin::AssetForge::Builtins::TranslateTerrainHeightmapSource(
		".r16", Bytes, Decoded, Error));
	EXPECT_EQ(Error, "Terrain heightmap source extension must be .png or .raw.");
}

TEST(FTerrainHeightmapImportTests, RawImportReimportRelocationAndWarmDdcPreserveContract)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "RawDDC");
	const std::filesystem::path Source = Root / "Sources/Asymmetric.raw";
	const std::array<uint16, 9> Initial{
		1, 2, 3,
		100, 200, 300,
		65'535, 900, 4};
	WriteRaw16(Source, Initial);

	const Durin::Testing::TFactoryImportResult<Durin::DTerrainHeightmap> Imported =
		Durin::AssetForge::Builtins::ImportTerrainHeightmapForTest(
			Source.generic_string(), "/TerrainHeightmap/RawAsymmetric");
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_NE(Imported.Asset, nullptr);
	EXPECT_EQ(Imported.Asset->GetPayload()->Samples,
		std::vector<uint16>(Initial.begin(), Initial.end()));
	const auto* ImportData = Imported.Asset->GetAssetImportData();
	ASSERT_NE(ImportData, nullptr);
	const Durin::FSourceFile* ImportedSource =
		ImportData->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_TRUE(ImportedSource->Hint.ends_with(".raw"));

	const uint64 InitialRevision = Imported.Asset->GetRevision();
	Durin::FReimportResult Noop;
	Durin::FReimportManager::Reimport(*Imported.Asset, {},
		[&](Durin::FReimportResult Result) { Noop = std::move(Result); });
	ASSERT_TRUE(Noop) << Noop.Message;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision);
	const std::array<uint16, 9> Changed{
		9, 8, 7,
		6, 5, 4,
		3, 2, 1};
	WriteRaw16(Source, Changed);
	const auto Updated = ReimportHeightmap(*Imported.Asset);
	ASSERT_TRUE(Updated.bSucceeded) << Updated.Diagnostic;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision + 1);
	EXPECT_EQ(Imported.Asset->GetPayload()->Samples,
		std::vector<uint16>(Changed.begin(), Changed.end()));

	const std::string Key = Imported.Asset->GetDerivedDataKey();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TerrainHeightmap/RawAsymmetric", Path));
	ASSERT_TRUE(Durin::SavePackage(Imported.Asset->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	std::error_code ErrorCode;
	std::filesystem::remove(Source, ErrorCode);
	Durin::DTerrainHeightmap* Reloaded = nullptr;
	const Durin::FAssetResult Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	EXPECT_TRUE(Reloaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Reloaded->GetDerivedDataKey(), Key);
	EXPECT_EQ(Reloaded->GetPayload()->Samples,
		std::vector<uint16>(Changed.begin(), Changed.end()));
}

TEST(FTerrainHeightmapImportTests, ExplicitImportReimportAndFailedSavePreservePublishedState)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "DDC");
	std::string Error;
	const std::filesystem::path Source = Root / "Sources/Asymmetric.png";
	const std::array<uint16, 6> Initial{0, 100, 200, 300, 400, 65'535};
	WritePng(Source, 3, 2, Initial);
	const Durin::Testing::TFactoryImportResult<Durin::DTerrainHeightmap> Imported =
		Durin::AssetForge::Builtins::ImportTerrainHeightmapForTest(
			Source.generic_string(), "/TerrainHeightmap/Asymmetric");
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_NE(Imported.Asset, nullptr);
	const auto* ImportData = Imported.Asset->GetAssetImportData();
	ASSERT_NE(ImportData, nullptr);
	EXPECT_EQ(Imported.Asset->GetRevision(), 1);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());
	EXPECT_EQ(Imported.Asset->GetMinimum(), 0);
	EXPECT_EQ(Imported.Asset->GetMaximum(), 65'535);
	Durin::Editor::FSourceReferenceIndex SourceIndex;
	SourceIndex.Refresh();
	const Durin::FSourceFile* ImportedSource =
		ImportData->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	const auto References = SourceIndex.FindReferences(
		ImportedSource->Hint);
	ASSERT_EQ(References.size(), 1);
	EXPECT_EQ(References.front().AssetPath.ToString(), "/TerrainHeightmap/Asymmetric");
	auto* Duplicate = Durin::Cast<Durin::DTerrainHeightmap>(Durin::DuplicateObject(
		Imported.Asset, nullptr, "AsymmetricDuplicate"));
	ASSERT_NE(Duplicate, nullptr);
	ASSERT_NE(Duplicate->GetPayload(), nullptr);
	EXPECT_EQ(Duplicate->GetPayload()->Samples, Imported.Asset->GetPayload()->Samples);
	EXPECT_EQ(Duplicate->GetRevision(), Imported.Asset->GetRevision());
	const auto RetainedSnapshot = Duplicate->GetPayload();
	Duplicate->BeginDestroy();
	EXPECT_EQ(RetainedSnapshot->Samples, std::vector<uint16>(Initial.begin(), Initial.end()));

	// Ordinary PNG import remains the Texture2D path even for a 16-bit grayscale source.
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> TextureImport = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TerrainHeightmap/DefaultPngTexture");
	ASSERT_TRUE(TextureImport) << TextureImport.Message;
	ASSERT_NE(TextureImport.Asset, nullptr);

	const uint64 InitialRevision = Imported.Asset->GetRevision();
	const auto Noop = ReimportHeightmap(*Imported.Asset);
	ASSERT_TRUE(Noop.bSucceeded) << Noop.Diagnostic;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());

	const std::array<uint16, 6> Changed{1, 2, 3, 4, 5, 6};
	WritePng(Source, 3, 2, Changed);
	const auto Updated = ReimportHeightmap(*Imported.Asset);
	ASSERT_TRUE(Updated.bSucceeded) << Updated.Diagnostic;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision + 1);
	EXPECT_EQ(Imported.Asset->GetMinimum(), 1);
	EXPECT_EQ(Imported.Asset->GetMaximum(), 6);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());

	const uint64 BeforeFailureRevision = Imported.Asset->GetRevision();
	const std::array<uint16, 6> FailedChange{9, 9, 9, 9, 9, 9};
	WritePng(Source, 3, 2, FailedChange);
	const auto Failed = ReimportHeightmap(*Imported.Asset,
		{.ShouldFail = [](Durin::EAssetBundleSavePhase Phase, size_t) {
			return Phase == Durin::EAssetBundleSavePhase::StagePackage;
		}});
	EXPECT_TRUE(Failed.bSucceeded);
	EXPECT_TRUE(Failed.bPersistenceFailed);
	EXPECT_FALSE(Failed.Diagnostic.empty());
	EXPECT_EQ(Imported.Asset->GetRevision(), BeforeFailureRevision + 1);
	EXPECT_EQ(Imported.Asset->GetPayload()->Samples,
		std::vector<uint16>(FailedChange.begin(), FailedChange.end()));
	EXPECT_TRUE(Imported.Asset->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::SavePackage(Imported.Asset->GetPackage()));
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());
}

TEST(FTerrainHeightmapImportTests, AuthoredReloadUsesWarmDdcWithoutReopeningSource)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "WarmReloadDDC");
	const std::filesystem::path Source = Root / "Sources/WarmReload.png";
	const std::array<uint16, 6> Samples{5, 4, 3, 2, 1, 0};
	WritePng(Source, 3, 2, Samples);
	const Durin::Testing::TFactoryImportResult<Durin::DTerrainHeightmap> Imported =
		Durin::AssetForge::Builtins::ImportTerrainHeightmapForTest(
			Source.generic_string(), "/TerrainHeightmap/WarmReload");
	ASSERT_TRUE(Imported) << Imported.Message;
	const std::string Key = Imported.Asset->GetDerivedDataKey();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TerrainHeightmap/WarmReload", Path));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	std::error_code ErrorCode;
	std::filesystem::remove(Source, ErrorCode);
	Durin::DTerrainHeightmap* Reloaded = nullptr;
	const Durin::FAssetResult Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_TRUE(Reloaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Reloaded->GetDerivedDataKey(), Key);
	EXPECT_EQ(Reloaded->GetPayload()->Samples,
		std::vector<uint16>(Samples.begin(), Samples.end()));
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	const std::filesystem::path CachePath = GetTerrainCachePath(Key);
	std::string Error;
	const std::array<uint8, 4> Corrupt{1, 2, 3, 4};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(Corrupt)), CachePath));
	Reloaded = nullptr;
	const Durin::FAssetResult Rebuilt = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Reloaded);
	ASSERT_TRUE(Rebuilt) << Rebuilt.Message;
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_FALSE(Reloaded->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Reloaded->GetPayload()->Samples,
		std::vector<uint16>(Samples.begin(), Samples.end()));
}

TEST(FTerrainHeightmapImportTests, AsyncLoadHandlesWarmDdcCorruptionRecoveryAndMissingSource)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "AsyncReloadDDC");
	const std::filesystem::path Source = Root / "Sources/AsyncReload.raw";
	Durin::FByteArray Bytes(513u * 513u * sizeof(uint16));
	for (size_t Index = 0; Index < Bytes.size() / 2; ++Index)
	{
		const uint16 Value = static_cast<uint16>(Index);
		Bytes[Index * 2] = static_cast<std::byte>(Value);
		Bytes[Index * 2 + 1] = static_cast<std::byte>(Value >> 8);
	}
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(Bytes)), Source));
	const Durin::Testing::TFactoryImportResult<Durin::DTerrainHeightmap> Imported =
		Durin::AssetForge::Builtins::ImportTerrainHeightmapForTest(
			Source.generic_string(), "/TerrainHeightmap/AsyncReload");
	ASSERT_TRUE(Imported) << Imported.Message;
	const std::string Key = Imported.Asset->GetDerivedDataKey();
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TerrainHeightmap/AsyncReload", Path));
	ASSERT_TRUE(Durin::UnloadPackage(Path));

	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
	Durin::DTerrainHeightmap* Reloaded = nullptr;
	Durin::FAssetResult Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetStatus(), Durin::ETerrainHeightmapStatus::Ready);
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	Reloaded = nullptr;
	Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Reloaded, nullptr);
	const Durin::FPackageResourceHandle WarmResource =
		Durin::GetPackageResourceManager().FindPackage(Path.ToString());
	ASSERT_TRUE(WarmResource);
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 0u);
	EXPECT_EQ(Reloaded->GetStatus(), Durin::ETerrainHeightmapStatus::Ready);
	std::string Error;
	ASSERT_TRUE(Durin::WaitForTerrainHeightmapDerivedDataLoad(*Reloaded, Error)) << Error;
	EXPECT_EQ(Reloaded->GetStatus(), Durin::ETerrainHeightmapStatus::Ready);
	EXPECT_TRUE(Reloaded->WasLoadedFromDerivedDataCache());
	ASSERT_TRUE(Durin::UnloadPackage(Path));

	const std::filesystem::path CachePath = GetTerrainCachePath(Key);
	const std::array<uint8, 4> Corrupt{1, 2, 3, 4};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(Corrupt)), CachePath));
	Reloaded = nullptr;
	Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_TRUE(Durin::WaitForTerrainHeightmapDerivedDataLoad(*Reloaded, Error)) << Error;
	EXPECT_FALSE(Reloaded->WasLoadedFromDerivedDataCache());
	EXPECT_NE(Reloaded->GetLastDiagnostic().find("Rebuilt terrain heightmap"), std::string::npos);
	ASSERT_TRUE(Durin::UnloadPackage(Path));

	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(Corrupt)), CachePath));
	std::error_code ErrorCode;
	std::filesystem::remove(Source, ErrorCode);
	Reloaded = nullptr;
	Loaded = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Reloaded);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	EXPECT_TRUE(Durin::WaitForTerrainHeightmapDerivedDataLoad(*Reloaded, Error)) << Error;
	EXPECT_EQ(Reloaded->GetStatus(), Durin::ETerrainHeightmapStatus::Ready);
	EXPECT_FALSE(Reloaded->WasLoadedFromDerivedDataCache());
	ASSERT_TRUE(Durin::UnloadPackage(Path));
	Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
}
