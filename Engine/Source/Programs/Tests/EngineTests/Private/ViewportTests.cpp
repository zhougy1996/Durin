#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Actors/StaticMeshActor.h"
#include "AssetSystem.h"
#include "CameraEditorCustomizations.h"
#include "Client/ViewportClient.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DirectionalLightEditorCustomizations.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "Editor/EditorTransaction.h"
#include "IRendererModule.h"
#include "LevelViewportSessionSettings.h"
#include "LevelEditorContext.h"
#include "LevelEditorCustomizations.h"
#include "MonaImGui.h"
#include "Mona/SceneViewport.h"
#include "Misc/Paths.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "SplineEditorCustomizations.h"
#include "Viewport/ViewportCameraTransform.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/CameraPreviewViewportClient.h"
#include "Yaml/Yaml.h"

#include <gtest/gtest.h>

namespace
{
	class FCountingTransaction final : public Durin::IEditorTransaction
	{
	public:
		explicit FCountingTransaction(int& InValue, int InDelta = 1) : Value(InValue), Delta(InDelta) {}
		auto GetDescription() const -> std::string_view override { return "Counting"; }
		auto GetDetails(Durin::EEditorTransactionOperation Operation) const -> std::string override
		{
			return Operation == Durin::EEditorTransactionOperation::Undo ? "Counter changed backward" : "Counter changed forward";
		}
		auto Undo() -> bool override { Value -= Delta; return true; }
		auto Redo() -> bool override { Value += Delta; return true; }
	private:
		int& Value;
		int Delta;
	};
	struct FTransactionControl
	{
		bool bFailUndo = false;
		bool bFailRedo = false;
	};
	class FControlledTransaction final : public Durin::IEditorTransaction
	{
	public:
		FControlledTransaction(int& InValue, FTransactionControl& InControl) : Value(InValue), Control(InControl) {}
		auto GetDescription() const -> std::string_view override { return "Controlled"; }
		auto Undo() -> bool override
		{
			if (Control.bFailUndo) return false;
			--Value;
			return true;
		}
		auto Redo() -> bool override
		{
			if (Control.bFailRedo) return false;
			++Value;
			return true;
		}
	private:
		int& Value;
		FTransactionControl& Control;
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

	class FTestComponentVisualizer final : public Durin::IComponentEditorVisualizer
	{
	public:
		auto DrawVisualization(Durin::DActorComponent* Component, const Durin::FEditorVisualizationContext&, Durin::FEditorVisualizationCollector& Collector) const -> void override
		{
			auto* SceneComponent = Durin::Cast<Durin::DSceneComponent>(Component);
			Durin::AActor* Actor = SceneComponent ? SceneComponent->GetOwner() : nullptr;
			if (!Actor) return;
			const Durin::FVector3 Center = SceneComponent->GetWorldLocation();
			Collector.AddLine({Center - Durin::FVectorConstants::Right, Center + Durin::FVectorConstants::Right, Durin::FVector4f(1.0f), 2.0f, 8.0f, 5, Actor, Component});
		}
	};

	class FTestDetailsCustomization final : public Durin::IObjectDetailsCustomization
	{
	public:
		auto DrawDetails(Durin::FLevelEditorContext&, Durin::DObject*) -> bool override { return false; }
	};

	struct FCustomizationGuard
	{
		Durin::FLevelEditorCustomizationHandle Handle;
		~FCustomizationGuard() { if (Handle) Durin::FLevelEditorCustomizationRegistry::Get().Unregister(Handle); }
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

TEST(FEditorTransactionManagerTests, UsesStableIdsAndRejectsStaleUndoRequests)
{
	int Value = 0;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::FEditorTransactionId FirstId = Manager.GetUndoId();
	ASSERT_NE(FirstId, 0);
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::FEditorTransactionId SecondId = Manager.GetUndoId();
	ASSERT_NE(SecondId, FirstId);

	EXPECT_FALSE(Manager.Undo(FirstId));
	EXPECT_EQ(Value, 2);
	ASSERT_TRUE(Manager.Undo(SecondId));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.IsRedoHead(SecondId));
	ASSERT_TRUE(Manager.Redo(SecondId));
	EXPECT_EQ(Value, 2);
	EXPECT_TRUE(Manager.IsUndoHead(SecondId));

