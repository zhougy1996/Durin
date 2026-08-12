#include "AssetSystem.h"
#include "CookedAsset.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Components/TerrainComponent.h"
#include "Engine/FPrimitiveSceneProxy.h"

#include <gtest/gtest.h>

namespace
{
	auto BigU32(std::vector<Durin::uint8>& Bytes, Durin::uint32 Value) -> void
	{
		for (int Shift : {24, 16, 8, 0})
			Bytes.push_back(static_cast<Durin::uint8>(Value >> Shift));
	}

	auto Crc(std::span<const Durin::uint8> Bytes) -> Durin::uint32
	{
		Durin::uint32 Value = 0xffffffffu;
		for (Durin::uint8 Byte : Bytes)
		{
			Value ^= Byte;
			for (Durin::uint32 Bit = 0; Bit < 8; ++Bit)
				Value = (Value >> 1) ^ (0xedb88320u & (0u - (Value & 1u)));
		}
		return ~Value;
	}

	auto Chunk(std::vector<Durin::uint8>& Png, std::string_view Type,
		std::span<const Durin::uint8> Data) -> void
	{
		BigU32(Png, static_cast<Durin::uint32>(Data.size()));
		const size_t Begin = Png.size();
		Png.insert(Png.end(), Type.begin(), Type.end());
		Png.insert(Png.end(), Data.begin(), Data.end());
		BigU32(Png, Crc(std::span(Png).subspan(Begin)));
	}

	auto MakePng(std::span<const Durin::uint16> Samples) -> std::vector<Durin::uint8>
	{
		constexpr Durin::uint32 Width = 3;
		constexpr Durin::uint32 Height = 2;
		std::vector<Durin::uint8> Raw;
		for (Durin::uint32 Y = 0; Y < Height; ++Y)
		{
			Raw.push_back(0);
			for (Durin::uint32 X = 0; X < Width; ++X)
			{
				const Durin::uint16 Sample = Samples[Y * Width + X];
				Raw.push_back(static_cast<Durin::uint8>(Sample >> 8));
				Raw.push_back(static_cast<Durin::uint8>(Sample));
			}
		}
		std::vector<Durin::uint8> Deflate{0x78, 0x01, 0x01};
		const Durin::uint16 Count = static_cast<Durin::uint16>(Raw.size());
		const Durin::uint16 Inverse = static_cast<Durin::uint16>(~Count);
		Deflate.insert(Deflate.end(), {
			static_cast<Durin::uint8>(Count), static_cast<Durin::uint8>(Count >> 8),
			static_cast<Durin::uint8>(Inverse), static_cast<Durin::uint8>(Inverse >> 8)});
		Deflate.insert(Deflate.end(), Raw.begin(), Raw.end());
		Durin::uint32 A = 1, B = 0;
		for (Durin::uint8 Byte : Raw) { A = (A + Byte) % 65'521; B = (B + A) % 65'521; }
		BigU32(Deflate, (B << 16) | A);
		std::vector<Durin::uint8> Png{137, 80, 78, 71, 13, 10, 26, 10};
		std::vector<Durin::uint8> Header;
		BigU32(Header, Width); BigU32(Header, Height);
		Header.insert(Header.end(), {16, 0, 0, 0, 0});
		Chunk(Png, "IHDR", Header); Chunk(Png, "IDAT", Deflate); Chunk(Png, "IEND", {});
		return Png;
	}
}

TEST(FTerrainHeightmapCookTests, CookedRuntimeLoadsExactPayloadWithoutSourceOrDdc)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TerrainHeightmapCook";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path ContentRoot = Root / "Content";
	const std::filesystem::path Source = ContentRoot / "Sources/Height.png";
	std::filesystem::create_directories(Source.parent_path());
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", ContentRoot.generic_string() + "/");
	const std::string PreviousDdc = Durin::FPaths::DerivedDataCacheDir();
	Durin::FPaths::SetDerivedDataCacheDirForTests((Root / "DDC").generic_string());
	const std::array<Durin::uint16, 6> Samples{0, 17, 257, 4097, 32'768, 65'535};
	const std::vector<Durin::uint8> Png = MakePng(Samples);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Png)), Source));
	const auto Imported = Durin::AssetBuild::ImportTerrainHeightmapAsset(
		Source.generic_string(), "/Game/Height");
	ASSERT_TRUE(Imported) << Imported.Message;

	const std::filesystem::path CookRoot = Root / "Cooked";
	Durin::Asset::FCookContext Cook(
		CookRoot, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Imported.Asset->AddToCook(
		Cook, "/Game/Height", Error)) << Error;
	ASSERT_TRUE(Cook.Publish(&Error)) << Error;

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Height", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	Durin::Asset::ShutdownAssetManager();
	Durin::CollectGarbage();
	Durin::Testing::RemoveTestWorkDirectory(Root / "DDC");
	Durin::Testing::RemoveTestWorkDirectory(ContentRoot);
	Durin::FPaths::SetDerivedDataCacheDirForTests((Root / "AbsentDDC").generic_string());
	Durin::Asset::FAssetManager::Get().Initialize();
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::CookedRuntime, CookRoot}));
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	Durin::DTerrainHeightmap* Cooked = nullptr;
	const Durin::Asset::FAssetResult Loaded = Durin::Asset::LoadAsset(AssetPath, Cooked);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	ASSERT_NE(Cooked, nullptr);
	ASSERT_NE(Cooked->GetPayload(), nullptr);
	EXPECT_EQ(Cooked->GetPayload()->Samples,
		std::vector<Durin::uint16>(Samples.begin(), Samples.end()));
	EXPECT_TRUE(Cooked->GetSourceFile().empty());
	EXPECT_TRUE(Cooked->GetDerivedDataKey().empty());
	auto* Component = Durin::NewObject<Durin::DTerrainComponent>(nullptr, "CookedTerrainComponent");
	Component->SetHeightmap(Cooked);
	std::unique_ptr<Durin::FPrimitiveSceneProxy> Proxy = Component->CreateSceneProxy();
	ASSERT_NE(Proxy, nullptr);
	EXPECT_EQ(Proxy->GetKind(), Durin::EPrimitiveSceneProxyKind::Terrain);
	EXPECT_EQ(static_cast<Durin::FTerrainSceneProxy&>(*Proxy).GetPayload(), Cooked->GetPayload());
	Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousDdc);
}
