#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "Materials/ExplicitMaterialProgramTestFixture.h"
#include "Threading/Task.h"
#include "NativeAssetTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "Materials/MaterialTestSupport.h"
#include "Texture/TextureFactoryTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "MonaUIBackend.h"
#include "MonaTestFixtures.h"
#include "NativeTestSupport.h"
#include "Preview/PreviewMeshResources.h"
#include "RHICommandList.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/AssetThumbnailTestFixtures.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Thumbnail/TextureThumbnailRenderer.h"
#include "TexturePreview.h"
#include "Texture/TextureCubeRenderResource.h"
#include "AssetForge/Builtins/Texture2DImport.h"

#include <condition_variable>
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"

namespace
{
	// Render completion precedes the task-scheduled GameThread publication receipt.
	// Pump until the consumer's state changes, including any queued successor upload.
	template<typename FPredicate>
	auto WaitForResourcePublication(FPredicate&& IsComplete) -> testing::AssertionResult
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (;;)
		{
			Durin::FlushRenderingCommands();
			Durin::PumpGameThreadDeferredWork();
			if (IsComplete()) return testing::AssertionSuccess();
			if (std::chrono::steady_clock::now() >= Deadline)
				return testing::AssertionFailure() << "Timed out waiting for render resource publication.";
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	class FThumbnailVulkanTests : public testing::Test
	{
	public:
		static auto SetUpTestSuite() -> void
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(Durin::InitializeTaskScheduler());
			ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
		}

		static auto TearDownTestSuite() -> void
		{
			// Compiler shutdown is terminal; keep its scheduler alive across cases.
			Durin::ShutdownAssetCompilingManager();
			Durin::ShutdownTaskSystem();
		}

	protected:
		auto SetUp() -> void override
		{
			Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
			Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
			InitializeDObjectSystem();
			Durin::FMountPaths::InitDefaultMountPoints();
			const auto Root = Durin::Testing::CreateTestFixtureDirectory("ThumbnailVulkan");
			std::vector<Durin::FMountPoint> Mounts(
				Durin::FMountPaths::GetRegisteredMountPoints().begin(),
				Durin::FMountPaths::GetRegisteredMountPoints().end()
			);
			Mounts.push_back({.VirtualRoot = "/ThumbnailVulkan/", .Owner = Durin::EMountOwner::Test, .Root = Root, .bAutoScan = true, .bContentWritable = true});
			MountRegistry = std::make_unique<Durin::Testing::FScopedMountRegistryFixture>(Mounts);
			ASSERT_TRUE(MountRegistry->IsValid()) << MountRegistry->GetError();
			ASSERT_TRUE(Durin::RefreshAssetRegistry());
			ASSERT_EQ(Durin::GDynamicRHI, nullptr);
			Durin::InitRenderingThread();
			Durin::CollectGarbage();
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
			Durin::FModuleManager::Get().LoadModule("RenderCore");
			Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
			ASSERT_NE(Durin::GDynamicRHI, nullptr);
			Durin::InitRenderingThread();
			bStarted = true;
			RendererLifecycle.Start(Renderer);
			Engine.SetTestRendererModule(&Renderer);
			Durin::GEngine = &Engine;
			struct FBeginThumbnailFrame
			{
				static constexpr auto GetName() -> const char* { return "BeginThumbnailFrame"; }
			};
			Durin::EnqueueRenderCommand<FBeginThumbnailFrame>([](Durin::FRHICommandListImmediate& List) {
				List.SwitchPipeline(Durin::ERHIPipeline::Graphics);
				Durin::GDynamicRHI->RHIBeginFrame_RenderThread(List);
			});
			Contract.Output.Width = 64;
			Contract.Output.Height = 64;
		}

		auto TearDown() -> void override
		{
			if (!bStarted) return;
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			for (const auto& Path : Packages)
			{
				EXPECT_TRUE(Durin::UnloadPackage(Path, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
				EXPECT_TRUE(Durin::Testing::RemoveAssetPackageForTests(Path));
			}
			for (const auto Name : {Durin::Editor::FThumbnailVisualContract::SphereAssetPath, Durin::Editor::FPreviewMeshResources::BoxAssetPath})
			{
				Durin::FObjectPath Path;
				if (Durin::FObjectPath::TryCreate(Name, Path))
					(void)Durin::UnloadPackage(Path.GetPackagePath(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			}
			Durin::CollectGarbage();
			Durin::FPackagePath Studio;
			ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Engine/Renderer/DefaultStudioCube", Studio));
			(void)Durin::UnloadPackage(Studio, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			Durin::CollectGarbage();
			struct FEndThumbnailFrame
			{
				static constexpr auto GetName() -> const char* { return "EndThumbnailFrame"; }
			};
			Durin::EnqueueRenderCommand<FEndThumbnailFrame>([](Durin::FRHICommandListImmediate& List) {
				Durin::GDynamicRHI->RHIEndFrame_RenderThread(List);
			});
			Durin::FlushRenderingCommands();
			Durin::GEngine = nullptr;
			Durin::Editor::Texture::FTexturePreview::ReleaseSharedResources();
			RendererLifecycle.Shutdown();
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
			Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
			Durin::RHIExit();
		}

		auto Package(const char* Name) -> Durin::FPackagePath
		{
			Durin::FPackagePath Path;
			EXPECT_TRUE(Durin::FPackagePath::TryCreate(std::string("/ThumbnailVulkan/") + Name, Path));
			Packages.push_back(Path);
			return Path;
		}
		auto ImportTexture() -> Durin::DTexture2D*
		{
			const auto Source = Durin::Testing::GetTestWorkDirectory() / "Preview.png";
			WriteMaterialTextureFixture(Source);
			const auto Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(
				Source.generic_string(), Package("T_Preview").ToString()
			);
			EXPECT_TRUE(Result) << Result.Message;
			return Result.Asset;
		}
		auto ImportCube() -> Durin::DTextureCube*
		{
			const auto Result = Durin::AssetForge::Builtins::ImportTextureCubeFacesForTest(
				Durin::Tests::GetThumbnailDirectionalCubeFaces(), Package("TC_Preview").ToString()
			);
			EXPECT_TRUE(Result) << Result.Message;
			if (Result.Asset) Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Result.Asset);
			return Result.Asset;
		}
		Durin::Testing::FScopedMountRegistryFixture SavedMountRegistry;
		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> MountRegistry;
		std::vector<Durin::FPackagePath> Packages;
		FMaterialTestEngine Engine;
		Durin::FRendererModule Renderer;
		Durin::FModuleTestHarness RendererLifecycle{"ThumbnailVulkanRenderer"};
		Durin::Editor::FThumbnailVisualContract Contract;
		std::string Error;
		bool bStarted = false;
	};
} // namespace

TEST_F(FThumbnailVulkanTests, ColdGenerationReadsBackOnceAndWarmCacheSkipsRendering)
{
	auto Registration = Durin::Editor::GetDefaultThumbnailManager().RegisterScoped(
		std::make_unique<Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer>(), Error
	);
	ASSERT_TRUE(Registration) << Error;
	const auto StaticMeshFixturePath = Package("SM_Preview");
	const auto StaticMeshMaterialPath = Package("M_Preview");
	Durin::DStaticMesh* StaticMeshFixture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(
		StaticMeshFixturePath, StaticMeshFixture
	)) << Error;
	ASSERT_NE(StaticMeshFixture, nullptr);
	Durin::FStaticMeshDecodedGeometry ImportedMesh;
	ImportedMesh.MaterialSlots.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
	Durin::FStaticMeshImportedMesh& ImportedSection =
		ImportedMesh.Meshes.emplace_back();
	ImportedSection.Name = "ThumbnailTetrahedron";
	ImportedSection.Positions = {
		Durin::FVector3f(-0.6f, -0.5f, -0.4f),
		Durin::FVector3f(0.7f, -0.4f, -0.3f),
		Durin::FVector3f(0.0f, 0.8f, -0.2f),
		Durin::FVector3f(0.1f, 0.0f, 0.9f)
	};
	ImportedSection.Indices = {
		0, 2, 1,
		0, 1, 3,
		1, 2, 3,
		2, 0, 3
	};
	ImportedSection.SourceMaterialIndex = 0;
	const auto SynchronousBuild1 = Durin::BuildStaticMeshSynchronously(
		*StaticMeshFixture, std::move(ImportedMesh));
	ASSERT_TRUE(SynchronousBuild1) << Durin::FormatStaticMeshSynchronousError(SynchronousBuild1.Error);
	Durin::DMaterial* StaticMeshAssetMaterial = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(
		StaticMeshMaterialPath,
		StaticMeshAssetMaterial
	));
	ASSERT_NE(StaticMeshAssetMaterial, nullptr);
	auto Recipe = Durin::Testing::MakePBRMaterialExpressionsForTest();
	// Exercise environment inputs through shader compilation, vertex linkage and thumbnail rendering.
	Durin::TStrongObjectPtr<Durin::DMaterialExpressionWorldPosition> Position(Durin::NewObject<Durin::DMaterialExpressionWorldPosition>(nullptr, Durin::NAME_None));
	Durin::TStrongObjectPtr<Durin::DMaterialExpressionTime> Time(Durin::NewObject<Durin::DMaterialExpressionTime>(nullptr, Durin::NAME_None));
	Position->Id = Durin::FGuid::NewGuid(); Time->Id = Durin::FGuid::NewGuid();
	Recipe.Expressions.emplace_back(Position.Get()); Recipe.Expressions.emplace_back(Time.Get());
	Recipe.Outputs.Emissive = {Position->Id}; Recipe.Outputs.Roughness = {Time->Id};
	ASSERT_TRUE(Recipe.Apply(*StaticMeshAssetMaterial));
	ASSERT_TRUE(StaticMeshAssetMaterial->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.85, 0.12, 0.18)
	));
	ASSERT_TRUE(Durin::SavePackage(StaticMeshAssetMaterial->GetPackage()));
	StaticMeshFixture->SetMaterialSlotDefaultMaterial(0, StaticMeshAssetMaterial);
	ASSERT_TRUE(Durin::SavePackage(StaticMeshFixture->GetPackage()));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation
	));
	const Durin::FAssetCatalogEntry StaticMeshAssetData =
		Durin::FindAssetExact(StaticMeshFixturePath);
	ASSERT_NE(StaticMeshAssetData, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint StaticMeshFingerprint = {
		.AssetPath = Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(
			StaticMeshAssetData->PackagePath
		),
		.PackagePath = StaticMeshAssetData->PackagePath,
		.AssetClassName = StaticMeshAssetData->AssetClassName,
		.PackageFormatVersion = StaticMeshAssetData->FormatVersion,
		.FileSize = static_cast<uint64>(StaticMeshAssetData->FileSize),
		.LastWriteTimeTicks = StaticMeshAssetData->LastWriteTimeTicks
	};
	const std::filesystem::path ThumbnailCacheRoot =
		Durin::Testing::GetTestWorkDirectory() / "StaticMeshRenderedCacheVulkan";
	Durin::Testing::RemoveTestWorkDirectory(ThumbnailCacheRoot);
	ASSERT_EQ(Durin::Mona::GetActiveUIBackend(), nullptr);
	Durin::Tests::FThumbnailTestUIBackend ThumbnailUIBackend;
	Durin::Tests::FScopedActiveUIBackend ThumbnailBackendScope(ThumbnailUIBackend);
	auto PumpCacheToReady = [&](Durin::Editor::FAssetThumbnailPool& Cache) {
		Durin::Editor::FAssetThumbnailView View;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Cache.BeginFrame();
			Cache.Request(
				StaticMeshFingerprint,
				Durin::Editor::EAssetThumbnailPriority::Visible
			);
			View = Cache.Find(StaticMeshFingerprint.AssetPath);
			Cache.EndFrame();
			Durin::FlushRenderingCommands();
			std::this_thread::yield();
			if (View.State == Durin::Editor::EAssetThumbnailState::Ready
				&& View.Texture != nullptr)
				break;
		}
		return View;
	};
	{
		Durin::Editor::FAssetThumbnailPool Cache({}, {.CacheRoot = ThumbnailCacheRoot, .ObjectExtension = ".png"});
		const Durin::Editor::FAssetThumbnailView Ready = PumpCacheToReady(Cache);
		ASSERT_EQ(Ready.State, Durin::Editor::EAssetThumbnailState::Ready)
			<< Ready.Diagnostic;
		ASSERT_NE(Ready.Texture, nullptr);

		const Durin::Editor::FAssetThumbnailPoolStats Stats = Cache.GetStats();
		EXPECT_EQ(Stats.Generation.Loads, 1u);
		EXPECT_EQ(Stats.Generation.Renders, 1u);
		EXPECT_EQ(Stats.Generation.Readbacks, 1u);
		EXPECT_EQ(Stats.Generation.DiskHits, 0u);
		EXPECT_EQ(Stats.PreviewSceneCreations, 1u);
		EXPECT_EQ(Stats.PreviewSceneAssignments, 1u);
		EXPECT_EQ(Stats.UploadsCompleted, 1u);
		EXPECT_EQ(Stats.LiveGpuTextures, 1u);
		EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 1u);
		// Display readiness no longer implies persistent publication. Let the
		// optional save finish before clearing requests and testing a warm reopen.
		const auto SaveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (Cache.GetStats().Generation.CacheWrites == 0
			   && std::chrono::steady_clock::now() < SaveDeadline)
		{
			Cache.BeginFrame();
			Cache.EndFrame();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ASSERT_EQ(Cache.GetStats().Generation.CacheWrites, 1u);
		Cache.Clear();
		EXPECT_EQ(Cache.GetStats().LiveGpuTextures, 0u);
		EXPECT_EQ(ThumbnailUIBackend.NumRegistered(), 0u);
	}
	{
		Durin::Editor::FAssetThumbnailPool WarmCache({}, {.CacheRoot = ThumbnailCacheRoot, .ObjectExtension = ".png"});
		const Durin::Editor::FAssetThumbnailView Ready = PumpCacheToReady(WarmCache);
		ASSERT_EQ(Ready.State, Durin::Editor::EAssetThumbnailState::Ready)
			<< Ready.Diagnostic;
		const Durin::Editor::FAssetThumbnailPoolStats Stats = WarmCache.GetStats();
		EXPECT_EQ(Stats.Generation.DiskHits, 1u);
		EXPECT_EQ(Stats.Generation.Loads, 0u);
		EXPECT_EQ(Stats.Generation.Renders, 0u);
		EXPECT_EQ(Stats.Generation.Readbacks, 0u);
		EXPECT_EQ(Stats.PreviewSceneCreations, 0u);
		EXPECT_EQ(Stats.PreviewSceneAssignments, 0u);
		EXPECT_EQ(Stats.UploadsCompleted, 1u);

		WarmCache.CancelPendingRequests();
		const Durin::Editor::FAssetThumbnailView Retained =
			WarmCache.Find(StaticMeshFingerprint.AssetPath);
		EXPECT_EQ(Retained.State, Durin::Editor::EAssetThumbnailState::Ready);
		EXPECT_EQ(Retained.Texture, Ready.Texture);
		WarmCache.BeginFrame();
		WarmCache.Request(
			StaticMeshFingerprint,
			Durin::Editor::EAssetThumbnailPriority::Visible
		);
		const Durin::Editor::FAssetThumbnailView Revisited =
			WarmCache.Find(StaticMeshFingerprint.AssetPath);
		WarmCache.EndFrame();
		EXPECT_EQ(Revisited.State, Durin::Editor::EAssetThumbnailState::Ready);
		EXPECT_EQ(Revisited.Texture, Ready.Texture);
		const Durin::Editor::FAssetThumbnailPoolStats RevisitedStats =
			WarmCache.GetStats();
		EXPECT_EQ(RevisitedStats.Generation.DiskHits, Stats.Generation.DiskHits);
		EXPECT_EQ(RevisitedStats.UploadsQueued, Stats.UploadsQueued);
		WarmCache.Clear();
	}
}

