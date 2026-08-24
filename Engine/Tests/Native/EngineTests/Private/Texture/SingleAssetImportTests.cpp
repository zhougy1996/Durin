#include "AssetForge/ImportTypes.h"
#include "AssetForge/ImportService.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetTools.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "AssetForgeBuiltinsProviders.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Texture/TextureCube.h"
#include "TextureTestSupport.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "Threading/Task.h"

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
		EXPECT_TRUE(Durin::AssetForge::Builtins::RegisterAssetForgeBuiltinsProviders(
			Error, GetEngineTestModuleCallbackGate())) << Error;
		return Root;
	}

	class FScopedSingleAssetTaskScheduler
	{
	public:
		FScopedSingleAssetTaskScheduler()
		{
			Durin::ShutdownTaskScheduler(false);
			bInitialized = Durin::InitializeTaskScheduler(2);
		}
		~FScopedSingleAssetTaskScheduler()
		{
			Durin::ShutdownTaskScheduler(false);
		}
		auto IsInitialized() const -> bool { return bInitialized; }
	private:
		bool bInitialized = false;
	};

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

TEST(FSingleAssetImportTests, Texture2DPersistsImportProvenance)
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
	Durin::AssetForge::FImportProvenance Provenance;
	std::string Error;
	EXPECT_TRUE(Durin::AssetForge::Builtins::InspectTexture2DImportProvenance(
		*Imported.Asset, Provenance, Error)) << Error;
	EXPECT_EQ(Provenance.Translator.Id, "Durin.Image");
	ASSERT_EQ(Provenance.PlanningPassStack.size(), 1u);
	EXPECT_EQ(Provenance.PlanningPassStack.front().PlanningPassId,
		"Durin.Texture2D.Default");
	ASSERT_TRUE(Imported.Asset->GetSourceImportData().HasSource());
	Imported.Asset->PublishImportProvenance({});
	EXPECT_FALSE(Durin::AssetForge::Builtins::InspectTexture2DImportProvenance(
		*Imported.Asset, Provenance, Error));
	EXPECT_EQ(Error, "Texture2D has no current AssetForge provenance.");
}

TEST(FSingleAssetImportTests, ReimportsGeometryOnlyThroughAssetForge)
{
	InitializeSingleAssetImportTests();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaticMeshDdc");
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::FStaticMeshImportResult Imported = Durin::AssetForge::Builtins::ImportStaticMeshAsset(
		Source.generic_string(), "/SingleAssetStage2/Geometry");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Imported.Asset->GetPackage()->GetPackagePath(), AssetPath));
	Durin::AssetForge::FImportProvenance Provenance;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::InspectStaticMeshImportProvenance(
		*Imported.Asset, Provenance, Error)) << Error;
	Durin::AssetForge::FImportRequest Request;
	ASSERT_TRUE(Durin::AssetForge::Builtins::MakeStaticMeshImportRequest(
		Imported.Asset->GetSourceImportData().SourcePath, AssetPath,
		Imported.Asset->GetImportSettings(), Durin::AssetForge::EImportMode::Reimport,
		{.OwnerId = "Tests.StaticMesh.ImportReimport"}, Provenance,
		Request, Error)) << Error;
	const Durin::AssetForge::FImportResult Result =
		Durin::AssetForge::GetImportService().RunImportInline(
			std::move(Request), "Reimport StaticMesh");
	EXPECT_EQ(Result.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Result.Outcome.Diagnostic;
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);
	ASSERT_TRUE(Imported.Asset->GetSourceImportData().HasSource());
	Imported.Asset->PublishImportProvenance({});
	EXPECT_FALSE(Durin::AssetForge::Builtins::InspectStaticMeshImportProvenance(
		*Imported.Asset, Provenance, Error));
	EXPECT_EQ(Error, "StaticMesh has no current AssetForge provenance.");
}