	const std::vector<Durin::FEditorTransactionEvent> Events = Manager.ConsumeEvents();
	ASSERT_EQ(Events.size(), 4);
	EXPECT_EQ(Events[0].Type, Durin::EEditorTransactionEventType::Executed);
	EXPECT_EQ(Events[0].Details, "Counter changed forward");
	EXPECT_EQ(Events[1].Type, Durin::EEditorTransactionEventType::Executed);
	EXPECT_EQ(Events[2].Type, Durin::EEditorTransactionEventType::Undone);
	EXPECT_EQ(Events[2].Details, "Counter changed backward");
	EXPECT_EQ(Events[3].Type, Durin::EEditorTransactionEventType::Redone);
	EXPECT_EQ(Events[3].Details, "Counter changed forward");
}

TEST(FEditorTransactionManagerTests, KeepsTransactionsOnTheirOriginalStackWhenApplyFails)
{
	int Value = 0;
	FTransactionControl Control;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FControlledTransaction>(Value, Control)));
	const Durin::FEditorTransactionId Id = Manager.GetUndoId();
	Manager.ConsumeEvents();

	Control.bFailUndo = true;
	EXPECT_FALSE(Manager.Undo(Id));
	EXPECT_EQ(Value, 1);
	EXPECT_TRUE(Manager.IsUndoHead(Id));
	ASSERT_EQ(Manager.ConsumeEvents().back().Type, Durin::EEditorTransactionEventType::Failed);

	Control.bFailUndo = false;
	ASSERT_TRUE(Manager.Undo(Id));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.IsRedoHead(Id));
	Manager.ConsumeEvents();

	Control.bFailRedo = true;
	EXPECT_FALSE(Manager.Redo(Id));
	EXPECT_EQ(Value, 0);
	EXPECT_TRUE(Manager.IsRedoHead(Id));
	ASSERT_EQ(Manager.ConsumeEvents().back().Type, Durin::EEditorTransactionEventType::Failed);
}

TEST(FEditorTransactionManagerTests, ClearsRedoBranchAndPendingEventsOnNewCommitAndClear)
{
	int Value = 0;
	Durin::FEditorTransactionManager Manager;
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value)));
	const Durin::FEditorTransactionId OldId = Manager.GetUndoId();
	ASSERT_TRUE(Manager.Undo(OldId));
	ASSERT_TRUE(Manager.Execute(std::make_unique<FCountingTransaction>(Value, 2)));
	EXPECT_FALSE(Manager.CanRedo());
	EXPECT_FALSE(Manager.Redo(OldId));

	Manager.Clear();
	EXPECT_TRUE(Manager.ConsumeEvents().empty());
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

TEST(FSplineComponentVisualizerTests, EmitsSelectableCurveAndControlPointLines)
{
	InitializeDObjectSystem();
	auto* Actor = Durin::NewObject<Durin::AActor>(nullptr, "SplineActor");
	auto* Spline = Durin::Cast<Durin::DSplineComponent>(Actor->AddInstanceComponent(Durin::DSplineComponent::StaticClass(), "Spline"));
	ASSERT_NE(Spline, nullptr);
	Durin::FSceneView View;
	View.ViewportWidth = 800;
	View.ViewportHeight = 600;
	Durin::FEditorVisualizationCollector Collector;
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateSplineComponentVisualizer();
	Visualizer->DrawVisualization(Spline, {View, nullptr, true, false, true}, Collector);

	ASSERT_FALSE(Collector.GetLines().empty());
	EXPECT_TRUE(std::ranges::all_of(Collector.GetLines(), [Actor, Spline](const Durin::FEditorVisualizationLine& Line) {
		return Line.Actor == Actor && Line.Component == Spline;
	}));
	EXPECT_GT(Collector.GetLines().size(), static_cast<size_t>(Spline->GetReparamStepsPerSegment()));

	Durin::MarkObjectHierarchyAsGarbage(Actor);
	Durin::CollectGarbage();
}

