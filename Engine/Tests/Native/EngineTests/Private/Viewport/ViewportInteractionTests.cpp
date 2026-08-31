#include "ViewportTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Actors/GameMode.h"
#include "Actors/PlayerStart.h"
#include "LevelEditorViewportEditing.h"
#include "Math/Operations.h"

namespace
{
	struct FEditModeProbe
	{
		int EnterCount = 0;
		int ExitCount = 0;
		int ForcedExitCount = 0;
		int TickCount = 0;
	};

	class FProbeEditMode final : public Durin::Editor::Level::ILevelViewportEditMode
	{
	public:
		explicit FProbeEditMode(std::shared_ptr<FEditModeProbe> InProbe) : Probe(std::move(InProbe)) {}
		auto Enter(Durin::Editor::Level::FLevelEditorContext&) -> void override { ++Probe->EnterCount; }
		auto Exit(Durin::Editor::Level::FLevelEditorContext&, bool bForced) -> void override { ++Probe->ExitCount; Probe->ForcedExitCount += bForced; }
		auto Tick(Durin::Editor::Level::FLevelEditorContext&, Durin::Editor::Level::FLevelEditorViewportClient&, const Durin::FSceneView&,
			Durin::Editor::Level::FLevelEditorViewportInput&, Durin::DTransactor*) -> bool override { ++Probe->TickCount; return true; }
	private:
		std::shared_ptr<FEditModeProbe> Probe;
	};

	class FProbeTransformTarget final : public Durin::Editor::Level::ITransformGizmoTarget
	{
	public:
		auto IsValid() const -> bool override { return bValid; }
		auto GetIdentity() const -> const void* override { return this; }
		auto GetTransform() const -> Durin::FTransform override { return Transform; }
		auto SetTransform(const Durin::FTransform& Value) -> bool override { Transform = Value; return bValid; }
		auto GetLabel() const -> std::string override { return "Probe"; }
		auto GetCapabilities() const -> Durin::Editor::Level::ETransformGizmoCapability override { return Capabilities; }
		Durin::FTransform Transform;
		Durin::Editor::Level::ETransformGizmoCapability Capabilities = Durin::Editor::Level::ETransformGizmoCapability::All;
		bool bValid = true;
	};

	auto SimulateFlyNavigation(
		Durin::Editor::Level::FLevelEditorViewportClient& Client,
		Durin::DLevel* Level,
		float DeltaSeconds,
		int32 FrameCount
	) -> void
	{
		Durin::Editor::Level::FLevelEditorViewportInput Input;
		Input.DeltaSeconds = DeltaSeconds;
		Input.bHovered = true;
		Input.bFocused = true;
		Input.bRightMouseDown = true;
		Input.bMoveForward = true;
		Input.MouseDelta = {90.0f * DeltaSeconds, -30.0f * DeltaSeconds};
		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			Input.bRightMousePressed = Frame == 0;
			Client.Update(Level, nullptr, Input);
		}
	}
}

TEST(FLevelViewportEditModeTests, KeepsInstancesPerManagerAndRepairsUnavailableModes)
{
	InitializeDObjectSystem();
	bool bAvailable = true;
	auto ProbeA = std::make_shared<FEditModeProbe>();
	auto ProbeB = std::make_shared<FEditModeProbe>();
	int FactoryCount = 0;
	auto& Registry = Durin::Editor::Level::FLevelViewportEditModeRegistry::Get();
	const Durin::Editor::Level::FLevelViewportEditModeHandle Handle = Registry.Register({
		.Id = "Probe",
		.DisplayName = "Probe",
		.Priority = 10,
		.CanActivate = [&bAvailable](const Durin::Editor::Level::FLevelEditorContext&) { return bAvailable; },
		.Factory = [&] { return std::make_unique<FProbeEditMode>(FactoryCount++ == 0 ? ProbeA : ProbeB); },
	});
	ASSERT_TRUE(Handle);
	Durin::Editor::Level::FLevelEditorContext ContextA;
	Durin::Editor::Level::FLevelEditorContext ContextB;
	Durin::Editor::Level::FLevelViewportEditModeManager ManagerA;
	Durin::Editor::Level::FLevelViewportEditModeManager ManagerB;
	ASSERT_TRUE(ManagerA.Activate("Probe", ContextA));
	ASSERT_TRUE(ManagerB.Activate("Probe", ContextB));
	EXPECT_EQ(FactoryCount, 2);
	EXPECT_NE(ManagerA.GetActiveMode(), ManagerB.GetActiveMode());
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	EXPECT_TRUE(ManagerA.Tick(ContextA, Client, View, Input, nullptr));
	EXPECT_EQ(ProbeA->TickCount, 1);
	bAvailable = false;
	ManagerA.Synchronize(ContextA);
	EXPECT_EQ(ManagerA.GetActiveModeId(), "Select");
	EXPECT_EQ(ProbeA->ExitCount, 1);
	EXPECT_EQ(ProbeA->ForcedExitCount, 1);
	EXPECT_TRUE(Registry.Unregister(Handle));
	ManagerB.Synchronize(ContextB);
	EXPECT_EQ(ManagerB.GetActiveModeId(), "Select");
	EXPECT_EQ(ProbeB->ForcedExitCount, 1);
}

