#include "Editor/WorkspaceManager.h"
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
	Durin::Editor::FOrbitAssetPreviewController Controller;
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
	Controller.ApplyInput(Durin::Editor::FAssetPreviewViewportInput{
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

TEST(FStaticMeshEditorTests, RegistrationIsExactReadOnlyAndScoped)
{
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	Durin::FStaticMeshEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("StaticMeshEditor");
	ModuleHarness.Start(Module);
	ASSERT_TRUE(Module.RegisterStaticMeshEditor(Manager, ThumbnailManager));
	EXPECT_FALSE(Module.RegisterStaticMeshEditor(Manager, ThumbnailManager));
	EXPECT_TRUE(ThumbnailManager.Find(
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString()));

	auto Workspace = Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("StaticMeshEditor"));
	ASSERT_NE(Workspace, nullptr);
	EXPECT_FALSE(Workspace->CanSaveActiveDocument());
	EXPECT_FALSE(Workspace->SaveActiveDocument());
	ASSERT_EQ(Manager.GetWorkspaceDescriptors().size(), 1u);
	EXPECT_EQ(Manager.GetWorkspaceDescriptors().front().DisplayName, "StaticMesh Inspector");

	Module.UnregisterStaticMeshEditor();
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("StaticMeshEditor")), nullptr);
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(Module.RegisterStaticMeshEditor(Manager, ThumbnailManager));
	Workspace.reset();
	ModuleHarness.Shutdown();
}

TEST(FStaticMeshEditorTests, PreviewSceneOwnsAndReleasesItsViewportComponents)
{
	FMaterialPreviewHarness Harness;
	constexpr uint64 PreviewId = 246813579;
	const std::string ActorName = std::format("StaticMeshPreviewActor_{}", PreviewId);
	const std::string LightName = std::format("StaticMeshPreviewLightActor_{}", PreviewId);
	{
		Durin::Editor::StaticMesh::FStaticMeshPreview Preview(PreviewId);
		EXPECT_NE(FindObjectByName(ActorName), nullptr);
		EXPECT_NE(FindObjectByName(LightName), nullptr);
		EXPECT_FALSE(Preview.IsWireframe());
		Preview.SetWireframe(true);
		EXPECT_TRUE(Preview.IsWireframe());
		Preview.SetVisible(false);
		Preview.ResetView();
	}
	Durin::CollectGarbage();
	EXPECT_EQ(FindObjectByName(ActorName), nullptr);
	EXPECT_EQ(FindObjectByName(LightName), nullptr);
}

TEST(FStaticMeshEditorTests, ThumbnailConflictRollsBackWorkspaceRegistration)
{
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	std::string Error;
	auto Existing = ThumbnailManager.RegisterScoped(
		std::make_unique<Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer>(), Error);
	ASSERT_TRUE(Existing) << Error;
	const std::string ClassName =
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	const uint64 ExistingGeneration = ThumbnailManager.Find(ClassName).Generation;

	Durin::FStaticMeshEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("StaticMeshEditor");
	ModuleHarness.Start(Module);
	EXPECT_FALSE(Module.RegisterStaticMeshEditor(Manager, ThumbnailManager));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("StaticMeshEditor")),
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

	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	Durin::FStaticMeshEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("StaticMeshEditor");
	ModuleHarness.Start(Module);
	ASSERT_TRUE(Module.RegisterStaticMeshEditor(Manager, ThumbnailManager));
	const std::string ClassName = Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	const std::string FirstPath = "/StaticMeshEditorTests/First.First";
	const std::string SecondPath = "/StaticMeshEditorTests/Second.Second";
	ASSERT_TRUE(Manager.OpenAsset(FirstPath, ClassName));
	ASSERT_TRUE(Manager.OpenAsset(FirstPath, ClassName));
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	ASSERT_TRUE(Manager.OpenAsset(SecondPath, ClassName));
	ASSERT_EQ(Manager.GetDocuments().size(), 2u);
	ASSERT_NE(Manager.GetActiveDocument(), nullptr);
	EXPECT_EQ(Manager.GetActiveDocument()->ResourceId, SecondPath);
	const Durin::Editor::FDocumentId ActiveBeforeUnsupported = Manager.GetActiveDocument()->Id;
	EXPECT_FALSE(Manager.OpenAsset(FirstPath, "Durin::DTexture2D"));
	ASSERT_NE(Manager.GetActiveDocument(), nullptr);
	EXPECT_EQ(Manager.GetActiveDocument()->Id, ActiveBeforeUnsupported);

	const Durin::Editor::FDocumentId FirstId = Manager.GetDocuments().front().Id;
	EXPECT_EQ(Manager.RequestCloseDocument(FirstId), Durin::Editor::EDocumentCloseResult::Closed);
	EXPECT_EQ(Manager.GetDocuments().size(), 1u);
	ASSERT_TRUE(Manager.OpenAsset(FirstPath, ClassName));
	ASSERT_EQ(Manager.GetDocuments().size(), 2u);
	auto Workspace = Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("StaticMeshEditor"));
	ASSERT_NE(Workspace, nullptr);
	for (const Durin::Editor::FDocumentTab& Document : Manager.GetDocuments())
		EXPECT_FALSE(Workspace->IsDocumentDirty(Document));
	while (!Manager.GetDocuments().empty())
		ASSERT_EQ(Manager.RequestCloseDocument(Manager.GetDocuments().front().Id),
			Durin::Editor::EDocumentCloseResult::Closed);
	Workspace.reset();
	ModuleHarness.Shutdown();
}