TEST(FLevelEditorCustomizationRegistryTests, RejectsDuplicatesFindsBaseClassAndUnregisters)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::FLevelEditorCustomizationRegistry::Get();
	const auto Visualizer = std::make_shared<FTestComponentVisualizer>();
	FCustomizationGuard VisualizerGuard{Registry.RegisterComponentVisualizer(Durin::DSceneComponent::StaticClass(), Visualizer)};
	ASSERT_TRUE(VisualizerGuard.Handle);
	EXPECT_FALSE(Registry.RegisterComponentVisualizer(Durin::DSceneComponent::StaticClass(), std::make_shared<FTestComponentVisualizer>()));
	EXPECT_EQ(Registry.FindComponentVisualizer(Durin::DCameraComponent::StaticClass()), Visualizer);

	const auto Details = std::make_shared<FTestDetailsCustomization>();
	FCustomizationGuard DetailsGuard{Registry.RegisterObjectDetails(Durin::DSceneComponent::StaticClass(), Details)};
	ASSERT_TRUE(DetailsGuard.Handle);
	EXPECT_EQ(Registry.FindObjectDetails(Durin::DCameraComponent::StaticClass()), Details);
	ASSERT_TRUE(Registry.Unregister(DetailsGuard.Handle));
	DetailsGuard.Handle = {};
	EXPECT_EQ(Registry.FindObjectDetails(Durin::DCameraComponent::StaticClass()), nullptr);
}

TEST(FEditorVisualizationCollectorTests, UsesTheSameLinesForRenderingAndPicking)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "VisualizationWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "VisualizationLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::FEditorVisualizationCollector Collector;
	Collector.AddLine({Center - Client.GetCameraTransform().GetRightVector(), Center + Client.GetCameraTransform().GetRightVector(), {1.0f, 0.5f, 0.25f, 1.0f}, 3.0f, 7.0f, 4, Actor, Actor->GetCameraComponent()});
	Collector.AppendToView(View);
	ASSERT_EQ(View.OverlayLines.size(), 1u);
	EXPECT_FLOAT_EQ(View.OverlayLines.front().WidthPixels, 3.0f);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter);
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_EQ(Hit.Component, Actor->GetCameraComponent());
	EXPECT_EQ(Collector.HitTest(View, ScreenCenter + Durin::FVector2f(0.0f, 50.0f)).Actor, nullptr);
}

TEST(FLevelEditorViewportClientTests, PublishesWorldGridStateToTheSceneView)
{
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	EXPECT_TRUE(View.EditorGrid.bVisible);
	EXPECT_DOUBLE_EQ(View.EditorGrid.Height, 0.0);
	EXPECT_GT(View.EditorGrid.FadeDistance, 0.0f);
	EXPECT_GT(View.EditorGrid.AxisXColor.r, View.EditorGrid.AxisXColor.g);
	EXPECT_GT(View.EditorGrid.AxisYColor.g, View.EditorGrid.AxisYColor.r);

	Client.SetGridVisible(false);
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	EXPECT_FALSE(View.EditorGrid.bVisible);
}

TEST(FEditorVisualizationCollectorTests, UsesTheSameIconsForRenderingAndDepthIndependentPicking)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "IconVisualizationWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "IconVisualizationLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const Durin::FVector3 Center = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::FEditorVisualizationCollector Collector;
	Collector.AddIcon({Durin::EViewOverlayIcon::Camera, Center, Durin::FVector4f(1.0f), 30.0f, 3.0f, 100, Actor, Actor->GetCameraComponent(), true});
	Collector.AppendToView(View);
	ASSERT_EQ(View.OverlayIcons.size(), 1u);
	EXPECT_FLOAT_EQ(View.OverlayIcons.front().SizePixels, 30.0f);
	Durin::FVector2f ScreenCenter;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, Center, ScreenCenter));
	const Durin::FEditorVisualizationHit Hit = Collector.HitTest(View, ScreenCenter + Durin::FVector2f(14.0f, 0.0f));
	EXPECT_EQ(Hit.Actor, Actor);
	EXPECT_TRUE(Hit.bDepthIndependent);
	EXPECT_EQ(Collector.HitTest(View, ScreenCenter + Durin::FVector2f(24.0f, 0.0f)).Actor, nullptr);
}