TEST_F(FThumbnailVulkanTests, CubeSessionRetainsPreparedInputAcrossFailureAndRecovery)
{
	auto* CaptureCube = ImportCube();
	ASSERT_NE(CaptureCube, nullptr);
	const auto CaptureCubePath = Packages.back();
	auto CaptureCubeReference = CaptureCube->GetTextureReferenceRHI();
	// A previously accepted revision stays invalid after a failed publication and recovery.
	Durin::Editor::Texture::DTextureCubeThumbnailRenderer CubeRenderer;
	Durin::Editor::Texture::FTextureCubeThumbnailGenerationInput Input(
		Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(CaptureCubePath)
	);
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	Durin::Tests::FAssetThumbnailTestPool Pool(Contract);
	ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
	auto Session = CubeRenderer.CreateGenerationSession(Request, Input, Error);
	ASSERT_NE(Session, nullptr) << Error;
	const auto Loaded = Session->Load();
	Durin::Editor::FThumbnailRendererSessionUpdate Ready;
	ASSERT_TRUE(WaitForResourcePublication([&] {
		Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
		Ready = Session->PollResources();
		return Ready.State != Durin::Editor::EThumbnailRendererSessionState::WaitingForResources;
	}));
	ASSERT_EQ(Ready.State, Durin::Editor::EThumbnailRendererSessionState::ReadyToRender) << Ready.Diagnostic;
	ASSERT_TRUE(Session->PreparePreview(Pool.GetPreviewScene(), Error)) << Error;
	const auto Snapshot = CaptureCube->GetPublishedTexture();
	ASSERT_NE(Snapshot, nullptr);
	Durin::FlushRenderingCommands();
	ASSERT_TRUE(Session->ValidatePreparedInput(Error)) << Error;
	// Failed publication retires the resource and invalidates the prepared session input.
	Durin::VulkanRHI::ArmVulkanCreateFailure(Durin::VulkanRHI::EVulkanCreateFailurePoint::Image);
	CaptureCube->UpdateResource();
	ASSERT_TRUE(WaitForResourcePublication([&] { return !CaptureCube->IsResourceUpdatePending(); }));
	EXPECT_EQ(CaptureCube->GetResourceUpdateState(), Durin::ETextureResourceUpdateState::Failed);
	EXPECT_EQ(CaptureCube->GetPublishedTexture(), nullptr);
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
	CaptureCube->UpdateResource();
	ASSERT_TRUE(WaitForResourcePublication([&] { return !CaptureCube->IsResourceUpdatePending(); }));
	EXPECT_EQ(CaptureCube->GetResourceUpdateState(), Durin::ETextureResourceUpdateState::Succeeded);
	ASSERT_NE(CaptureCube->GetPublishedTexture(), nullptr);
	EXPECT_EQ(CaptureCube->GetTextureReferenceRHI(), CaptureCubeReference);
	EXPECT_NE(CaptureCube->GetPublishedTexture(), Snapshot);
	EXPECT_EQ(Session->PollResources().State, Durin::Editor::EThumbnailRendererSessionState::ReadyToRender);
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
	Session->ResetPreview();
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
}

