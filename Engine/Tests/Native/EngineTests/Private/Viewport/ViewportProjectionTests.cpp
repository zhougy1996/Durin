#include "ViewportTestSupport.h"
#include "Math/Operations.h"

TEST(FLevelEditorViewportClientTests, NavigationDoesNotDirtyTheLevelPackage)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "ViewportLevels";
	static std::unordered_set<std::filesystem::path> InitializedRoots;
	if (InitializedRoots.insert(Root).second)
	{
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPointForTests("/ViewportTests/", Root.generic_string() + "/");
	}
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/ViewportTests/NavigationDirty", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Level));
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	ASSERT_FALSE(Level->GetPackage()->IsDirty());

	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bFocused = true;
	Input.bHovered = true;
	Input.bRightMousePressed = true;
	Input.bRightMouseDown = true;
	Input.MouseDelta = {20.0f, -10.0f};
	Client.Update(Level, nullptr, Input);
	EXPECT_FALSE(Level->GetPackage()->IsDirty());
	EXPECT_TRUE(Durin::Asset::UnloadPackage(Path));
}

TEST(FBoxTests, AccumulatesFinitePointsAndResets)
{
	Durin::FBox Box;
	EXPECT_FALSE(Box.bIsValid);
	Box.AddPoint({2.0, -1.0, 4.0});
	Box.AddPoint({-3.0, 5.0, 1.0});
	ASSERT_TRUE(Box.bIsValid);
	ExpectVectorNear(Box.Min, {-3.0, -1.0, 1.0});
	ExpectVectorNear(Box.Max, {2.0, 5.0, 4.0});
	ExpectVectorNear(Box.GetCenter(), {-0.5, 2.0, 2.5});
	Box.Reset();
	EXPECT_FALSE(Box.bIsValid);
}

TEST(FLevelEditorViewportClientTests, BuildsCenterPickingRayAndRejectsInvalidViewport)
{
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FVector3 Origin;
	Durin::FVector3 Direction;
	EXPECT_FALSE(Client.BuildPickingRay({0.0f, 0.0f}, {0.0f, 100.0f}, Origin, Direction));
	ASSERT_TRUE(Client.BuildPickingRay({400.0f, 300.0f}, {800.0f, 600.0f}, Origin, Direction));
	EXPECT_NEAR(Durin::Math::Length(Direction), 1.0, 1.e-8);
	ExpectVectorNear(Direction, Client.GetCameraTransform().GetForwardVector(), 1.e-6);
}

TEST(FLevelEditorViewportClientTests, RestoresDocumentedViewDistanceDefaults)
{
	using FViewportClient = Durin::Editor::Level::FLevelEditorViewportClient;
	FViewportClient Client;
	Client.SetClipDistances(0.001f, 10000.0f);
	Client.SetTerrainDistance(100.0f, 1000.0f);
	Client.ResetViewDistances();

	EXPECT_FLOAT_EQ(Client.GetNearClip(), FViewportClient::DefaultNearClip);
	EXPECT_FLOAT_EQ(Client.GetFarClip(), FViewportClient::DefaultFarClip);
	EXPECT_FLOAT_EQ(Client.GetTerrainFadeStart(), FViewportClient::DefaultTerrainFadeStart);
	EXPECT_FLOAT_EQ(Client.GetTerrainRenderDistance(), FViewportClient::DefaultTerrainRenderDistance);
}

TEST(FLevelEditorViewportClientTests, FocusesTheSelectedActorFromViewportInput)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "FocusWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "FocusLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("FocusTarget");
	ASSERT_NE(Actor, nullptr);
	Actor->GetRootComponent()->SetWorldLocation({12.0, -4.0, 7.0});

	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bFocused = true;
	Input.bFocusSelection = true;
	Client.Update(Level, Actor, Input);

	ExpectVectorNear(Client.GetCameraTransform().GetOrbitPivot(), Actor->GetRootComponent()->GetWorldLocation());
	EXPECT_DOUBLE_EQ(Client.GetCameraTransform().GetOrbitDistance(), 5.0);
}

TEST(FSceneViewProjectionTests, ProjectsAndBuildsRayFromSceneView)
{
	Durin::FSceneView View;
	View.ViewportWidth = 800;
	View.ViewportHeight = 600;
	View.ViewProjectionMatrix = Durin::FMatrix(1.0);
	Durin::FVector2f ViewportPosition;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, {0.0, 0.0, 0.5}, ViewportPosition));
	EXPECT_FLOAT_EQ(ViewportPosition.x, 400.0f);
	EXPECT_FLOAT_EQ(ViewportPosition.y, 300.0f);
	Durin::FVector3 Origin;
	Durin::FVector3 Direction;
	ASSERT_TRUE(Durin::SceneViewProjection::BuildViewportRay(View, ViewportPosition, Origin, Direction));
	ExpectVectorNear(Origin, {0.0, 0.0, 0.0});
	ExpectVectorNear(Direction, {0.0, 0.0, 1.0});
	View.ViewProjectionMatrix = Durin::FMatrix(0.0);
	EXPECT_FALSE(Durin::SceneViewProjection::BuildViewportRay(View, ViewportPosition, Origin, Direction));
}
