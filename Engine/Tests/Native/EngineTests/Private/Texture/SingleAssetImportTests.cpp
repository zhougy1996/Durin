#include "AssetImportCore.h"
#include "ImportService.h"
#include "ImportedScene.h"
#include "TextureCubeSourceTranslation.h"
#include "AssetLoad.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "AssetForgeProviders.h"
#include "StaticMeshSourceTranslation.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"
#include "Texture/TextureCube.h"
#include "TextureTestSupport.h"
#include "TerrainHeightmapSourceTranslation.h"

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
		std::string Error;
		EXPECT_TRUE(Durin::Asset::Forge::RegisterAssetForgeProviders(
			Error, GetEngineTestModuleCallbackGate())) << Error;
		return Root;
	}

	auto PlanCurrent(Durin::DObject* Asset)
		-> Durin::Asset::FSingleAssetPlanResult
	{
		return Durin::Asset::GetImportService().CreateSingleAssetReimportPlan(
			{.Asset = Asset});
	}
}

TEST(FAssetForgeImageSourcePolicyTests, KeepsCodecCapabilitySeparateFromAssetAdmission)
{
	using namespace Durin::Asset::Forge;
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

TEST(FSingleAssetImportTests, Texture2DRestoresAuthoredAndRuntimeStateWhenSaveFails)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTexture2DDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTexture2D.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Imported = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/SingleAssetStage2/Texture2D");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Identity = Imported.Asset;
	const std::string PriorKey = Identity->GetDerivedDataKey();
	const Durin::FTexture2DSourceImportData PriorSource = Identity->GetSourceImportData();
	ASSERT_FALSE(Identity->GetPackage()->IsDirty());

	auto PlanResult = PlanCurrent(Identity);
	ASSERT_TRUE(PlanResult) << PlanResult.Message;
	const auto Capabilities = Durin::Asset::GetImportService()
		.QuerySingleAssetCapabilities(*Identity);
	const auto* Capability = Capabilities.Find(
		Durin::Asset::ESingleAssetImportCapability::ReimportCurrentSource);
	ASSERT_NE(Capability, nullptr);
	EXPECT_TRUE(Capability->bAvailable);
	EXPECT_NE(Capability->ReplacedStateDescription.find("render resource"), std::string::npos);

	const auto Failed = Durin::Asset::GetImportService().ExecuteSingleAssetImport(
		PlanResult.Plan,
		{.SaveOptions = {.ShouldFail = [](Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
			return Phase == Durin::Asset::EAssetBundleSavePhase::StagePackage;
		}}});
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Identity, Imported.Asset);
	EXPECT_EQ(Identity->GetDerivedDataKey(), PriorKey);
	EXPECT_EQ(Identity->GetSourceImportData(), PriorSource);
	EXPECT_FALSE(Identity->GetPackage()->IsDirty());
}

TEST(FSingleAssetImportTests, RejectsStalePackageRevisionBeforeExchange)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaleDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStale.png";
	WriteTextureFixture(Source);
	Durin::FTexture2DImportResult Imported = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/SingleAssetStage2/StaleTexture");
	ASSERT_TRUE(Imported) << Imported.Message;
	auto PlanResult = PlanCurrent(Imported.Asset);
	ASSERT_TRUE(PlanResult) << PlanResult.Message;
	const std::string PriorKey = Imported.Asset->GetDerivedDataKey();
	Imported.Asset->GetPackage()->MarkDirty();
	const auto Result = Durin::Asset::GetImportService().ExecuteSingleAssetImport(PlanResult.Plan);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Imported.Asset->GetDerivedDataKey(), PriorKey);
	EXPECT_TRUE(std::ranges::any_of(Result.Diagnostics, [](const auto& Diagnostic) {
		return Diagnostic.Category == Durin::Asset::EImportDiagnosticCategory::StalePlan;
	}));
}

TEST(FSingleAssetImportTests, ReimportsGeometryWithoutAnImportRecord)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaticMeshDdc");
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::FStaticMeshImportResult Imported = Durin::Asset::Forge::ImportStaticMeshAsset(
		Source.generic_string(), "/SingleAssetStage2/Geometry");
	ASSERT_TRUE(Imported) << Imported.Message;
	auto PlanResult = PlanCurrent(Imported.Asset);
	ASSERT_TRUE(PlanResult) << PlanResult.Message;
	const auto Result = Durin::Asset::GetImportService().ExecuteSingleAssetImport(PlanResult.Plan);
	EXPECT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.Asset, Imported.Asset);
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);
}

TEST(FSingleAssetImportTests, ReimportsPanoramaTextureCubeFromCapturedBytes)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetTextureCubeDdc");
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR)
		/ "EquirectangularPanorama" / "AnalyticalLDR.tga";
	Durin::Asset::Forge::FTextureCubeImportResult Imported = Durin::Asset::Forge::ImportTextureCubePanorama(
		Source.generic_string(), "/SingleAssetStage2/Panorama");
	ASSERT_TRUE(Imported) << Imported.Message;
	auto PlanResult = PlanCurrent(Imported.Asset);
	ASSERT_TRUE(PlanResult) << PlanResult.Message;
	const auto Result = Durin::Asset::GetImportService().ExecuteSingleAssetImport(PlanResult.Plan);
	EXPECT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.Asset, Imported.Asset);
	EXPECT_EQ(Imported.Asset->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_NE(Imported.Asset->GetPlatformData(), nullptr);
}
