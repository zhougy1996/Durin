#include "Animation/AnimationClip.h"
#include "Editor/WorkspaceManager.h"
#include "Engine/Actor.h"
#include "EngineTestSupport.h"
#include "Materials/MaterialTestSupport.h"
#include "NativeTestSupport.h"
#include "Preview/OrbitAssetPreview.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "SkeletalMeshEditorModule.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Widgets/SkeletalAssetPreview.h"

#include <gtest/gtest.h>

TEST(FSkeletalMeshEditorTests, PreviewControllerFramesAndNavigatesDeterministically)
{
	Durin::Editor::FOrbitAssetPreviewController Controller;
	const Durin::FBox Bounds(
		Durin::FVector3(-2.0, -1.0, 3.0), Durin::FVector3(6.0, 5.0, 11.0));
	Controller.FrameBounds(Bounds);
	const Durin::FVector3 InitialTarget = Controller.GetTarget();
	const double InitialDistance = Controller.GetDistance();
	EXPECT_EQ(InitialTarget, Durin::FVector3(2.0, 2.0, 7.0));
	Controller.Orbit(20.0f, 10.0f);
	EXPECT_DOUBLE_EQ(Controller.GetYawDegrees(), -40.0);
	EXPECT_DOUBLE_EQ(Controller.GetPitchDegrees(), 27.5);
	Controller.Zoom(1.0f);
	EXPECT_LT(Controller.GetDistance(), InitialDistance);
	Controller.Pan(12.0f, -8.0f);
	EXPECT_NE(Controller.GetTarget(), InitialTarget);
	Controller.Reset();
	EXPECT_EQ(Controller.GetTarget(), InitialTarget);
	EXPECT_DOUBLE_EQ(Controller.GetDistance(), InitialDistance);
}

TEST(FSkeletalMeshEditorTests, RegistrationIsExactReadOnlyAndScoped)
{
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	Durin::FSkeletalMeshEditorModule Module;
	ASSERT_TRUE(Module.RegisterSkeletalMeshEditor(Manager, ThumbnailManager));
	EXPECT_FALSE(Module.RegisterSkeletalMeshEditor(Manager, ThumbnailManager));
	auto Workspace = Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("SkeletalMeshEditor"));
	ASSERT_NE(Workspace, nullptr);
	EXPECT_FALSE(Workspace->CanSaveActiveDocument());
	EXPECT_FALSE(Workspace->SaveActiveDocument());
	EXPECT_EQ(Manager.GetWorkspaceDescriptors().front().DisplayName, "Skeletal Asset Inspector");
	EXPECT_TRUE(ThumbnailManager.Find(
		Durin::DSkeletalMesh::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DSkeleton::StaticClass()->GetQualifiedName().ToString()));

	const std::array Classes{
		Durin::DSkeleton::StaticClass()->GetQualifiedName().ToString(),
		Durin::DSkeletalMesh::StaticClass()->GetQualifiedName().ToString(),
		Durin::DAnimationClip::StaticClass()->GetQualifiedName().ToString()};
	for (const std::string& ClassName : Classes)
		EXPECT_FALSE(Manager.OpenAsset("/Missing/SkeletalAsset", ClassName));

	Module.UnregisterSkeletalMeshEditor();
	EXPECT_EQ(Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("SkeletalMeshEditor")), nullptr);
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DSkeletalMesh::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(Module.RegisterSkeletalMeshEditor(Manager, ThumbnailManager));
}

TEST(FSkeletalMeshEditorTests, PreviewSceneOwnsAndReleasesProductionComponents)
{
	FMaterialPreviewHarness Harness;
	constexpr uint64 PreviewId = 97531;
	const std::string ActorName = std::format("SkeletalAssetPreviewActor_{}", PreviewId);
	const std::string LightName = std::format("SkeletalAssetPreviewLightActor_{}", PreviewId);
	{
		Durin::Editor::SkeletalMesh::FSkeletalAssetPreview Preview(PreviewId);
		auto* PreviewActor = Durin::Cast<Durin::AActor>(FindObjectByName(ActorName));
		ASSERT_NE(PreviewActor, nullptr);
		EXPECT_TRUE(PreviewActor->IsActorTickEnabled());
		EXPECT_NE(FindObjectByName(LightName), nullptr);
		EXPECT_TRUE(Preview.IsLit());
		EXPECT_FALSE(Preview.IsWireframe());
		Preview.SetLit(false);
		Preview.SetWireframe(true);
		EXPECT_FALSE(Preview.IsLit());
		EXPECT_TRUE(Preview.IsWireframe());
		Preview.SetVisible(false);
	}
	Durin::CollectGarbage();
	EXPECT_EQ(FindObjectByName(ActorName), nullptr);
	EXPECT_EQ(FindObjectByName(LightName), nullptr);
}
