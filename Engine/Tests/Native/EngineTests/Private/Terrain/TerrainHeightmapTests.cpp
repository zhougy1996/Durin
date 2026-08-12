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
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"

#include <gtest/gtest.h>

namespace
{
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
	Durin::FTerrainHeightmapDerivedDataKeyInput KeyInput{
		.SourceContentHash = Durin::FXxHash128::HashBuffer(std::as_bytes(std::span(Samples))),
		.TargetPlatform = Durin::Asset::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::Asset::ECookTargetProfile::Game};
	std::string FirstKey;
	std::string SecondKey;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapDerivedDataKey(KeyInput, FirstKey, Error)) << Error;
	ASSERT_TRUE(Durin::BuildTerrainHeightmapDerivedDataKey(KeyInput, SecondKey, Error)) << Error;
	EXPECT_EQ(FirstKey, SecondKey);
	EXPECT_EQ(FirstKey, "7b5c3faf0186011b52e3ff3368519321");
	EXPECT_EQ(FirstKey.size(), 32);
	std::vector<Durin::uint8> Bytes;
	ASSERT_TRUE(Durin::EncodeTerrainHeightmapPayload(
		*Payload, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Bytes, Error)) << Error;
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Bytes).ToString(),
		"b82a8b45c019f5a7a7d9c748c9d25d17");
	std::shared_ptr<const Durin::FTerrainHeightmapPayload> Decoded;
	ASSERT_TRUE(Durin::DecodeTerrainHeightmapPayload(
		Bytes, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Decoded));
	EXPECT_EQ(Decoded->Samples, Payload->Samples);
	EXPECT_EQ(Decoded->Nodes, Payload->Nodes);
	Bytes.back() ^= 0x80;
	EXPECT_FALSE(Durin::DecodeTerrainHeightmapPayload(
		Bytes, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, Decoded));
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

TEST(FTerrainHeightmapImportTests, ExplicitImportReimportAndRollbackPreserveTheAssetContract)
{
	const std::filesystem::path Root = InitializeHeightmapTests();
	FScopedDdcRoot Ddc(Root / "DDC");
	std::string Error;
	ASSERT_TRUE(Durin::RegisterStandardAssetImportProviders(Error)) << Error;
	const std::filesystem::path Source = Root / "Sources/Asymmetric.png";
	const std::array<Durin::uint16, 6> Initial{0, 100, 200, 300, 400, 65'535};
	WritePng(Source, 3, 2, Initial);
	const Durin::FTerrainHeightmapImportResult Imported =
		Durin::AssetBuild::ImportTerrainHeightmapAsset(
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
	const Durin::FTexture2DImportResult TextureImport = Durin::StandardAssetImport::ImportTexture2DAsset(
		Source.generic_string(), "/TerrainHeightmap/DefaultPngTexture");
	ASSERT_TRUE(TextureImport) << TextureImport.Message;
	ASSERT_NE(TextureImport.Asset, nullptr);

	auto Plan = Durin::AssetImport::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::AssetImport::GetProviderRegistry(),
		Durin::AssetImport::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const Durin::uint64 InitialRevision = Imported.Asset->GetRevision();
	const auto Noop = Durin::AssetImport::ExecuteSingleAssetImport(Plan.Plan);
	ASSERT_TRUE(Noop) << Noop.Message;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());

	const std::array<Durin::uint16, 6> Changed{1, 2, 3, 4, 5, 6};
	WritePng(Source, 3, 2, Changed);
	Plan = Durin::AssetImport::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::AssetImport::GetProviderRegistry(),
		Durin::AssetImport::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Updated = Durin::AssetImport::ExecuteSingleAssetImport(Plan.Plan);
	ASSERT_TRUE(Updated) << Updated.Message;
	EXPECT_EQ(Imported.Asset->GetRevision(), InitialRevision + 1);
	EXPECT_EQ(Imported.Asset->GetMinimum(), 1);
	EXPECT_EQ(Imported.Asset->GetMaximum(), 6);
	EXPECT_FALSE(Imported.Asset->GetPackage()->IsDirty());

	const Durin::uint64 BeforeFailureRevision = Imported.Asset->GetRevision();
	const auto BeforeFailurePayload = Imported.Asset->GetPayload();
	const std::array<Durin::uint16, 6> FailedChange{9, 9, 9, 9, 9, 9};
	WritePng(Source, 3, 2, FailedChange);
	Plan = Durin::AssetImport::CreateSingleAssetReimportPlan(
		{.Asset = Imported.Asset}, Durin::AssetImport::GetProviderRegistry(),
		Durin::AssetImport::GetSingleAssetHandlerRegistry());
	ASSERT_TRUE(Plan) << Plan.Message;
	const auto Failed = Durin::AssetImport::ExecuteSingleAssetImport(
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
		Durin::AssetBuild::ImportTerrainHeightmapAsset(
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