TEST(FCameraComponentVisualizerTests, DrawsOnlyAnIconUntilSelected)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "CameraVisualizerWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "CameraVisualizerLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::FLevelEditorViewportClient Client;
	Actor->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateCameraComponentVisualizer();
	ASSERT_NE(Visualizer, nullptr);

	Durin::FEditorVisualizationCollector Unselected;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, false, false, false}, Unselected);
	EXPECT_EQ(Unselected.GetIcons().size(), 1u);
	EXPECT_TRUE(Unselected.GetLines().empty());
	EXPECT_FLOAT_EQ(Unselected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(36.0f));

	Actor->GetCameraComponent()->SetProjectionParameters(60.0f, 0.25f, 1.0f);
	Durin::FEditorVisualizationCollector Selected;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, true, false, true}, Selected);
	EXPECT_EQ(Selected.GetIcons().size(), 1u);
	EXPECT_FLOAT_EQ(Selected.GetIcons().front().SizePixels, Durin::MonaImGui::ScaleUI(40.0f));
	ASSERT_EQ(Selected.GetLines().size(), 13u);
	const Durin::FVector3 Forward = Actor->GetCameraComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	const Durin::FVector3 Origin = Actor->GetCameraComponent()->GetWorldLocation();
	EXPECT_NEAR(glm::dot(Selected.GetLines()[0].Start - Origin, Forward), 0.25, 1.e-5);
	EXPECT_NEAR(glm::dot(Selected.GetLines()[0].End - Origin, Forward), 0.25, 1.e-5);
	EXPECT_EQ(Selected.GetLines()[2].Pattern, Durin::EViewOverlayLinePattern::Solid);
	const ImVec4& ExpectedForwardColor = Durin::MonaImGui::GetThemeColor(Durin::MonaImGui::EUIThemeColor::AxisX);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().Color.r, ExpectedForwardColor.x);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().Color.g, ExpectedForwardColor.y);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().Color.b, ExpectedForwardColor.z);
	EXPECT_FLOAT_EQ(Selected.GetLines().back().WidthPixels, 3.0f);
}

TEST(FCameraComponentVisualizerTests, UsesTheActualFarPlaneAtExtremeFieldOfView)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "TruncatedCameraVisualizerWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "TruncatedCameraVisualizerLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	Durin::FLevelEditorViewportClient Client;
	Actor->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Actor->GetCameraComponent()->SetProjectionParameters(170.0f, 0.1f, 1000.0f);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateCameraComponentVisualizer();
	Durin::FEditorVisualizationCollector Collector;
	Visualizer->DrawVisualization(Actor->GetCameraComponent(), {View, Level, true, false, true}, Collector);
	ASSERT_EQ(Collector.GetLines().size(), 13u);
	EXPECT_EQ(std::ranges::count_if(Collector.GetLines(), [](const Durin::FEditorVisualizationLine& Line) {
		return Line.Pattern == Durin::EViewOverlayLinePattern::Dashed;
	}), 0);
	const Durin::FVector3 Forward = Actor->GetCameraComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	const Durin::FVector3 Origin = Actor->GetCameraComponent()->GetWorldLocation();
	EXPECT_NEAR(glm::dot(Collector.GetLines()[2].Start - Origin, Forward), 1000.0, 1.e-4);
	EXPECT_NEAR(glm::dot(Collector.GetLines()[2].End - Origin, Forward), 1000.0, 1.e-4);
	EXPECT_TRUE(std::ranges::all_of(Collector.GetLines(), [](const Durin::FEditorVisualizationLine& Line) {
		return std::isfinite(glm::length(Line.End - Line.Start)) && glm::length(Line.End - Line.Start) > Durin::kSmallNumber;
	}));
}

