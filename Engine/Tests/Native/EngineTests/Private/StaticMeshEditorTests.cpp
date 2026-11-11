#include "Editor/AssetEditorTestHost.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Documents/EditorDocumentManager.h"
#include "EngineTestSupport.h"
#include "Materials/MaterialTestSupport.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "Preview/OrbitAssetPreview.h"
#include "StaticMesh/StaticMesh.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "StaticMeshEditorModule.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Widgets/StaticMeshPreview.h"

#include <gtest/gtest.h>

TEST(FStaticMeshEditorTests, PreviewControllerFramesAndNavigatesDeterministically)
{
	Durin::FOrbitAssetPreviewController Controller;
	const Durin::FBox Bounds(Durin::FVector3(-2.0, -1.0, 3.0), Durin::FVector3(6.0, 5.0, 11.0));
	Controller.FrameBounds(Bounds);
	const Durin::FVector3 InitialTarget = Controller.GetTarget();
	const double InitialDistance = Controller.GetDistance();
	EXPECT_DOUBLE_EQ(InitialTarget.x, 2.0);
	EXPECT_DOUBLE_EQ(InitialTarget.y, 2.0);
	EXPECT_DOUBLE_EQ(InitialTarget.z, 7.0);
	EXPECT_GT(InitialDistance, 4.0);

	Controller.Orbit(20.0f, 0.0f);
	EXPECT_DOUBLE_EQ(Controller.GetYawDegrees(), -40.0);
	EXPECT_DOUBLE_EQ(Controller.GetPitchDegrees(), 25.0);
	Controller.Orbit(0.0f, 10.0f);
	EXPECT_DOUBLE_EQ(Controller.GetYawDegrees(), -40.0);
	EXPECT_DOUBLE_EQ(Controller.GetPitchDegrees(), 27.5);
	Controller.Orbit(0.0f, 1000.0f);
	EXPECT_DOUBLE_EQ(Controller.GetPitchDegrees(), 85.0);
	Controller.ApplyInput(Durin::FAssetPreviewViewportInput{
		.MouseWheel = 1.0f});
	EXPECT_LT(Controller.GetDistance(), InitialDistance);
	Controller.Pan(12.0f, -8.0f);
	EXPECT_NE(Controller.GetTarget(), InitialTarget);

	Controller.Reset();
	EXPECT_EQ(Controller.GetTarget(), InitialTarget);
	EXPECT_DOUBLE_EQ(Controller.GetDistance(), InitialDistance);
	EXPECT_DOUBLE_EQ(Controller.GetYawDegrees(), -45.0);
	EXPECT_DOUBLE_EQ(Controller.GetPitchDegrees(), 25.0);
}

TEST(FStaticMeshEditorTests, ModulePublishesMetadataAndRendererWithoutCreatingASharedToolkit)
{
	InitializeDObjectSystem();
	Durin::FEditorDocumentManager Manager;
	auto& Thumbnails = Durin::GetDefaultThumbnailManager();
	Durin::FStaticMeshEditorModule Module;
	Durin::FModuleTestHarness Harness("StaticMeshEditor");
	Harness.Start(Module);
	const auto Class = Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	EXPECT_TRUE(Thumbnails.Find(Class));
	const auto* Definition = Durin::FAssetDefinitionRegistry::Get().FindDefinition(Class);
	ASSERT_NE(nullptr, Definition);
	EXPECT_EQ("StaticMesh", Definition->GetAssetDisplayName());
	EXPECT_TRUE(!Manager.GetToolkit());
	Harness.Shutdown();
	EXPECT_FALSE(Thumbnails.Find(Class));
}


TEST(FStaticMeshEditorTests, PreviewSceneOwnsAndReleasesItsViewportComponents)
{
	FMaterialPreviewHarness Harness;
	constexpr uint64 PreviewId = 246813579;
	const std::string ActorName = std::format("StaticMeshPreviewActor_{}", PreviewId);
	const std::string LightName = std::format("StaticMeshPreviewLightActor_{}", PreviewId);
	{
		Durin::StaticMeshEditor::FStaticMeshPreview Preview(PreviewId);
		EXPECT_NE(FindObjectByName(ActorName), nullptr);
		EXPECT_NE(FindObjectByName(LightName), nullptr);
		EXPECT_FALSE(Preview.IsWireframe());
		Preview.SetWireframe(true);
		EXPECT_TRUE(Preview.IsWireframe());
		Preview.SetVisible(false);
		Preview.ResetView();
	}
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(FindObjectByName(ActorName), nullptr);
	EXPECT_EQ(FindObjectByName(LightName), nullptr);
}