TEST_F(FThumbnailVulkanTests, MaterialSessionRetainsPreparedInputAcrossFailureAndRecovery)
{
	auto* Texture = ImportTexture();
	ASSERT_NE(Texture, nullptr);
	const auto StaticMeshMaterialPath = Package("M_Preview");
	Durin::DMaterial* StaticMeshAssetMaterial = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(StaticMeshMaterialPath, StaticMeshAssetMaterial));
	auto Validation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*StaticMeshAssetMaterial);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(StaticMeshAssetMaterial->SetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), Texture
	));
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*StaticMeshAssetMaterial);
	ASSERT_TRUE(Durin::SavePackage(StaticMeshAssetMaterial->GetPackage()));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(Durin::EAssetRegistryScanMode::FullValidation));
	const auto Data = Durin::FindAssetExact(StaticMeshMaterialPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailRequest Request = {
		.Asset = {
			.AssetPath = Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(StaticMeshMaterialPath),
			.PackagePath = Data->PackagePath,
			.AssetClassName = Data->AssetClassName,
			.PackageFormatVersion = Data->FormatVersion,
			.FileSize = static_cast<uint64>(Data->FileSize),
			.LastWriteTimeTicks = Data->LastWriteTimeTicks
		}
	};
	Durin::Editor::Material::DMaterialThumbnailRenderer MaterialRenderer(
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString()
	);
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	ASSERT_TRUE(MaterialRenderer.CaptureGenerationRequest(Request, 1, Captured, Error)) << Error;
	Durin::Tests::FAssetThumbnailTestPool Pool(Contract);
	ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
	auto Session = MaterialRenderer.CreateGenerationSession(Captured, *Captured.Input, Error);
	ASSERT_NE(Session, nullptr);
	const auto Loaded = Session->Load();
	Durin::Editor::FThumbnailRendererSessionUpdate Ready;
	ASSERT_TRUE(WaitForResourcePublication([&] {
		Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
		Ready = Session->PollResources();
		return Ready.State != Durin::Editor::EThumbnailRendererSessionState::WaitingForResources;
	}));
	ASSERT_EQ(Ready.State, Durin::Editor::EThumbnailRendererSessionState::ReadyToRender) << Ready.Diagnostic;
	ASSERT_TRUE(Session->PreparePreview(Pool.GetPreviewScene(), Error)) << Error;
	Durin::FlushRenderingCommands();
	ASSERT_TRUE(Session->ValidatePreparedInput(Error)) << Error;
	const auto Stable = Texture->GetTextureReferenceRHI();
	Durin::VulkanRHI::ArmVulkanCreateFailure(Durin::VulkanRHI::EVulkanCreateFailurePoint::Image);
	Texture->UpdateResource();
	ASSERT_TRUE(WaitForResourcePublication([&] { return !Texture->IsResourceUpdatePending(); }));
	EXPECT_EQ(Texture->GetResourceUpdateState(), Durin::ETextureResourceUpdateState::Failed);
	EXPECT_EQ(Texture->GetPublishedTexture(), nullptr);
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
	Texture->UpdateResource();
	ASSERT_TRUE(WaitForResourcePublication([&] { return !Texture->IsResourceUpdatePending(); }));
	EXPECT_EQ(Texture->GetResourceUpdateState(), Durin::ETextureResourceUpdateState::Succeeded);
	ASSERT_NE(Texture->GetPublishedTexture(), nullptr);
	EXPECT_EQ(Texture->GetTextureReferenceRHI(), Stable);
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
	Session->ResetPreview();
	ASSERT_TRUE(StaticMeshAssetMaterial->SetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), nullptr
	));
	ASSERT_TRUE(Durin::SavePackage(StaticMeshAssetMaterial->GetPackage()));
}