TEST(FDirectionalLightComponentVisualizerTests, DrawsSelectableIconAndSelectedDirectionCue)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "DirectionalLightVisualizerWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "DirectionalLightVisualizerLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<Durin::ADirectionalLightActor>("DirectionalLight");
	ASSERT_NE(Actor, nullptr);
	Durin::FLevelEditorViewportClient Client;
	Actor->GetLightComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 10.0);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	const std::shared_ptr<Durin::IComponentEditorVisualizer> Visualizer = Durin::CreateDirectionalLightComponentVisualizer();
	ASSERT_NE(Visualizer, nullptr);

	Durin::FEditorVisualizationCollector Unselected;
	Visualizer->DrawVisualization(Actor->GetLightComponent(), {View, Level, false, false, false}, Unselected);
	ASSERT_EQ(Unselected.GetIcons().size(), 1u);
	EXPECT_EQ(Unselected.GetIcons().front().Icon, Durin::EViewOverlayIcon::DirectionalLight);
	EXPECT_TRUE(Unselected.GetLines().empty());

	Durin::FEditorVisualizationCollector Selected;
	Visualizer->DrawVisualization(Actor->GetLightComponent(), {View, Level, true, false, true}, Selected);
	ASSERT_EQ(Selected.GetIcons().size(), 1u);
	ASSERT_EQ(Selected.GetLines().size(), 5u);
	const Durin::FVector3 Origin = Actor->GetLightComponent()->GetWorldLocation();
	const Durin::FVector3 Forward = Actor->GetLightComponent()->GetWorldRotation() * Durin::FVectorConstants::Forward;
	EXPECT_NEAR(glm::length(Selected.GetLines().front().Start - Origin), 0.0, 1.e-6);
	EXPECT_GT(glm::dot(Selected.GetLines().front().End - Origin, Forward), 0.0);
	EXPECT_TRUE(Selected.GetIcons().front().bDepthIndependentHit);
}

TEST(FLevelEditorViewportClientTests, PicksVisualizerForActorWithoutStaticMesh)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::FLevelEditorCustomizationRegistry::Get();
	FCustomizationGuard Guard{Registry.RegisterComponentVisualizer(Durin::DCameraComponent::StaticClass(), std::make_shared<FTestComponentVisualizer>())};
	ASSERT_TRUE(Guard.Handle);
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "VisualizerPickingWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "VisualizerPickingLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::FLevelEditorViewportClient Client;
	Durin::ACameraActor* Camera = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Camera, nullptr);
	Camera->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	EXPECT_EQ(Client.PickActor(Level, {400.0f, 300.0f}, {800.0f, 600.0f}), Camera);
}

TEST(FLevelEditorViewportClientTests, ResetsIndependentViewUnlessSavedStateExists)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "ViewportResetWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportResetLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Camera = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Camera, nullptr);
	Camera->GetCameraComponent()->SetWorldLocation({100.0, 200.0, 300.0});
	Durin::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Level);
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), Durin::FLevelViewportCameraState{}.Location);
	Durin::FLevelViewportCameraState Saved;
	Saved.Location = {8.0, 9.0, 10.0};
	Saved.OrbitPivot = {1.0, 2.0, 3.0};
	Client.InitializeForLevel(Level, &Saved);
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), Saved.Location);
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

	Durin::FEditorTransactionManager TransformTransactions;
	DragInput.bCancel = false;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.MousePosition += glm::normalize(HandleScreen - CenterScreen) * 30.0f;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	DragInput.bLeftMouseDown = false;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	const std::vector<Durin::FEditorTransactionEvent> TransformEvents = TransformTransactions.ConsumeEvents();
	ASSERT_EQ(TransformEvents.size(), 1);
	EXPECT_EQ(TransformEvents.front().Description, "Translate 'Selected'");
	EXPECT_NE(TransformEvents.front().Details.find("'Selected'"), std::string::npos);
	EXPECT_NE(TransformEvents.front().Details.find("Location"), std::string::npos);
	EXPECT_NE(TransformEvents.front().Details.find("Delta"), std::string::npos);

	Client.GetTransformGizmo().SetMode(Durin::ETransformGizmoMode::Rotate);
	Durin::FSceneView RotateView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, RotateView));
	EXPECT_EQ(RotateView.OverlayPrimitives.size(), 3u);
	Client.GetTransformGizmo().SetMode(Durin::ETransformGizmoMode::Scale);
	EXPECT_EQ(Client.GetTransformGizmo().GetSpace(), Durin::ETransformGizmoSpace::World);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::ETransformGizmoSpace::Local);
	Client.GetTransformGizmo().SetSpace(Durin::ETransformGizmoSpace::Parent);
	EXPECT_EQ(Client.GetTransformGizmo().GetSpace(), Durin::ETransformGizmoSpace::Parent);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::ETransformGizmoSpace::Local);
	Actor->GetRootComponent()->SetWorldRotation(glm::angleAxis(glm::half_pi<double>(), Durin::FVectorConstants::Up));
	Client.GetTransformGizmo().Update(Context, RotateView, Input, nullptr);
	Durin::FSceneView ScaleView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, ScaleView));
	EXPECT_EQ(ScaleView.OverlayPrimitives.size(), 7u);
	ASSERT_FALSE(ScaleView.OverlayPrimitives.empty());
	ExpectVectorNear(glm::normalize(Durin::FVector3(ScaleView.OverlayPrimitives.front().LocalToWorld[0])), Durin::FVectorConstants::Right);
	Client.GetTransformGizmo().SetMode(Durin::ETransformGizmoMode::Translate);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::ETransformGizmoSpace::Parent);

	Durin::ACameraActor* Parent = Level->SpawnActor<Durin::ACameraActor>("Parent");
	ASSERT_NE(Parent, nullptr);
	Actor->GetRootComponent()->SetWorldRotation(glm::identity<Durin::FQuat>());
	Parent->GetRootComponent()->SetWorldRotation(glm::angleAxis(glm::half_pi<double>(), Durin::FVectorConstants::Up));
	ASSERT_TRUE(Actor->AttachToActor(Parent, Durin::EAttachmentTransformRule::KeepWorld));
	Client.GetTransformGizmo().Update(Context, ScaleView, Input, nullptr);
	Durin::FSceneView ParentView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, ParentView));
	ASSERT_FALSE(ParentView.OverlayPrimitives.empty());
	ExpectVectorNear(glm::normalize(Durin::FVector3(ParentView.OverlayPrimitives.front().LocalToWorld[0])), Durin::FVectorConstants::Right);
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