TEST(FStaticMeshEditorTests, ThumbnailConflictDoesNotAffectEditorDiscovery)
{
	Durin::FEditorDocumentManager Manager;
	Durin::DThumbnailManager ThumbnailManager;
	std::string Error;
	auto Existing = ThumbnailManager.RegisterScoped(
		std::make_unique<Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer>());
	ASSERT_TRUE(Existing) << Existing.error();
	const std::string ClassName =
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	const uint64 ExistingGeneration = ThumbnailManager.Find(ClassName).Generation;

	Durin::FStaticMeshEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("StaticMeshEditor");
	ModuleHarness.Start(Module);
	EXPECT_FALSE(ThumbnailManager.RegisterScoped(std::make_unique<Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer>()));
	EXPECT_NE(nullptr, Durin::FAssetDefinitionRegistry::Get().FindDefinition(ClassName));
	EXPECT_EQ(
		Manager.FindToolkit(Durin::FAssetEditorToolkitId("StaticMeshEditor")),
		nullptr);
	EXPECT_EQ(ThumbnailManager.Find(ClassName).Generation, ExistingGeneration);
	ModuleHarness.Shutdown();
}

TEST(FStaticMeshEditorTests, ReusesOneDocumentPerMeshAndSupportsCloseReopen)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshEditorDocuments";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root / "Content");
	Durin::Testing::RegisterMountPointForTests(
		"/StaticMeshEditorTests/", (Root / "Content").generic_string() + "/");
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	const Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> First = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Source.generic_string(), "/StaticMeshEditorTests/First");
	const Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Second = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Source.generic_string(), "/StaticMeshEditorTests/Second");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_TRUE(Second) << Second.Message;

	Durin::FStaticMeshEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("StaticMeshEditor");
	ModuleHarness.Start(Module);
	Durin::Testing::FScopedAssetEditorTestHost TestHost;
	auto* Service = TestHost.GetService();
	ASSERT_NE(nullptr, Service);
	const std::string ClassName = Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	const std::string FirstPath = "/StaticMeshEditorTests/First.First";
	const std::string SecondPath = "/StaticMeshEditorTests/Second.Second";
	ASSERT_TRUE(TestHost.Open(FirstPath, ClassName));
	ASSERT_TRUE(TestHost.Open(FirstPath, ClassName));
	ASSERT_EQ(1u, Service->GetOwnedAssetEditors().size());
	auto* FirstSession = Service->GetOwnedAssetEditors().front().Get();
	ASSERT_TRUE(TestHost.Open(SecondPath, ClassName));
	ASSERT_EQ(2u, Service->GetOwnedAssetEditors().size());
	const auto ActiveBeforeUnsupported = Service->GetActiveAssetToolkit();
	EXPECT_EQ(SecondPath, ActiveBeforeUnsupported->GetOwningAssetEditor()->GetDocumentManager()->GetActiveDocument()->ResourceId);
	EXPECT_FALSE(TestHost.Open(FirstPath, "Durin::DTexture2D"));
	EXPECT_EQ(ActiveBeforeUnsupported, Service->GetActiveAssetToolkit());
	EXPECT_TRUE(FirstSession->CloseWindow(Durin::EAssetEditorCloseReason::AssetEditorHostClosed));
	EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
	ASSERT_TRUE(TestHost.Open(FirstPath, ClassName));
	EXPECT_EQ(2u, Service->GetOwnedAssetEditors().size());
	for (const auto& Session : Service->GetOwnedAssetEditors())
		for (const auto& Document : Session->GetDocumentManager()->GetDocuments())
			EXPECT_FALSE(Session->GetToolkit()->IsDocumentDirty(Document));
	EXPECT_TRUE(Service->CloseAllAssetEditors());
	EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
	TestHost.Shutdown();
	ModuleHarness.Shutdown();
}