TEST(FTransformGizmoTests, ManipulatesGenericTargetsAndCommitsWithoutActorKnowledge)
{
	InitializeDObjectSystem();
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	auto Target = std::make_shared<FProbeTransformTarget>();
	Target->Transform.Translation = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::Editor::Level::FTransformGizmoTargetSet Targets{{Target}, "Probes"};
	Durin::Editor::Level::FTransformGizmo Gizmo;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.ViewportSize = {800.0f, 600.0f};
	Gizmo.Update(Targets, View, Input, nullptr);
	Durin::FSceneView OverlayView = View;
	Gizmo.AppendOverlayPrimitives(OverlayView);
	ASSERT_FALSE(OverlayView.OverlayPrimitives.empty());
	const Durin::FVector3 InitialLocation = Target->Transform.Translation;
	const Durin::FVector3 HandlePoint = Durin::FVector3(OverlayView.OverlayPrimitives.front().LocalToWorld * Durin::FVector4(0.65, 0.0, 0.0, 1.0));
	Durin::FVector2f CenterScreen;
	Durin::FVector2f HandleScreen;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, InitialLocation, CenterScreen));
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, HandlePoint, HandleScreen));
	Durin::Tests::FTestTransactorOwner Transactions;
	const uint64 MountedContentRevision =
		Transactions->GetMountedContentMutationRevision();
	Input.bFocused = true;
	Input.bHovered = true;
	Input.bLeftMousePressed = true;
	Input.bLeftMouseDown = true;
	Input.MousePosition = HandleScreen;
	Gizmo.Update(Targets, View, Input, Transactions.Get());
	ASSERT_TRUE(Gizmo.IsDragging());
	Input.bLeftMousePressed = false;
	Input.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Gizmo.Update(Targets, View, Input, Transactions.Get());
	EXPECT_GT(Durin::Math::Length(Target->Transform.Translation - InitialLocation), 0.001);
	Input.bLeftMouseDown = false;
	Gizmo.Update(Targets, View, Input, Transactions.Get());
	ASSERT_TRUE(Transactions->CanUndo());
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(),
		MountedContentRevision);
	EXPECT_EQ(Transactions->GetUndoDescription(), "Translate 'Probe'");
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(),
		MountedContentRevision);
	ExpectVectorNear(Target->Transform.Translation, InitialLocation);
	Target->Capabilities = Durin::Editor::Level::ETransformGizmoCapability::Translate;
	Gizmo.SetMode(Durin::Editor::Level::ETransformGizmoMode::Rotate);
	Gizmo.Update(Targets, View, {}, nullptr);
	Durin::FSceneView UnsupportedView = View;
	Gizmo.AppendOverlayPrimitives(UnsupportedView);
	EXPECT_TRUE(UnsupportedView.OverlayPrimitives.empty());
}

