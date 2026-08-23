#include "AssetImportCore.h"
#include "ImportService.h"
#include "ImportedScene.h"
#include "TextureCubeSourceTranslation.h"
#include "AssetTools.h"
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

	auto WaitForSingleAssetExecution(
		Durin::Asset::FSingleAssetAsyncExecutionHandle& Handle,
		Durin::Asset::FSingleAssetExecutionResult& OutResult)
		-> Durin::Asset::EAsyncImportPlanStatus
	{
		for (uint32 Attempt = 0; Attempt < 10'000; ++Attempt)
		{
			const auto Status = Durin::Asset::GetImportService().
				PollSingleAssetImportExecution(Handle, OutResult);
			if (Status != Durin::Asset::EAsyncImportPlanStatus::Pending)
				return Status;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return Durin::Asset::EAsyncImportPlanStatus::Pending;
	}

	auto WaitForSingleAssetPlan(
		Durin::Asset::FSingleAssetAsyncPlanHandle& Handle,
		Durin::Asset::FSingleAssetPlanResult& OutResult)
		-> Durin::Asset::EAsyncImportPlanStatus
	{
		for (uint32 Attempt = 0; Attempt < 10'000; ++Attempt)
		{
			const auto Status = Durin::Asset::GetImportService().
				PollSingleAssetReimportPlan(Handle, OutResult);
			if (Status != Durin::Asset::EAsyncImportPlanStatus::Pending)
				return Status;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return Durin::Asset::EAsyncImportPlanStatus::Pending;
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

TEST(FSingleAssetImportTests, Texture2DIsAdmittedOnlyByInterchange)
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
	auto PlanResult = PlanCurrent(Imported.Asset);
	EXPECT_FALSE(PlanResult);
	const auto Capabilities = Durin::Asset::GetImportService()
		.QuerySingleAssetCapabilities(*Imported.Asset);
	const auto* LegacyReimport = Capabilities.Find(
		Durin::Asset::ESingleAssetImportCapability::ReimportCurrentSource);
	ASSERT_NE(LegacyReimport, nullptr);
	EXPECT_FALSE(LegacyReimport->bAvailable);
	Durin::Asset::FInterchangeProvenance Provenance;
	std::string Error;
	EXPECT_TRUE(Durin::Asset::Forge::InspectTexture2DInterchangeProvenance(
		*Imported.Asset, Provenance, Error)) << Error;
	EXPECT_EQ(Provenance.Translator.Id, "Durin.Image");
	ASSERT_EQ(Provenance.PipelineStack.size(), 1u);
	EXPECT_EQ(Provenance.PipelineStack.front().PipelineId,
		"Durin.Texture2D.Default");
}

TEST(FSingleAssetImportTests, ReimportsGeometryOnlyThroughInterchange)
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
	EXPECT_FALSE(PlanResult);
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Imported.Asset->GetPackage()->GetPackagePath(), AssetPath));
	Durin::Asset::FInterchangeProvenance Provenance;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::InspectStaticMeshInterchangeProvenance(
		*Imported.Asset, Provenance, Error)) << Error;
	Durin::Asset::FInterchangeImportRequest Request;
	ASSERT_TRUE(Durin::Asset::Forge::MakeStaticMeshInterchangeRequest(
		Imported.Asset->GetSourceImportData().SourcePath, AssetPath,
		Imported.Asset->GetImportSettings(), Durin::Asset::EInterchangeImportMode::Reimport,
		{.OwnerId = "Tests.StaticMesh.InterchangeReimport"}, Provenance,
		Request, Error)) << Error;
	const Durin::Asset::FInterchangeImportResult Result =
		Durin::Asset::GetImportService().RunInterchangeImportInline(
			std::move(Request), "Reimport StaticMesh");
	EXPECT_EQ(Result.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Result.Outcome.Diagnostic;
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);
}

TEST(FSingleAssetImportTests, ReimportsGeometryThroughScheduledInterchangeJob)
{
	InitializeSingleAssetImportTests();
	FScopedSingleAssetTaskScheduler Scheduler;
	ASSERT_TRUE(Scheduler.IsInitialized());
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "SingleAssetStaticMeshAsyncDdc");
	const std::filesystem::path Source =
		std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj";
	Durin::FStaticMeshImportResult Imported = Durin::Asset::Forge::ImportStaticMeshAsset(
		Source.generic_string(), "/SingleAssetStage2/AsyncGeometry");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Imported.Asset->GetPackage()->GetPackagePath(), AssetPath));
	Durin::Asset::FInterchangeProvenance Provenance;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::InspectStaticMeshInterchangeProvenance(
		*Imported.Asset, Provenance, Error)) << Error;
	Durin::Asset::FInterchangeImportRequest Request;
	ASSERT_TRUE(Durin::Asset::Forge::MakeStaticMeshInterchangeRequest(
		Imported.Asset->GetSourceImportData().SourcePath, AssetPath,
		Imported.Asset->GetImportSettings(), Durin::Asset::EInterchangeImportMode::Reimport,
		{.OwnerId = "Tests.StaticMesh.ScheduledInterchange"}, Provenance,
		Request, Error)) << Error;
	auto Handle = Durin::Asset::GetImportService().SubmitInterchangeImport(
		Request, "Scheduled StaticMesh reimport");
	ASSERT_TRUE(Handle);
	Durin::Asset::FInterchangeImportResult Executed;
	for (uint32 Attempt = 0; Attempt < 10'000 && !Handle.TryGetResult(Executed); ++Attempt)
	{
		(void)Durin::Asset::GetImportService().PumpImportOperations();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(Executed.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Executed.Outcome.Diagnostic;
	EXPECT_NE(Imported.Asset->GetRenderData(), nullptr);

	Request.Owner.OwnerId = "Tests.StaticMesh.CanceledInterchange";
	auto CanceledHandle = Durin::Asset::GetImportService().SubmitInterchangeImport(
		std::move(Request), "Canceled StaticMesh reimport");
	ASSERT_TRUE(CanceledHandle.GetOperationHandle().RequestCancel());
	Durin::Asset::GetImportService().CancelAndDrainImportOperation(
		CanceledHandle.GetOperationHandle());
	Durin::Asset::FInterchangeImportResult Canceled;
	ASSERT_TRUE(CanceledHandle.TryGetResult(Canceled));
	EXPECT_EQ(Canceled.Outcome.State, Durin::Asset::EImportOperationState::Canceled);
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
