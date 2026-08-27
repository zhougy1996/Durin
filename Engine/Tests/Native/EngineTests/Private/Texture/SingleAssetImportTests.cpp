#include "AssetForge/Builtins/ImportSupport.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetTools.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/Texture2DImportData.h"
#include "Texture/TextureCube.h"
#include "TextureTestSupport.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "AssetForge/Builtins/TextureCubeImportData.h"

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
			Durin::PathUtilities::RegisterMountPointForTests(
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

TEST(FSingleAssetImportTests, Texture2DPersistsFamilyImportData)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTexture2DDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTexture2D.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Imported = Durin::AssetForge::Builtins::ImportTexture2DAsset(
		Source.generic_string(), "/SingleAssetStage2/Texture2D");
	ASSERT_TRUE(Imported) << Imported.Message;
	const auto* ImportData = dynamic_cast<const Durin::AssetForge::Builtins::DTexture2DImportData*>(
		Imported.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	EXPECT_EQ(ImportData->GetDecoderId(), "DurinImage");
	EXPECT_EQ(ImportData->GetDecoderVersion(), 1u);
	ASSERT_NE(Imported.Asset->GetImportedSource(), nullptr);
	EXPECT_EQ(Imported.Asset->GetImportedSource()->Filename,
		Imported.Asset->GetSourceFile());
}

TEST(FSingleAssetImportTests, ReimportsGeometryDirectlyFromFamilyImportData)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaticMeshDdc");
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::FStaticMeshImportResult Imported = Durin::AssetForge::Builtins::ImportStaticMeshAsset(
		Source.generic_string(), "/SingleAssetStage2/Geometry");
	ASSERT_TRUE(Imported) << Imported.Message;
	const auto* ImportData = dynamic_cast<const Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Imported.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	EXPECT_EQ(ImportData->GetImporterId(), "Assimp");
	ASSERT_NE(Imported.Asset->GetImportedSource(), nullptr);
	EXPECT_FALSE(Imported.Asset->GetImportedSource()->Filename.empty());
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMeshSource(
		*Imported.Asset, {}, Error)) << Error;
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);
	ASSERT_NE(Imported.Asset->GetImportedSource(), nullptr);
}

TEST(FSingleAssetImportTests, ReimportsPanoramaTextureCubeFromCapturedBytes)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTextureCubeDdc");
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR)
		/ "EquirectangularPanorama" / "AnalyticalLDR.tga";
	Durin::AssetForge::Builtins::FTextureCubeImportResult Imported = Durin::AssetForge::Builtins::ImportTextureCubePanorama(
		Source.generic_string(), "/SingleAssetStage2/Panorama");
	ASSERT_TRUE(Imported) << Imported.Message;
	const auto* ImportData = dynamic_cast<const Durin::AssetForge::Builtins::DTextureCubeImportData*>(
		Imported.Asset->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	EXPECT_EQ(ImportData->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	ASSERT_NE(ImportData->GetSourceData().FindByRole("panorama"), nullptr);
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTextureCubePanorama(
		*Imported.Asset, {},
		{.FaceDimension = Imported.Asset->GetPanoramaFaceDimension(),
			.ExposureEV = Imported.Asset->GetPanoramaExposureEV()},
		Error)) << Error;
	EXPECT_EQ(Imported.Asset->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_NE(Imported.Asset->GetPlatformData(), nullptr);
}