TEST(FLevelEditorViewportClientTests, SmoothsCombinedFlyNavigationConsistentlyAcrossFrameRates)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "SmoothFlyWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "SmoothFlyLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::Editor::Level::FLevelEditorViewportClient SixtyHzClient;
	Durin::Editor::Level::FLevelEditorViewportClient OneTwentyHzClient;

	SimulateFlyNavigation(SixtyHzClient, Level, 1.0f / 60.0f, 60);
	SimulateFlyNavigation(OneTwentyHzClient, Level, 1.0f / 120.0f, 120);

	EXPECT_NEAR(
		SixtyHzClient.GetCameraTransform().GetYaw(),
		OneTwentyHzClient.GetCameraTransform().GetYaw(),
		1.e-4
	);
	EXPECT_NEAR(
		SixtyHzClient.GetCameraTransform().GetPitch(),
		OneTwentyHzClient.GetCameraTransform().GetPitch(),
		1.e-4
	);
	ExpectVectorNear(
		SixtyHzClient.GetCameraTransform().GetLocation(),
		OneTwentyHzClient.GetCameraTransform().GetLocation(),
		1.e-3
	);
}

TEST(FLevelEditorViewportClientTests, CapsFlyMovementAcrossAnAbnormallyLongFrame)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "LongFlyFrameWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "LongFlyFrameLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	const Durin::FVector3 InitialLocation = Client.GetCameraTransform().GetLocation();
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.DeltaSeconds = 1.0f;
	Input.bHovered = true;
	Input.bFocused = true;
	Input.bRightMousePressed = true;
	Input.bRightMouseDown = true;
	Input.bMoveForward = true;

	Client.Update(Level, nullptr, Input);

	const double Distance = Durin::Math::Length(Client.GetCameraTransform().GetLocation() - InitialLocation);
	EXPECT_GT(Distance, 0.0);
	EXPECT_LE(Distance, 5.0 / 30.0);
}

TEST(FLevelEditorViewportClientTests, AdjustsAndClampsFlySpeed)
{
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Client.SetMovementSpeed(10.0f);
	EXPECT_FLOAT_EQ(Client.GetMovementSpeed(), 10.0f);
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bHovered = true;
	Input.bFocused = true;
	Input.bRightMousePressed = true;
	Input.bRightMouseDown = true;
	Input.MouseWheel = 1.0f;
	Client.Update(nullptr, nullptr, Input);
	EXPECT_FLOAT_EQ(Client.GetMovementSpeed(), 12.0f);
	Client.SetMovementSpeed(-1.0f);
	EXPECT_FLOAT_EQ(Client.GetMovementSpeed(), 0.05f);
	Client.SetMovementSpeed(20000.0f);
	EXPECT_FLOAT_EQ(Client.GetMovementSpeed(), 10000.0f);
}

TEST(FLevelEditorViewportClientTests, SupportsDirectLocalAndWorldCameraMovement)
{
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	const Durin::FVector3 InitialLocation = Client.GetCameraTransform().GetLocation();
	const Durin::FVector3 Forward = Client.GetCameraTransform().GetForwardVector();
	Client.MoveCameraLocal({25.0, 0.0, 0.0});
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), InitialLocation + Forward * 25.0);
	Client.SetCameraLocation({100.0, 200.0, 300.0});
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), {100.0, 200.0, 300.0});
}

TEST(FLevelEditorViewportClientTests, PicksVisualizerForActorWithoutStaticMesh)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::Editor::Level::FLevelEditorCustomizationRegistry::Get();
	FCustomizationGuard Guard{Registry.RegisterComponentVisualizer(Durin::DCameraComponent::StaticClass(), std::make_shared<FTestComponentVisualizer>())};
	ASSERT_TRUE(Guard.Handle);
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "VisualizerPickingWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "VisualizerPickingLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::ACameraActor* Camera = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Camera, nullptr);
	Camera->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::FSceneView PickView;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, PickView));
	const Durin::Editor::Level::FViewportPickSubmission Pick = Client.SubmitViewportPick(Level, PickView, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Completion.Hit);
	EXPECT_EQ(Pick.Completion.Hit->Actor.Get(), Camera);
}

