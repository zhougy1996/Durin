#include "Editor/EditorWorkspace.h"
#include "EngineTestSupport.h"
#include "Materials/MaterialTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMeshEditorModule.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Widgets/StaticMeshPreview.h"

#include <gtest/gtest.h>

TEST(FStaticMeshEditorTests, PreviewControllerFramesAndNavigatesDeterministically)
{
	Durin::FStaticMeshPreviewController Controller;
	const Durin::FBox Bounds(Durin::FVector3(-2.0, -1.0, 3.0), Durin::FVector3(6.0, 5.0, 11.0));
	Controller.FrameBounds(Bounds);
	const Durin::FVector3 InitialTarget = Controller.GetTarget();
	const double InitialDistance = Controller.GetDistance();
	EXPECT_DOUBLE_EQ(InitialTarget.x, 2.0);
	EXPECT_DOUBLE_EQ(InitialTarget.y, 2.0);
	EXPECT_DOUBLE_EQ(InitialTarget.z, 7.0);
	EXPECT_GT(InitialDistance, 4.0);

	Controller.Orbit(20.0f, -1000.0f);
	EXPECT_GE(Controller.GetPitchDegrees(), -85.0);
	EXPECT_NE(Controller.GetYawDegrees(), -45.0);
	Controller.Zoom(1.0f);
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
	Durin::FEditorWorkspaceManager Manager;
	Durin::FRenderedAssetThumbnailService ThumbnailService;
	Durin::FStaticMeshEditorModule Module;
	ASSERT_TRUE(Module.RegisterStaticMeshEditor(Manager, ThumbnailService));
	EXPECT_FALSE(Module.RegisterStaticMeshEditor(Manager, ThumbnailService));
	EXPECT_TRUE(ThumbnailService.Find(
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString()));

	auto Workspace = Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("StaticMeshEditor"));
	ASSERT_NE(Workspace, nullptr);
	EXPECT_FALSE(Workspace->CanSaveActiveDocument());
	EXPECT_FALSE(Workspace->SaveActiveDocument());
	ASSERT_EQ(Manager.GetWorkspaceDescriptors().size(), 1u);
	EXPECT_EQ(Manager.GetWorkspaceDescriptors().front().DisplayName, "StaticMesh Inspector");

	Module.UnregisterStaticMeshEditor();
	EXPECT_EQ(Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("StaticMeshEditor")), nullptr);
	EXPECT_FALSE(ThumbnailService.Find(
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(Module.RegisterStaticMeshEditor(Manager, ThumbnailService));
}

TEST(FStaticMeshEditorTests, PreviewSceneOwnsAndReleasesItsViewportComponents)
{
	FMaterialPreviewHarness Harness;
	constexpr Durin::uint64 PreviewId = 246813579;
	const std::string ActorName = std::format("StaticMeshPreviewActor_{}", PreviewId);
	const std::string LightName = std::format("StaticMeshPreviewLightActor_{}", PreviewId);
	{
		Durin::FStaticMeshPreview Preview(PreviewId);
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
	Durin::FEditorWorkspaceManager Manager;
	Durin::FRenderedAssetThumbnailService ThumbnailService;
	std::string Error;
	auto Existing = ThumbnailService.RegisterScoped(
		std::make_unique<Durin::FStaticMeshAssetThumbnailProvider>(), Error);
	ASSERT_TRUE(Existing) << Error;
	const std::string ClassName =
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	const Durin::uint64 ExistingGeneration = ThumbnailService.Find(ClassName).Generation;

	Durin::FStaticMeshEditorModule Module;
	EXPECT_FALSE(Module.RegisterStaticMeshEditor(Manager, ThumbnailService));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("StaticMeshEditor")),
		nullptr);
	EXPECT_EQ(ThumbnailService.Find(ClassName).Generation, ExistingGeneration);
}

TEST(FStaticMeshEditorTests, ReusesOneDocumentPerMeshAndSupportsCloseReopen)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshEditorDocuments";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root / "Content");
	Durin::PathUtilities::RegisterMountPointForTests(
		"/StaticMeshEditorTests/", (Root / "Content").generic_string() + "/");
	const std::filesystem::path Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
	const Durin::FStaticMeshImportResult First = Durin::DStaticMesh::ImportAsset(
		Source.generic_string(), "/StaticMeshEditorTests/First");
	const Durin::FStaticMeshImportResult Second = Durin::DStaticMesh::ImportAsset(
		Source.generic_string(), "/StaticMeshEditorTests/Second");
	ASSERT_TRUE(First) << First.Message;
	ASSERT_TRUE(Second) << Second.Message;

	Durin::FEditorWorkspaceManager Manager;
	Durin::FRenderedAssetThumbnailService ThumbnailService;
	Durin::FStaticMeshEditorModule Module;
	ASSERT_TRUE(Module.RegisterStaticMeshEditor(Manager, ThumbnailService));
	const std::string ClassName = Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	ASSERT_TRUE(Manager.OpenAsset("/StaticMeshEditorTests/First", ClassName));
	ASSERT_TRUE(Manager.OpenAsset("/StaticMeshEditorTests/First", ClassName));
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	ASSERT_TRUE(Manager.OpenAsset("/StaticMeshEditorTests/Second", ClassName));
	ASSERT_EQ(Manager.GetDocuments().size(), 2u);
	ASSERT_NE(Manager.GetActiveDocument(), nullptr);
	EXPECT_EQ(Manager.GetActiveDocument()->ResourceId, "/StaticMeshEditorTests/Second");
	const Durin::FEditorDocumentId ActiveBeforeUnsupported = Manager.GetActiveDocument()->Id;
	EXPECT_FALSE(Manager.OpenAsset("/StaticMeshEditorTests/First", "Durin::DTexture2D"));
	ASSERT_NE(Manager.GetActiveDocument(), nullptr);
	EXPECT_EQ(Manager.GetActiveDocument()->Id, ActiveBeforeUnsupported);

	const Durin::FEditorDocumentId FirstId = Manager.GetDocuments().front().Id;
	EXPECT_EQ(Manager.RequestCloseDocument(FirstId), Durin::EEditorDocumentCloseResult::Closed);
	EXPECT_EQ(Manager.GetDocuments().size(), 1u);
	ASSERT_TRUE(Manager.OpenAsset("/StaticMeshEditorTests/First", ClassName));
	ASSERT_EQ(Manager.GetDocuments().size(), 2u);
	auto Workspace = Manager.FindWorkspace(Durin::FEditorWorkspaceTypeId("StaticMeshEditor"));
	ASSERT_NE(Workspace, nullptr);
	for (const Durin::FEditorDocumentTab& Document : Manager.GetDocuments())
		EXPECT_FALSE(Workspace->IsDocumentDirty(Document));
}
