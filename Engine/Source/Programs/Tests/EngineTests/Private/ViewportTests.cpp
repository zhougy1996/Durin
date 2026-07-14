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
#include "Editor/EditorTransaction.h"
#include "IRendererModule.h"
#include "LevelViewportSessionSettings.h"
#include "LevelEditorContext.h"
#include "Mona/SceneViewport.h"
#include "Misc/Paths.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Viewport/ViewportCameraTransform.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Yaml/Yaml.h"

#include <gtest/gtest.h>

namespace
{
	class FCountingTransaction final : public Durin::IEditorTransaction
	{
	public:
		explicit FCountingTransaction(int& InValue, int InDelta = 1) : Value(InValue), Delta(InDelta) {}
		auto GetDescription() const -> std::string_view override { return "Counting"; }
		auto Undo() -> bool override { Value -= Delta; return true; }
		auto Redo() -> bool override { Value += Delta; return true; }
	private:
		int& Value;
		int Delta;
	};
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

TEST(FEditorTransactionManagerTests, ExecutesUndoesRedoesAndClearsRedoBranch)
{
	int Value = 0;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.CanUndo());
	EXPECT_EQ(Manager.GetUndoDescription(), "Counting");
	ASSERT_TRUE(Manager.Undo());
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.CanRedo());
	ASSERT_TRUE(Manager.Redo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Manager.Undo());
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value, 2)));
	EXPECT_EQ(Value, 2);
	EXPECT_FALSE(Manager.CanRedo());
	Manager.Clear();
	EXPECT_FALSE(Manager.CanUndo());
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

TEST(FTransformGizmoTests, BuildsNativeOverlayForSelectedActorModes)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "GizmoWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "GizmoLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Selected");
	ASSERT_NE(Actor, nullptr);
	Durin::FLevelEditorContext Context;
	Context.Synchronize(World);
	Context.SelectActor(Actor);
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	Durin::FLevelEditorViewportInput Input;
	Input.ViewportSize = {800.0f, 600.0f};
	Client.GetTransformGizmo().Update(Context, View, Input, nullptr);

	Durin::FSceneView TranslateView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, TranslateView));
	EXPECT_GE(TranslateView.OverlayPrimitives.size(), 6u);
	ASSERT_FALSE(TranslateView.OverlayPrimitives.empty());
	ExpectVectorNear(Durin::FVector3(TranslateView.OverlayPrimitives.front().LocalToWorld[3]), Actor->GetActorTransform().Translation);
	const Durin::FVector3 InitialLocation = Actor->GetActorTransform().Translation;
	const Durin::FVector3 XHandlePoint = Durin::FVector3(TranslateView.OverlayPrimitives.front().LocalToWorld * Durin::FVector4(0.65, 0.0, 0.0, 1.0));
	Durin::FVector2f CenterScreen;
	Durin::FVector2f HandleScreen;
	ASSERT_TRUE(Client.ProjectWorldToViewport(InitialLocation, {800.0f, 600.0f}, CenterScreen));
	ASSERT_TRUE(Client.ProjectWorldToViewport(XHandlePoint, {800.0f, 600.0f}, HandleScreen));
	Durin::FLevelEditorViewportInput DragInput;
	DragInput.bFocused = true;
	DragInput.bHovered = true;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.ViewportSize = {800.0f, 600.0f};
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, nullptr);
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.MousePosition += glm::normalize(HandleScreen - CenterScreen) * 30.0f;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, nullptr);
	EXPECT_GT(glm::length(Actor->GetActorTransform().Translation - InitialLocation), 0.001);
	Durin::FSceneView DraggedView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, DraggedView));
	ExpectVectorNear(Durin::FVector3(DraggedView.OverlayPrimitives.front().LocalToWorld[3]), Actor->GetActorTransform().Translation);
	DragInput.bCancel = true;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, nullptr);
	ExpectVectorNear(Actor->GetActorTransform().Translation, InitialLocation);
	Client.GetTransformGizmo().SetMode(Durin::ETransformGizmoMode::Rotate);
	Durin::FSceneView RotateView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, RotateView));
	EXPECT_EQ(RotateView.OverlayPrimitives.size(), 3u);
	Client.GetTransformGizmo().SetMode(Durin::ETransformGizmoMode::Scale);
	Durin::FSceneView ScaleView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, ScaleView));
	EXPECT_EQ(ScaleView.OverlayPrimitives.size(), 7u);
}

TEST(FLevelEditorViewportClientTests, BuildsComponentOrientedSelectionBounds)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "SelectionBoundsWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "SelectionBoundsLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle(Level);
	Durin::AStaticMeshActor* Actor = Level->SpawnActor<Durin::AStaticMeshActor>("SelectedMesh");
	ASSERT_NE(Actor, nullptr);
	Durin::DStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
	Component->SetStaticMesh(Mesh);
	Component->SetWorldLocation({3.0, 4.0, 5.0});
	Component->SetWorldRotation(glm::angleAxis(glm::radians(35.0), Durin::FVector3(0.0, 0.0, 1.0)));
	Component->SetWorldScale3D({2.0, 0.5, 1.5});

	std::vector<Durin::TObjectPtr<Durin::AActor>> Selection;
	Selection.emplace_back(Actor);
	Durin::FLevelEditorViewportClient Client;
	Client.SetSelectedActors(Selection, Actor);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const auto It = std::ranges::find_if(View.OverlayPrimitives, [](const Durin::FViewOverlayPrimitive& Primitive) {
		return Primitive.Shape == Durin::EViewOverlayShape::WireBox;
	});
	ASSERT_NE(It, View.OverlayPrimitives.end());
	const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	const Durin::FVector3 ActualMin = Durin::FVector3(It->LocalToWorld * Durin::FVector4(-0.5, -0.5, -0.5, 1.0));
	const Durin::FVector3 ActualMax = Durin::FVector3(It->LocalToWorld * Durin::FVector4(0.5, 0.5, 0.5, 1.0));
	const Durin::FMatrix LocalToWorld = Component->GetRenderMatrix();
	ExpectVectorNear(ActualMin, Durin::FVector3(LocalToWorld * Durin::FVector4(RenderData->LocalBounds.Min, 1.0)));
	ExpectVectorNear(ActualMax, Durin::FVector3(LocalToWorld * Durin::FVector4(RenderData->LocalBounds.Max, 1.0)));
	EXPECT_FLOAT_EQ(It->Color.r, 1.0f);
	EXPECT_FLOAT_EQ(It->Color.g, 0.72f);
	EXPECT_FLOAT_EQ(It->Color.b, 0.19f);
	EXPECT_FLOAT_EQ(It->Color.a, 1.0f);
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
	InitializeDObjectSystem();
	FTestEngine Engine;
	FTestViewportClient Client;
	auto ClientViewport = std::make_shared<Durin::FSceneViewport>(&Client, std::shared_ptr<Durin::MViewport>{});
	Engine.SetTestViewport(ClientViewport);
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {11.0, 12.0, 13.0});

	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(&Engine, "ViewportTestWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportTestLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(CameraActor, nullptr);
	CameraActor->GetCameraComponent()->SetWorldLocation({7.0, 8.0, 9.0});
	Engine.SetTestWorld(World);
	Engine.SetTestViewport(nullptr);
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {7.0, 8.0, 9.0});
}
