#include "Actors/CameraActor.h"
#include "Actors/StaticMeshActor.h"
#include "AssetSystem.h"
#include "Client/ViewportClient.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "IRendererModule.h"
#include "LevelViewportSessionSettings.h"
#include "Mona/SceneViewport.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMesh.h"
#include "Viewport/ViewportCameraTransform.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Yaml/Yaml.h"

#include <gtest/gtest.h>

namespace
{
	class FTestViewportClient final : public Durin::FViewportClient
	{
	public:
		auto CalcSceneView(Durin::uint32 Width, Durin::uint32 Height, Durin::FSceneView& OutView) const -> bool override
		{
			OutView.ViewportWidth = Width;
			OutView.ViewportHeight = Height;
			OutView.ViewLocation = {11.0, 12.0, 13.0};
			return true;
		}
	};

	class FTestEngine final : public Durin::DEngine
	{
	public:
		FTestEngine() : DEngine(Durin::FObjectInitializer::Get()) {}
		using DEngine::BuildMainSceneView;

		auto SetTestWorld(Durin::DWorld* World) -> void { MainWorld = World; }
		auto SetTestViewport(const std::shared_ptr<Durin::FSceneViewport>& Viewport) -> void { MainSceneViewport = Viewport; }
	};

	auto ExpectVectorNear(const Durin::FVector3& Actual, const Durin::FVector3& Expected, double Tolerance = 1.e-6) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}

}

TEST(FViewportCameraTransformTests, ClampsPitchAndBuildsOrthonormalDirections)
{
	Durin::FViewportCameraTransform Camera;
	Camera.Rotate(0.0f, 200.0f);
	EXPECT_DOUBLE_EQ(Camera.GetPitch(), 89.0);
	EXPECT_NEAR(glm::length(Camera.GetForwardVector()), 1.0, 1.e-8);
	EXPECT_NEAR(glm::dot(Camera.GetForwardVector(), Camera.GetRightVector()), 0.0, 1.e-8);
	EXPECT_NEAR(glm::dot(Camera.GetForwardVector(), Camera.GetUpVector()), 0.0, 1.e-8);
}

TEST(FViewportCameraTransformTests, MovesPansAndPreservesOrbitDistance)
{
	Durin::FViewportCameraTransform Camera;
	const Durin::FVector3 InitialLocation = Camera.GetLocation();
	const Durin::FVector3 InitialPivot = Camera.GetOrbitPivot();
	Camera.MoveLocal({2.0, 0.0, 0.0});
	ExpectVectorNear(Camera.GetOrbitPivot() - InitialPivot, Camera.GetLocation() - InitialLocation);

	Camera.Pan(1.0f, -0.5f);
	const double Distance = Camera.GetOrbitDistance();
	Camera.Orbit(35.0f, 15.0f);
	EXPECT_NEAR(glm::length(Camera.GetOrbitPivot() - Camera.GetLocation()), Distance, 1.e-8);
}

TEST(FViewportCameraTransformTests, FocusAndDollyRemainFiniteAtDegenerateDistance)
{
	Durin::FViewportCameraTransform Camera;
	Camera.Focus({3.0, 4.0, 5.0}, 0.0f);
	EXPECT_GE(Camera.GetOrbitDistance(), 0.05);
	ExpectVectorNear(Camera.GetOrbitPivot(), {3.0, 4.0, 5.0});
	Camera.Dolly(100000.0f);
	EXPECT_GE(Camera.GetOrbitDistance(), 0.05);
	const Durin::FVector3 Location = Camera.GetLocation();
	EXPECT_TRUE(std::isfinite(Location.x) && std::isfinite(Location.y) && std::isfinite(Location.z));
}

TEST(FViewportCameraTransformTests, RestoresCompleteStateAndConstrainsInvalidNavigationValues)
{
	Durin::FViewportCameraTransform Camera;
	Durin::FLevelViewportCameraState State;
	State.Location = {10.0, 20.0, 30.0};
	State.OrbitPivot = {1.0, 2.0, 3.0};
	State.OrbitDistance = -4.0;
	State.Pitch = 200.0;
	State.Yaw = 123.0;
	Camera.SetState(State);
	const Durin::FLevelViewportCameraState Actual = Camera.GetState();
	ExpectVectorNear(Actual.Location, State.Location);
	ExpectVectorNear(Actual.OrbitPivot, State.OrbitPivot);
	EXPECT_DOUBLE_EQ(Actual.OrbitDistance, 0.05);
	EXPECT_DOUBLE_EQ(Actual.Pitch, 89.0);
	EXPECT_DOUBLE_EQ(Actual.Yaw, State.Yaw);
}