TEST(FLevelEditorViewportClientTests, PicksPlayerStartActorVisualizerWithoutSceneGeometry)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::Editor::Level::FLevelEditorCustomizationRegistry::Get();
	FCustomizationGuard Guard{Registry.RegisterActorVisualizer(
		Durin::APlayerStart::StaticClass(), Durin::Editor::Level::CreatePlayerStartActorVisualizer())};
	ASSERT_TRUE(Guard.Handle);
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "PlayerStartVisualizerPickingWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "PlayerStartVisualizerPickingLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	auto* PlayerStart = Level->SpawnActor<Durin::APlayerStart>("PlayerStart");
	ASSERT_NE(PlayerStart, nullptr);
	PlayerStart->GetRootComponent()->SetWorldLocation(
		Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::FSceneView PickView;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, PickView));
	const Durin::Editor::Level::FViewportPickSubmission Pick =
		Client.SubmitViewportPick(Level, PickView, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Completion.Hit);
	EXPECT_EQ(Pick.Completion.Hit->Actor.Get(), PlayerStart);
	EXPECT_EQ(Pick.Completion.Hit->Component.Get(), PlayerStart->GetRootComponent());
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
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Level);
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), Durin::Editor::Level::FLevelViewportCameraState{}.Location);
	EXPECT_FLOAT_EQ(Client.GetNearClip(), Durin::Editor::Level::FLevelEditorViewportClient::DefaultNearClip);
	EXPECT_FLOAT_EQ(Client.GetFarClip(), Durin::Editor::Level::FLevelEditorViewportClient::DefaultFarClip);
	Durin::Editor::Level::FLevelViewportCameraState Saved;
	Saved.Location = {8.0, 9.0, 10.0};
	Saved.OrbitPivot = {1.0, 2.0, 3.0};
	Saved.NearClip = 2.0f;
	Saved.FarClip = 10000.0f;
	Saved.ViewFadeStart = 7000.0f;
	Saved.ViewRenderDistance = 9000.0f;
	Client.InitializeForLevel(Level, &Saved);
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), Saved.Location);
	EXPECT_FLOAT_EQ(Client.GetNearClip(), Saved.NearClip);
	EXPECT_FLOAT_EQ(Client.GetFarClip(), Saved.FarClip);
	EXPECT_FLOAT_EQ(Client.GetViewFadeStart(), Saved.ViewFadeStart);
	EXPECT_FLOAT_EQ(Client.GetViewRenderDistance(), Saved.ViewRenderDistance);
	Client.InitializeForLevel(Level);
	EXPECT_FLOAT_EQ(Client.GetNearClip(), Durin::Editor::Level::FLevelEditorViewportClient::DefaultNearClip);
	EXPECT_FLOAT_EQ(Client.GetFarClip(), Durin::Editor::Level::FLevelEditorViewportClient::DefaultFarClip);
}