TEST(FSingleAssetImportTests, ReimportsGeometryThroughScheduledImportJob)
{
	InitializeSingleAssetImportTests();
	FScopedSingleAssetTaskScheduler Scheduler;
	ASSERT_TRUE(Scheduler.IsInitialized());
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaticMeshAsyncDdc");
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::FStaticMeshImportResult Imported = Durin::AssetForge::Builtins::ImportStaticMeshAsset(
		Source.generic_string(), "/SingleAssetStage2/AsyncGeometry");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Imported.Asset->GetPackage()->GetPackagePath(), AssetPath));
	Durin::AssetForge::FImportProvenance Provenance;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::InspectStaticMeshImportProvenance(
		*Imported.Asset, Provenance, Error)) << Error;
	Durin::AssetForge::FImportRequest Request;
	ASSERT_TRUE(Durin::AssetForge::Builtins::MakeStaticMeshImportRequest(
		Imported.Asset->GetSourceImportData().SourcePath, AssetPath,
		Imported.Asset->GetImportSettings(), Durin::AssetForge::EImportMode::Reimport,
		{.OwnerId = "Tests.StaticMesh.ScheduledAssetForge"}, Provenance,
		Request, Error)) << Error;
	auto Handle = Durin::AssetForge::GetImportService().SubmitImport(
		Request, "Scheduled StaticMesh reimport");
	ASSERT_TRUE(Handle);
	Durin::AssetForge::FImportResult Executed;
	for (uint32 Attempt = 0; Attempt < 10'000 && !Handle.TryGetResult(Executed); ++Attempt)
	{
		(void)Durin::AssetForge::GetImportService().PumpImportOperations();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(Executed.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Executed.Outcome.Diagnostic;
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);

	Request.Owner.OwnerId = "Tests.StaticMesh.CanceledImport";
	auto CanceledHandle = Durin::AssetForge::GetImportService().SubmitImport(
		std::move(Request), "Canceled StaticMesh reimport");
	ASSERT_TRUE(CanceledHandle.GetOperationHandle().RequestCancel());
	Durin::AssetForge::GetImportService().CancelAndDrainImportOperation(
		CanceledHandle.GetOperationHandle());
	Durin::AssetForge::FImportResult Canceled;
	ASSERT_TRUE(CanceledHandle.TryGetResult(Canceled));
	EXPECT_EQ(Canceled.Outcome.State, Durin::AssetForge::EImportOperationState::Canceled);
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
	Durin::FAssetPath Destination;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Imported.Asset->GetPackage()->GetPackagePath(), Destination));
	Durin::AssetForge::FImportProvenance Provenance;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::InspectTextureCubeImportProvenance(
		*Imported.Asset, Provenance, Error)) << Error;
	Durin::AssetForge::FImportRequest Request;
	const std::array Sources{Imported.Asset->GetSourceImportData().Panorama.SourcePath};
	ASSERT_TRUE(Durin::AssetForge::Builtins::MakeTextureCubeImportRequest(
		Sources, Durin::ETextureCubeSourceLayout::EquirectangularPanorama,
		Destination, {.bSRGB = Imported.Asset->IsSRGB()},
		{.FaceDimension = Imported.Asset->GetPanoramaFaceDimension(),
			.ExposureEV = Imported.Asset->GetPanoramaExposureEV()},
		Durin::AssetForge::EImportMode::Reimport,
		{.OwnerId = "Tests.TextureCube.ImportReimport"}, Provenance,
		Request, Error)) << Error;
	const auto Result = Durin::AssetForge::GetImportService().RunImportInline(
		std::move(Request), "Reimport panorama TextureCube");
	EXPECT_EQ(Result.Outcome.State, Durin::AssetForge::EImportOperationState::Succeeded)
		<< Result.Outcome.Diagnostic;
	EXPECT_EQ(Imported.Asset->GetSourceLayout(),
		Durin::ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_NE(Imported.Asset->GetPlatformData(), nullptr);
}