TEST(FViewportSelectionTests, ConstrainedCameraBuildsCenteredContentRect)
{
	InitializeDObjectSystem();
	FTestEngine Engine;
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(&Engine, "ConstrainedViewportWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ConstrainedViewportLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(CameraActor, nullptr);
	ASSERT_TRUE(Level->SetPrimaryCameraActor(CameraActor));
	Engine.SetTestWorld(World);

	Durin::FSceneView View = Engine.BuildMainSceneView(800, 600);
	EXPECT_EQ(View.ViewportX, 0u);
	EXPECT_EQ(View.ViewportY, 0u);
	EXPECT_EQ(View.ViewportWidth, 800u);
	EXPECT_EQ(View.ViewportHeight, 600u);

	CameraActor->GetCameraComponent()->SetAspectRatio(Durin::ECameraAspectRatioMode::Ratio16By9, 16.0f / 9.0f);
	View = Engine.BuildMainSceneView(800, 600);
	EXPECT_EQ(View.ViewportX, 0u);
	EXPECT_EQ(View.ViewportY, 75u);
	EXPECT_EQ(View.ViewportWidth, 800u);
	EXPECT_EQ(View.ViewportHeight, 450u);
	EXPECT_FLOAT_EQ(View.AspectRatioConstraint, 16.0f / 9.0f);
}

TEST(FCameraPreviewViewportClientTests, BuildsViewFromAssignedCameraAndRejectsMissingCamera)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "CameraPreviewWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "CameraPreviewLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("PreviewCamera");
	ASSERT_NE(CameraActor, nullptr);
	CameraActor->GetCameraComponent()->SetWorldLocation({3.0, 4.0, 5.0});

	Durin::FCameraPreviewViewportClient Client;
	Durin::FSceneView View;
	EXPECT_FALSE(Client.CalcSceneView(320, 180, View));
	Client.SetCamera(CameraActor->GetCameraComponent());
	ASSERT_TRUE(Client.CalcSceneView(320, 180, View));
	EXPECT_EQ(View.ViewportWidth, 320u);
	EXPECT_EQ(View.ViewportHeight, 180u);
	ExpectVectorNear(View.ViewLocation, {3.0, 4.0, 5.0});
	CameraActor->GetCameraComponent()->SetAspectRatio(Durin::ECameraAspectRatioMode::Ratio4By3, 4.0f / 3.0f);
	ASSERT_TRUE(Client.CalcSceneView(320, 180, View));
	EXPECT_NEAR(std::abs(View.ProjectionMatrix[2][1] / View.ProjectionMatrix[1][0]), 4.0f / 3.0f, 1.e-5f);
}