TEST(FTransformGizmoTests, BuildsNativeOverlayForSelectedActorModes)
{
	InitializeDObjectSystem();
	Durin::DPackage* Package = MakeRevisionTestPackage("GizmoPackage");
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(Package, "GizmoWorld");
	ASSERT_EQ(Package->FindTopLevelAsset(World->GetFName()), World);
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "GizmoLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Selected");
	ASSERT_NE(Actor, nullptr);
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(World);
	Context.SelectActor(Actor);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	Durin::Editor::Level::FLevelEditorViewportInput Input;
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
	Durin::Tests::FTestTransactorOwner TransformTransactions;
	TransformTransactions->EstablishSavedState(*Package);
	Durin::Editor::Level::FLevelEditorViewportInput DragInput;
	DragInput.bFocused = true;
	DragInput.bHovered = true;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.ViewportSize = {800.0f, 600.0f};
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	EXPECT_GT(Durin::Math::Length(Actor->GetActorTransform().Translation - InitialLocation), 0.001);
	Durin::FSceneView DraggedView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, DraggedView));
	ExpectVectorNear(Durin::FVector3(DraggedView.OverlayPrimitives.front().LocalToWorld[3]), Actor->GetActorTransform().Translation);
	DragInput.bCancel = true;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	ExpectVectorNear(Actor->GetActorTransform().Translation, InitialLocation);
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(TransformTransactions->CanUndo());
	EXPECT_TRUE(TransformTransactions->ConsumeEvents().empty());

	DragInput.bCancel = false;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.bLeftMouseDown = false;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(TransformTransactions->CanUndo());
	EXPECT_TRUE(TransformTransactions->ConsumeEvents().empty());

	DragInput.bCancel = false;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	DragInput.bLeftMouseDown = false;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	const std::vector<Durin::Editor::FTransactionEvent> TransformEvents = TransformTransactions->ConsumeEvents();
	ASSERT_EQ(TransformEvents.size(), 1);
	EXPECT_EQ(TransformEvents.front().Description, "Translate 'Selected'");
	EXPECT_NE(TransformEvents.front().Details.find("'Selected'"), std::string::npos);
	EXPECT_NE(TransformEvents.front().Details.find("Location"), std::string::npos);
	EXPECT_NE(TransformEvents.front().Details.find("Delta"), std::string::npos);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(TransformTransactions->Undo());
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(TransformTransactions->Redo());
	EXPECT_TRUE(Package->IsDirty());

	Client.GetTransformGizmo().SetMode(Durin::Editor::Level::ETransformGizmoMode::Rotate);
	Durin::FSceneView RotateView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, RotateView));
	EXPECT_EQ(RotateView.OverlayPrimitives.size(), 3u);
	Client.GetTransformGizmo().SetMode(Durin::Editor::Level::ETransformGizmoMode::Scale);
	EXPECT_EQ(Client.GetTransformGizmo().GetSpace(), Durin::Editor::Level::ETransformGizmoSpace::World);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::Editor::Level::ETransformGizmoSpace::Local);
	Client.GetTransformGizmo().SetSpace(Durin::Editor::Level::ETransformGizmoSpace::Parent);
	EXPECT_EQ(Client.GetTransformGizmo().GetSpace(), Durin::Editor::Level::ETransformGizmoSpace::Parent);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::Editor::Level::ETransformGizmoSpace::Local);
	Actor->GetRootComponent()->SetWorldRotation(Durin::Math::MakeQuaternionFromAxisAngleRadians(
		Durin::Math::HalfPi<Durin::FReal>(), Durin::FVectorConstants::Up));
	Client.GetTransformGizmo().Update(Context, RotateView, Input, nullptr);
	Durin::FSceneView ScaleView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, ScaleView));
	EXPECT_EQ(ScaleView.OverlayPrimitives.size(), 7u);
	ASSERT_FALSE(ScaleView.OverlayPrimitives.empty());
	ExpectVectorNear(
		Durin::Math::Normalize(Durin::FVector3(ScaleView.OverlayPrimitives.front().LocalToWorld[0])),
		Durin::FVectorConstants::Right);
	Client.GetTransformGizmo().SetMode(Durin::Editor::Level::ETransformGizmoMode::Translate);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::Editor::Level::ETransformGizmoSpace::Parent);

	Durin::ACameraActor* Parent = Level->SpawnActor<Durin::ACameraActor>("Parent");
	ASSERT_NE(Parent, nullptr);
	Actor->GetRootComponent()->SetWorldRotation(Durin::FQuatConstants::Identity);
	Parent->GetRootComponent()->SetWorldRotation(Durin::Math::MakeQuaternionFromAxisAngleRadians(
		Durin::Math::HalfPi<Durin::FReal>(), Durin::FVectorConstants::Up));
	ASSERT_TRUE(Actor->AttachToActor(Parent, Durin::EAttachmentTransformRule::KeepWorld));
	Client.GetTransformGizmo().Update(Context, ScaleView, Input, nullptr);
	Durin::FSceneView ParentView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, ParentView));
	ASSERT_FALSE(ParentView.OverlayPrimitives.empty());
	ExpectVectorNear(
		Durin::Math::Normalize(Durin::FVector3(ParentView.OverlayPrimitives.front().LocalToWorld[0])),
		Durin::FVectorConstants::Right);
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
	Component->SetWorldRotation(Durin::Math::MakeQuaternionFromAxisAngleDegrees(
		35.0, Durin::FVector3(0.0, 0.0, 1.0)));
	Component->SetWorldScale3D({2.0, 0.5, 1.5});

	std::vector<Durin::TObjectPtr<Durin::AActor>> Selection;
	Selection.emplace_back(Actor);
	Durin::Editor::Level::FLevelEditorViewportClient Client;
	Client.SetSelectedActors(Selection, Actor);
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	std::vector<const Durin::FSimpleElementLine*> BoundsLines;
	for (const Durin::FSimpleElement& Element : View.SimpleElements.GetElements())
	{
		if (Element.Type == Durin::ESimpleElementType::Line
			&& Element.DepthPriorityGroup
				== Durin::ESceneDepthPriorityGroup::World)
		{
			BoundsLines.push_back(
				&std::get<Durin::FSimpleElementLine>(Element.Value));
		}
	}
	// Flat debug geometry collapses four box edges; PDI rejects those zero-length lines.
	ASSERT_GE(BoundsLines.size(), 8u);
	const Durin::FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	const Durin::FMatrix LocalToWorld = Component->GetRenderMatrix();
	const Durin::FVector3 ExpectedMin = Durin::FVector3(
		LocalToWorld * Durin::FVector4(RenderData->LocalBounds.Min, 1.0));
	const Durin::FVector3 ExpectedMax = Durin::FVector3(
		LocalToWorld * Durin::FVector4(RenderData->LocalBounds.Max, 1.0));
	const auto ContainsPoint = [&BoundsLines](const Durin::FVector3& Expected) {
		return std::ranges::any_of(BoundsLines, [&Expected](const auto* Line) {
			return Durin::Math::Length(Line->Start - Expected) < 1.e-6
				|| Durin::Math::Length(Line->End - Expected) < 1.e-6;
		});
	};
	EXPECT_TRUE(ContainsPoint(ExpectedMin));
	EXPECT_TRUE(ContainsPoint(ExpectedMax));
	EXPECT_TRUE(std::ranges::all_of(BoundsLines, [](const auto* Line) {
		return Line->Color == Durin::FVector4f(1.0f, 0.72f, 0.19f, 1.0f);
	}));
}

