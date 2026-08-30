#include "AssetForge/Builtins/ImportedScene.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "EditorReimportHandler.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Texture/TextureCube.h"
#include "TextureTestSupport.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "Terrain/TerrainHeightmapFactoryTestSupport.h"
#include "Texture/VolumeTextureFactoryTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto InitializeSingleAssetImportTests() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "SingleAssetImportStage2";
		static const bool Initialized = [&] {
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::Testing::RegisterMountPointForTests(
				"/SingleAssetStage2/", Root.generic_string() + "/");
			return true;
		}();
		(void)Initialized;
		return Root;
	}

}

TEST(FAssetForgeBuiltinsImageSourcePolicyTests, KeepsCodecCapabilitySeparateFromAssetAdmission)
{
	using namespace Durin::AssetForge::Builtins;
	EXPECT_TRUE(IsTexture2DSourceExtension(".PNG"));
	EXPECT_FALSE(IsTexture2DSourceExtension(".hdr"));
	EXPECT_TRUE(IsTextureCubeFaceSourceExtension(".tga"));
	EXPECT_FALSE(IsTextureCubeFaceSourceExtension(".hdr"));
	EXPECT_TRUE(IsTextureCubePanoramaSourceExtension(".hdr"));
	EXPECT_FALSE(IsTextureCubePanoramaSourceExtension(".gif"));
	EXPECT_TRUE(IsTerrainHeightmapSourceExtension(".png"));
	EXPECT_TRUE(IsTerrainHeightmapSourceExtension(".RAW"));
	EXPECT_FALSE(IsTerrainHeightmapSourceExtension(".jpg"));
	EXPECT_TRUE(IsSceneSurfaceImageEncodingSupported(EImportedImageEncoding::Png));
	EXPECT_FALSE(IsSceneSurfaceImageEncodingSupported(
		static_cast<EImportedImageEncoding>(255)));
}

TEST(FSingleAssetImportTests, ReimportsGeometryDirectlyFromFamilyImportData)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaticMeshDdc");
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Imported = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Source.generic_string(), "/SingleAssetStage2/Geometry");
	ASSERT_TRUE(Imported) << Imported.Message;
	const auto* ImportData = dynamic_cast<const Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Imported.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	const Durin::FSourceFile* ImportedSource =
		ImportData->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_FALSE(ImportedSource->Hint.empty());
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*Imported.Asset, {},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);
	ASSERT_NE(Imported.Asset->GetAssetImportData(), nullptr);
	EXPECT_NE(Imported.Asset->GetAssetImportData()->GetSourceData().FindByRole("source"), nullptr);
}

TEST(FSingleAssetImportTests, FailedFamilyFactoriesDiscardTheirFormalPackages)
{
	const std::filesystem::path Root = InitializeSingleAssetImportTests();
	const std::array<uint8, 4> InvalidBytes{1, 2, 3, 4};
	const std::filesystem::path TerrainSource = Root / "Invalid.raw";
	const std::filesystem::path VolumeSource = Root / "Invalid.png";
	const std::filesystem::path MeshSource = Root / "Invalid.obj";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{InvalidBytes}), TerrainSource));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{InvalidBytes}), VolumeSource));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span{InvalidBytes}), MeshSource));

	const auto Terrain = Durin::AssetForge::Builtins::ImportTerrainHeightmapForTest(
		TerrainSource.generic_string(), "/SingleAssetStage2/InvalidTerrain");
	const auto Volume = Durin::AssetForge::Builtins::ImportVolumeTextureForTest(
		VolumeSource.generic_string(), "/SingleAssetStage2/InvalidVolume", {
			.SliceWidth = 1, .SliceHeight = 1, .Depth = 1,
			.TilesX = 1, .TilesY = 1});
	const auto Mesh = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		MeshSource.generic_string(), "/SingleAssetStage2/InvalidMesh");
	EXPECT_FALSE(Terrain);
	EXPECT_FALSE(Volume);
	EXPECT_FALSE(Mesh);

	for (const std::string_view PathText : {
		"/SingleAssetStage2/InvalidTerrain",
		"/SingleAssetStage2/InvalidVolume",
		"/SingleAssetStage2/InvalidMesh"})
	{
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(PathText, Path));
		EXPECT_EQ(Durin::Asset::FindResidentPackage(Path), nullptr);
	}
}

TEST(FSingleAssetImportTests, ReimportsPanoramaTextureCubeFromCapturedBytes)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTextureCubeDdc");
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR)
		/ "EquirectangularPanorama" / "AnalyticalLDR.tga";
	Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Imported = Durin::AssetForge::Builtins::ImportTextureCubePanoramaForTest(
		Source.generic_string(), "/SingleAssetStage2/Panorama");
	ASSERT_TRUE(Imported) << Imported.Message;
	const auto* ImportData = Imported.Asset->GetAssetImportData();
	ASSERT_NE(ImportData, nullptr);
	EXPECT_EQ(Imported.Asset->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	ASSERT_NE(ImportData->GetSourceData().FindByRole("panorama"), nullptr);
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*Imported.Asset, {},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_EQ(Imported.Asset->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_NE(Imported.Asset->GetPlatformData(), nullptr);
}