TEST_F(FThumbnailVulkanTests, EnvironmentValidationAndCancellationReleaseReferences)
{
	auto* Texture = ImportTexture();
	ASSERT_NE(Texture, nullptr);
	auto* Cube = ImportCube();
	ASSERT_NE(Cube, nullptr);
	auto CaptureCubeReference = Cube->GetTextureReferenceRHI();
	Durin::Tests::FAssetThumbnailTestPool Pool(Contract);
	ASSERT_TRUE(Pool.IsAvailable()) << Pool.GetDiagnostic();
	Durin::FRHITextureReferenceRef Texture2DReference =
		Texture->GetTextureReferenceRHI();
	const Durin::FViewEnvironmentOverride CubeEnvironment{
		.TextureReference = CaptureCubeReference
	};
	const Durin::FViewEnvironmentOverride Texture2DEnvironment{
		.TextureReference = Texture2DReference
	};
	const uint32 CubeReferenceBaseline =
		CaptureCubeReference->GetRefCount();
	const uint32 Texture2DReferenceBaseline =
		Texture2DReference->GetRefCount();
	ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
	ASSERT_TRUE(Pool.SetView(Error)) << Error;
	EXPECT_EQ(
		CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u
	);
	ASSERT_TRUE(Pool.SetViewEnvironment(Texture2DEnvironment, Error)) << Error;
	EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
	EXPECT_EQ(
		Texture2DReference->GetRefCount(), Texture2DReferenceBaseline + 1u
	);
	Pool.Reset();
	EXPECT_EQ(
		Texture2DReference->GetRefCount(), Texture2DReferenceBaseline
	);
	Pool.Reset();
	EXPECT_EQ(
		Texture2DReference->GetRefCount(), Texture2DReferenceBaseline
	);
	ASSERT_TRUE(Pool.SetView(Error)) << Error;
	ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
	EXPECT_EQ(
		CaptureCubeReference->GetRefCount(), CubeReferenceBaseline + 1u
	);
	Pool.Reset();
	EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);

	Durin::FTextureRHIRef OriginalCubeTarget;

	struct FRestoreThumbnailEnvironment
	{
		static constexpr auto GetName() -> const char*
		{
			return "RestoreThumbnailEnvironment";
		}
	};
	auto RestoreCubeTarget = [&] {
		Durin::EnqueueRenderCommand<FRestoreThumbnailEnvironment>(
			[Reference = CaptureCubeReference, OriginalCubeTarget](
				Durin::FRHICommandListImmediate&
			) {
				Durin::GDynamicRHI->RHIUpdateTextureReference(
					Reference.GetReference(), OriginalCubeTarget.GetReference()
				);
			}
		);
		Durin::FlushRenderingCommands();
	};

	ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
	struct FClearThumbnailEnvironment
	{
		static constexpr auto GetName() -> const char*
		{
			return "ClearThumbnailEnvironment";
		}
	};
	Durin::EnqueueRenderCommand<FClearThumbnailEnvironment>(
		[Reference = CaptureCubeReference, &OriginalCubeTarget](
			Durin::FRHICommandListImmediate&
		) {
			OriginalCubeTarget = Reference->GetReferencedTexture_RenderThread();
			Durin::GDynamicRHI->RHIUpdateTextureReference(
				Reference.GetReference(), nullptr
			);
		}
	);
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(Pool.BeginCapture(Error)) << Error;
	Durin::FlushRenderingCommands();
	Durin::FByteBuffer UnavailableEnvironmentPixels;
	EXPECT_EQ(
		Pool.PollCapture(UnavailableEnvironmentPixels, Error),
		Durin::Editor::EThumbnailCaptureState::Failed
	);
	EXPECT_TRUE(UnavailableEnvironmentPixels.empty());
	EXPECT_NE(Error.find("view environment"), std::string::npos);
	Pool.Reset();
	RestoreCubeTarget();

	ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
	ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
	Durin::FlushRenderingCommands();
	Durin::FByteBuffer RecoveredReadback;
	ASSERT_EQ(
		Pool.PollCapture(RecoveredReadback, Error),
		Durin::Editor::EThumbnailCaptureState::Ready
	) << Error;
	EXPECT_EQ(RecoveredReadback.size(), 64u * 64u * 4u);
	Pool.Reset();

	ASSERT_TRUE(Pool.SetViewEnvironment(Texture2DEnvironment, Error)) << Error;
	ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
	Durin::FlushRenderingCommands();
	Durin::FByteBuffer FailedEnvironmentPixels;
	EXPECT_EQ(
		Pool.PollCapture(FailedEnvironmentPixels, Error),
		Durin::Editor::EThumbnailCaptureState::Failed
	);
	EXPECT_TRUE(FailedEnvironmentPixels.empty());
	EXPECT_NE(Error.find("view environment"), std::string::npos);
	Pool.Reset();

	ASSERT_TRUE(Pool.SetViewEnvironment(CubeEnvironment, Error)) << Error;
	struct FThumbnailCancellationGate
	{
		std::mutex Mutex;
		std::condition_variable Condition;
		bool bEntered = false;
		bool bReleased = false;
	};
	struct FBlockThumbnailCaptureForCancellation
	{
		static constexpr auto GetName() -> const char*
		{
			return "BlockThumbnailCaptureForCancellation";
		}
	};
	const auto Gate = std::make_shared<FThumbnailCancellationGate>();
	Durin::EnqueueRenderCommand<FBlockThumbnailCaptureForCancellation>(
		[Gate](Durin::FRHICommandListImmediate&) {
			std::unique_lock Lock(Gate->Mutex);
			Gate->bEntered = true;
			Gate->Condition.notify_all();
			Gate->Condition.wait(Lock, [&Gate] { return Gate->bReleased; });
		}
	);
	{
		std::unique_lock Lock(Gate->Mutex);
		Gate->Condition.wait(Lock, [&Gate] { return Gate->bEntered; });
	}
	const bool bCancelledCaptureStarted = Pool.BeginCapture(Error);
	Pool.Reset();
	const uint32 QueuedReferenceCount =
		CaptureCubeReference->GetRefCount();
	{
		std::lock_guard Lock(Gate->Mutex);
		Gate->bReleased = true;
	}
	Gate->Condition.notify_all();
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(bCancelledCaptureStarted) << Error;
	EXPECT_GT(QueuedReferenceCount, CubeReferenceBaseline);
	EXPECT_EQ(CaptureCubeReference->GetRefCount(), CubeReferenceBaseline);
	Durin::FByteBuffer CancelledPixels;
	EXPECT_EQ(
		Pool.PollCapture(CancelledPixels, Error),
		Durin::Editor::EThumbnailCaptureState::Idle
	);
	EXPECT_TRUE(CancelledPixels.empty());
	EXPECT_TRUE(Error.empty());
}