TEST(FLevelEditorViewportClientTests, PicksClosestTriangleAndRejectsBoundsOnlyHit)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "PickingWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "PickingLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::Editor::Level::FLevelEditorViewportClient Client;
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
	NearActor->GetStaticMeshComponent()->SetWorldRotation(
		Durin::Math::MakeQuaternionFromAxisAngleDegrees(20.0, Forward));
	NearActor->GetStaticMeshComponent()->SetWorldScale3D({2.0, 0.5, 1.5});
	Durin::FSceneView PickView;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, PickView));
	const Durin::Editor::Level::FViewportPickSubmission CenterPick = Client.SubmitViewportPick(Level, PickView, {400.0f, 300.0f});
	ASSERT_EQ(CenterPick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	ASSERT_TRUE(CenterPick.Completion.Hit);
	EXPECT_EQ(CenterPick.Completion.Hit->Actor.Get(), NearActor);
	EXPECT_EQ(CenterPick.Completion.Hit->Component.Get(), NearActor->GetStaticMeshComponent());
	EXPECT_EQ(CenterPick.Completion.Hit->PrimitiveId, NearActor->GetStaticMeshComponent()->GetPrimitiveSceneId());
	const Durin::Editor::Level::FViewportPickSubmission EdgePick = Client.SubmitViewportPick(Level, PickView, {799.0f, 300.0f});
	EXPECT_EQ(EdgePick.Completion.Status, Durin::Editor::Level::EViewportPickStatus::Completed);
	EXPECT_FALSE(EdgePick.Completion.Hit);
}