TEST(FLevelViewportSessionSettingsTests, RoundTripsProjectsAndLevelsAndSkipsInvalidEntries)
{
	Durin::FLevelViewportStateMap States;
	States["G:/Projects/A/A.dproject"]["/A/Levels/Main"] = {{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}, 7.0, -8.0, 9.0};
	States["G:/Projects/B/B.dproject"]["/B/Levels/Other"] = {{10.0, 20.0, 30.0}, {40.0, 50.0, 60.0}, 70.0, -80.0, 90.0};
	Durin::FYamlDocument Document;
	Durin::FYamlNodeRef Root = Document.GetMutableRoot();
	Root.EnsureMap();
	Durin::SaveLevelViewportStates(Root, States);
	Durin::FYamlNodeRef Invalid = Root.GetRef("LevelViewportStates").AppendMap();
	Invalid.SetChildValue("Project", "G:/Projects/A/A.dproject");
	Invalid.SetChildValue("Level", "/A/Levels/Broken");
	Invalid.AddSequence("Location").AppendValue(1.0).AppendValue(2.0);

	Durin::FLevelViewportStateMap Loaded;
	Durin::LoadLevelViewportStates(Document.GetRootView(), Loaded);
	ASSERT_EQ(Loaded.size(), 2u);
	ASSERT_EQ(Loaded.at("G:/Projects/A/A.dproject").size(), 1u);
	const Durin::FLevelViewportCameraState& Main = Loaded.at("G:/Projects/A/A.dproject").at("/A/Levels/Main");
	ExpectVectorNear(Main.Location, {1.0, 2.0, 3.0});
	ExpectVectorNear(Main.OrbitPivot, {4.0, 5.0, 6.0});
	EXPECT_DOUBLE_EQ(Main.OrbitDistance, 7.0);
	EXPECT_DOUBLE_EQ(Main.Pitch, -8.0);
	EXPECT_DOUBLE_EQ(Main.Yaw, 9.0);
}

TEST(FLevelEditorViewportClientTests, NavigationDoesNotDirtyTheLevelPackage)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "ViewportLevels";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/ViewportTests/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/ViewportTests/NavigationDirty", Path));
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Level));
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	ASSERT_FALSE(Level->GetPackage()->IsDirty());

	Durin::FLevelEditorViewportClient Client;
	Durin::FLevelEditorViewportInput Input;
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
	Durin::FLevelEditorViewportClient Client;
	Durin::FVector3 Origin;
	Durin::FVector3 Direction;
	EXPECT_FALSE(Client.BuildPickingRay({0.0f, 0.0f}, {0.0f, 100.0f}, Origin, Direction));
	ASSERT_TRUE(Client.BuildPickingRay({400.0f, 300.0f}, {800.0f, 600.0f}, Origin, Direction));
	EXPECT_NEAR(glm::length(Direction), 1.0, 1.e-8);
	ExpectVectorNear(Direction, Client.GetCameraTransform().GetForwardVector(), 1.e-6);
}

TEST(FLevelEditorViewportClientTests, PicksClosestTriangleAndRejectsBoundsOnlyHit)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "PickingWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "PickingLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::FLevelEditorViewportClient Client;
	const Durin::FVector3 CameraLocation = Client.GetCameraTransform().GetLocation();
	const Durin::FVector3 Forward = Client.GetCameraTransform().GetForwardVector();
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle(Level);
	Durin::AStaticMeshActor* NearActor = Level->SpawnActor<Durin::AStaticMeshActor>("Near");
	Durin::AStaticMeshActor* FarActor = Level->SpawnActor<Durin::AStaticMeshActor>("Far");
	ASSERT_NE(NearActor, nullptr);
	ASSERT_NE(FarActor, nullptr);
	NearActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	FarActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	NearActor->GetStaticMeshComponent()->SetWorldLocation(CameraLocation + Forward * 3.0);
	FarActor->GetStaticMeshComponent()->SetWorldLocation(CameraLocation + Forward * 6.0);
	NearActor->GetStaticMeshComponent()->SetWorldRotation(glm::angleAxis(glm::radians(20.0), Forward));
	NearActor->GetStaticMeshComponent()->SetWorldScale3D({2.0, 0.5, 1.5});
	EXPECT_EQ(Client.PickActor(Level, {400.0f, 300.0f}, {800.0f, 600.0f}), NearActor);
	EXPECT_EQ(Client.PickActor(Level, {799.0f, 300.0f}, {800.0f, 600.0f}), nullptr);
}

TEST(FViewportSelectionTests, PrefersViewportClientAndFallsBackToPrimaryCamera)
{
	std::cerr << "viewport step 1\n";
	InitializeDObjectSystem();
	FTestEngine Engine;
	std::cerr << "viewport step 2\n";
	FTestViewportClient Client;
	auto ClientViewport = std::make_shared<Durin::FSceneViewport>(&Client, std::shared_ptr<Durin::MViewport>{});
	Engine.SetTestViewport(ClientViewport);
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {11.0, 12.0, 13.0});
	std::cerr << "viewport step 3\n";

	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(&Engine, "ViewportTestWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportTestLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	std::cerr << "viewport step 4\n";
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(CameraActor, nullptr);
	std::cerr << "viewport step 5\n";
	CameraActor->GetCameraComponent()->SetWorldLocation({7.0, 8.0, 9.0});
	Engine.SetTestWorld(World);
	Engine.SetTestViewport(nullptr);
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {7.0, 8.0, 9.0});
	std::cerr << "viewport step 6\n";
}