TEST_F(FThumbnailVulkanTests, Texture2DThumbnailUsesBuiltNormalAndRejectsReplacedAllocation)
{
	using namespace Durin;
	using namespace Durin::Editor;
	using namespace Durin::Editor::Texture;
	auto* Asset = ImportTexture();
	ASSERT_NE(Asset, nullptr);
	FAssetCompilingManager::Get().FinishCompilationForObject(*Asset);
	Asset->SetSource({});
	Asset->SetBuildSettings(ETextureUsage::Normal, false, 0, ETextureCompressionQuality::Normal,
		ETextureAlphaMipMode::Average, 0.5f);
	// No source art is present: the built BC5 allocation is the only preview input.
	auto Platform = std::make_unique<FTexturePlatformData>();
	Platform->PixelFormat = EPixelFormat::BC5_UNORM;
	FTexture2DMipData Mip;
	Mip.Width = Mip.Height = 4;
	Mip.RowPitch = 16;
	Mip.Pixels.resize(16, std::byte{0});
	Mip.Pixels[0] = Mip.Pixels[1] = Mip.Pixels[8] = Mip.Pixels[9] = std::byte{128};
	Platform->Mips.push_back(std::move(Mip));
	Asset->SetPlatformData(std::move(Platform));
	Asset->UpdateResource();
	ASSERT_TRUE(WaitForResourcePublication([&] { return !Asset->IsResourceUpdatePending(); }));
	ASSERT_TRUE(Asset->HasUsableResource());
	ASSERT_FALSE(Asset->GetSource().IsValid());
	const auto Data = FindAssetExact(Packages.back());
	ASSERT_TRUE(Data);
	FAssetThumbnailRequest Request;
	Request.Asset = {
		.AssetPath = Testing::MakePackageLeafTopLevelAssetPathForTests(Packages.back()),
		.PackagePath = Packages.back(), .AssetClassName = Data->AssetClassName,
		.PackageFormatVersion = Data->FormatVersion, .FileSize = static_cast<uint64>(Data->FileSize),
		.LastWriteTimeTicks = Data->LastWriteTimeTicks};
	DTextureThumbnailRenderer Renderer;
	FAssetThumbnailGenerationRequest Captured;
	ASSERT_TRUE(Renderer.CaptureGenerationRequest(Request, 1, Captured, Error)) << Error;
	ASSERT_EQ(Captured.GeneratedPixels, nullptr);
	ASSERT_NE(Captured.Input, nullptr);
	auto Session = Renderer.CreateGenerationSession(Captured, *Captured.Input, Error);
	ASSERT_NE(Session, nullptr);
	const auto Loaded = Session->Load();
	ASSERT_EQ(Loaded.State, EThumbnailRendererSessionState::ReadyToRender) << Loaded.Diagnostic;
	FThumbnailPreviewScenePool Pool(Contract);
	ASSERT_TRUE(Session->PreparePreview(Pool, Error)) << Error;
	ASSERT_TRUE(Session->ValidatePreparedInput(Error)) << Error;
	ASSERT_TRUE(Pool.BeginCapture(Error)) << Error;
	FlushRenderingCommands();
	FByteBuffer Pixels;
	EThumbnailCaptureState CaptureState{};
	ASSERT_TRUE(WaitForResourcePublication([&] {
		CaptureState = Pool.PollCapture(Pixels, Error);
		return CaptureState == EThumbnailCaptureState::Ready || CaptureState == EThumbnailCaptureState::Failed;
	}));
	ASSERT_EQ(CaptureState, EThumbnailCaptureState::Ready) << Error;
	ASSERT_EQ(Pixels.size(), 64u * 64u * 4u);
	const size_t Center = (32 * 64 + 32) * 4;
	EXPECT_NEAR(std::to_integer<int>(Pixels[Center]), 128, 1);
	EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 1]), 128, 1);
	EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 2]), 255, 1);
	EXPECT_EQ(Pixels[Center + 3], std::byte{255});
	Pool.Reset();
	// Exercise the complete cold/warm scheduler path, not only the renderer session.
	auto Registration = GetDefaultThumbnailManager().RegisterScoped(
		std::make_unique<DTextureThumbnailRenderer>(), Error);
	ASSERT_TRUE(Registration) << Error;
	Tests::FThumbnailTestUIBackend Backend;
	Tests::FScopedActiveUIBackend BackendScope(Backend);
	const auto CacheRoot = Testing::GetTestWorkDirectory() / "BuiltTextureThumbnailCache";
	for (const bool Warm : {false, true})
	{
		FAssetThumbnailPool Cache({}, {.CacheRoot = CacheRoot, .ObjectExtension = ".png"});
		FAssetThumbnailView View;
		ASSERT_TRUE(WaitForResourcePublication([&] {
			Cache.BeginFrame();
			Cache.Request(Request.Asset, EAssetThumbnailPriority::Visible);
			View = Cache.Find(Request.Asset.AssetPath);
			Cache.EndFrame();
			return View.State == EAssetThumbnailState::Ready || View.State == EAssetThumbnailState::Failed;
		}));
		ASSERT_EQ(View.State, EAssetThumbnailState::Ready) << View.Diagnostic;
		ASSERT_NE(View.Texture, nullptr);
		const auto Stats = Cache.GetStats().Generation;
		EXPECT_EQ(Stats.Renders, Warm ? 0u : 1u);
		EXPECT_EQ(Stats.Readbacks, Warm ? 0u : 1u);
		EXPECT_EQ(Stats.Loads, Warm ? 0u : 1u);
		EXPECT_EQ(Stats.DiskHits, Warm ? 1u : 0u);
		if (!Warm)
			ASSERT_TRUE(WaitForResourcePublication([&] {
				Cache.BeginFrame(); Cache.EndFrame();
				return Cache.GetStats().Generation.CacheWrites == 1;
			}));
	}

	Asset->UpdateResource();
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
	ASSERT_TRUE(WaitForResourcePublication([&] { return !Asset->IsResourceUpdatePending(); }));
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
	Session->ResetPreview();
	EXPECT_FALSE(Session->ValidatePreparedInput(Error));
}