TEST(FViewportSelectionTests, PrefersViewportClientThenControllerTargetThenPrimaryCamera)
{
	InitializeDObjectSystem();
	FTestEngine Engine;
	FTestViewportClient Client;
	Client.SetViewSettings({
		.Mode = {
			.RenderMode = Durin::ERenderMode::Unlit,
			.RasterMode = Durin::ERasterMode::Wireframe,
		},
		.PostProcess = {.bEnableFXAA = false, .ExposureEV = -2.0f},
		.DirectionalShadow = {
			.DiagnosticMode =
				Durin::EDirectionalShadowDiagnosticMode::CascadeIndex,
			.bEnableContactShadows = true,
		},
	});
	auto ClientViewport = Durin::FSceneViewport::CreateOffscreen(&Client);
	Engine.SetTestViewport(ClientViewport);
	const Durin::FSceneView ClientView = Engine.BuildMainSceneView(640, 480);
	ExpectVectorNear(ClientView.ViewLocation, {11.0, 12.0, 13.0});
	EXPECT_FALSE(ClientView.Settings.PostProcess.bEnableFXAA);
	EXPECT_TRUE(ClientView.Settings.DirectionalShadow.bEnableContactShadows);
	EXPECT_FLOAT_EQ(ClientView.Settings.PostProcess.ExposureEV, -2.0f);
	EXPECT_EQ(ClientView.Settings.Mode.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(ClientView.Settings.Mode.RasterMode, Durin::ERasterMode::Wireframe);
	EXPECT_EQ(
		ClientView.Settings.DirectionalShadow.DiagnosticMode,
		Durin::EDirectionalShadowDiagnosticMode::CascadeIndex);

	Client.SetViewSettings({});
	EXPECT_EQ(ClientView.Settings.Mode.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(
		Engine.BuildMainSceneView(640, 480).Settings.Mode.RenderMode,
		Durin::ERenderMode::Lit);

	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(&Engine, "ViewportTestWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportTestLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(CameraActor, nullptr);
	CameraActor->GetCameraComponent()->SetWorldLocation({7.0, 8.0, 9.0});
	Durin::ACameraActor* ControllerCamera = Level->SpawnActor<Durin::ACameraActor>("ControllerCamera");
	ASSERT_NE(ControllerCamera, nullptr);
	ControllerCamera->GetCameraComponent()->SetWorldLocation({21.0, 22.0, 23.0});
	ASSERT_NE(Level->SpawnActor<Durin::APlayerStart>("Start"), nullptr);
	ASSERT_TRUE(World->BeginPlay({
		.GameModeClass = Durin::AGameMode::StaticClass(),
		.ViewTargetOverride = ControllerCamera}));
	Engine.SetTestWorld(World);
	Durin::FViewportClient FallbackClient;
	FallbackClient.SetViewSettings({
		.Mode = {
			.RenderMode = Durin::ERenderMode::Unlit,
			.RasterMode = Durin::ERasterMode::Wireframe,
		},
		.PostProcess = {.bEnableFXAA = false, .ExposureEV = 1.5f},
		.DirectionalShadow = {
			.DiagnosticMode =
				Durin::EDirectionalShadowDiagnosticMode::CascadeIndex,
			.bEnableContactShadows = true,
		},
	});
	Engine.SetTestViewport(Durin::FSceneViewport::CreateOffscreen(&FallbackClient));
	const Durin::FSceneView FallbackView = Engine.BuildMainSceneView(640, 480);
	ExpectVectorNear(FallbackView.ViewLocation, {21.0, 22.0, 23.0});
	EXPECT_FALSE(FallbackView.Settings.PostProcess.bEnableFXAA);
	EXPECT_TRUE(FallbackView.Settings.DirectionalShadow.bEnableContactShadows);
	EXPECT_FLOAT_EQ(FallbackView.Settings.PostProcess.ExposureEV, 1.5f);
	EXPECT_EQ(FallbackView.Settings.Mode.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(FallbackView.Settings.Mode.RasterMode, Durin::ERasterMode::Wireframe);
	EXPECT_EQ(
		FallbackView.Settings.DirectionalShadow.DiagnosticMode,
		Durin::EDirectionalShadowDiagnosticMode::CascadeIndex);

	Engine.SetTestViewport(nullptr);
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {21.0, 22.0, 23.0});
	ASSERT_TRUE(World->DestroyActor(ControllerCamera));
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {7.0, 8.0, 9.0});
	World->EndPlay();
	Engine.SetTestWorld(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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
	Engine.SetTestWorld(nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
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

	Durin::Editor::Level::FCameraPreviewViewportClient Client;
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