TEST_F(FThumbnailVulkanTests, TexturePreviewPreservesRawChannelsSrgbAndAspectRatio)
{
	using namespace Durin;
	using namespace Durin::Editor;
	using namespace Durin::Editor::Texture;
	FTextureRHIRef Input;
	ENQUEUE_RENDER_COMMAND(CreatePreviewTestInput)([&Input](FRHICommandListImmediate& Commands) {
		auto Desc = FRHITextureCreateDesc::Create2D("PreviewTestInput", 2, 1, EPixelFormat::SRGBA8_UNORM);
		Desc.AddFlags(ETextureCreateFlags::ShaderResource);
		Input = GDynamicRHI->RHICreateTexture(Commands, Desc);
		const FByteBuffer Pixels{std::byte{128}, std::byte{64}, std::byte{32}, std::byte{128},
			std::byte{128}, std::byte{64}, std::byte{32}, std::byte{128}};
		if (Input) GDynamicRHI->RHIUpdateTexture2D(Commands, Input, 0, 0,
			FUpdateTextureRegion2D(0, 0, 0, 0, 2, 1), 8, Pixels);
	});
	FlushRenderingCommands();
	ASSERT_NE(Input, nullptr);
	FThumbnailPreviewScenePool Pool(Contract);
	for (const auto Channel : {ETexturePreviewChannel::RGBA, ETexturePreviewChannel::Blue, ETexturePreviewChannel::Alpha})
	{
		ASSERT_TRUE(Pool.SetImageRenderer([Input, Channel](FRHICommandListImmediate& Commands, uint32 W, uint32 H) {
			return RenderTexturePreview(Commands, Input, W, H,
				{.Usage = ETextureUsage::Normal, .Interpretation = Channel == ETexturePreviewChannel::RGBA
					? ETexturePreviewInterpretation::Raw : ETexturePreviewInterpretation::Auto, .Channel = Channel});
		}, Error));
		ASSERT_TRUE(Pool.BeginCapture(Error));
		FlushRenderingCommands();
		FByteBuffer Pixels;
		EThumbnailCaptureState CaptureState{};
		ASSERT_TRUE(WaitForResourcePublication([&] {
			CaptureState = Pool.PollCapture(Pixels, Error);
			return CaptureState == EThumbnailCaptureState::Ready || CaptureState == EThumbnailCaptureState::Failed;
		}));
		ASSERT_EQ(CaptureState, EThumbnailCaptureState::Ready) << Error;
		ASSERT_EQ(Pixels.size(), 64u * 64u * 4u);
		EXPECT_EQ(Pixels[3], std::byte{0}); // Aspect-fit margin stays transparent.
		const size_t Center = (32 * 64 + 32) * 4;
		const int Expected = Channel == ETexturePreviewChannel::Blue ? 32 : 128;
		EXPECT_NEAR(std::to_integer<int>(Pixels[Center]), Expected, 1);
		EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 1]), Channel == ETexturePreviewChannel::RGBA ? 64 : Expected, 1);
		EXPECT_NEAR(std::to_integer<int>(Pixels[Center + 2]), Channel == ETexturePreviewChannel::RGBA ? 32 : Expected, 1);
		EXPECT_EQ(Pixels[Center + 3], Channel == ETexturePreviewChannel::RGBA ? std::byte{128} : std::byte{255});
		Pool.Reset();
	}
}
