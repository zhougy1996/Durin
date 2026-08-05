#include "ViewportTestSupport.h"
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

	class FProbeEditMode final : public Durin::ILevelViewportEditMode
	{
	public:
		explicit FProbeEditMode(std::shared_ptr<FEditModeProbe> InProbe) : Probe(std::move(InProbe)) {}
		auto Enter(Durin::FLevelEditorContext&) -> void override { ++Probe->EnterCount; }
		auto Exit(Durin::FLevelEditorContext&, bool bForced) -> void override { ++Probe->ExitCount; Probe->ForcedExitCount += bForced; }
		auto Tick(Durin::FLevelEditorContext&, Durin::FLevelEditorViewportClient&, const Durin::FSceneView&,
			Durin::FLevelEditorViewportInput&, Durin::FEditorTransactionManager*) -> bool override { ++Probe->TickCount; return true; }
	private:
		std::shared_ptr<FEditModeProbe> Probe;
	};

	class FProbeTransformTarget final : public Durin::ITransformGizmoTarget
	{
	public:
		auto IsValid() const -> bool override { return bValid; }
		auto GetIdentity() const -> const void* override { return this; }
		auto GetTransform() const -> Durin::FTransform override { return Transform; }
		auto SetTransform(const Durin::FTransform& Value) -> bool override { Transform = Value; return bValid; }
		auto GetLabel() const -> std::string override { return "Probe"; }
		auto GetCapabilities() const -> Durin::ETransformGizmoCapability override { return Capabilities; }
		Durin::FTransform Transform;
		Durin::ETransformGizmoCapability Capabilities = Durin::ETransformGizmoCapability::All;
		bool bValid = true;
	};

	auto SimulateFlyNavigation(
		Durin::FLevelEditorViewportClient& Client,
		Durin::DLevel* Level,
		float DeltaSeconds,
		Durin::int32 FrameCount
	) -> void
	{
		Durin::FLevelEditorViewportInput Input;
		Input.DeltaSeconds = DeltaSeconds;
		Input.bHovered = true;
		Input.bFocused = true;
		Input.bRightMouseDown = true;
		Input.bMoveForward = true;
		Input.MouseDelta = {90.0f * DeltaSeconds, -30.0f * DeltaSeconds};
		for (Durin::int32 Frame = 0; Frame < FrameCount; ++Frame)
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
	auto& Registry = Durin::FLevelViewportEditModeRegistry::Get();
	const Durin::FLevelViewportEditModeHandle Handle = Registry.Register({
		.Id = "Probe",
		.DisplayName = "Probe",
		.Priority = 10,
		.CanActivate = [&bAvailable](const Durin::FLevelEditorContext&) { return bAvailable; },
		.Factory = [&] { return std::make_unique<FProbeEditMode>(FactoryCount++ == 0 ? ProbeA : ProbeB); },
	});
	ASSERT_TRUE(Handle);
	Durin::FLevelEditorContext ContextA;
	Durin::FLevelEditorContext ContextB;
	Durin::FLevelViewportEditModeManager ManagerA;
	Durin::FLevelViewportEditModeManager ManagerB;
	ASSERT_TRUE(ManagerA.Activate("Probe", ContextA));
	ASSERT_TRUE(ManagerB.Activate("Probe", ContextB));
	EXPECT_EQ(FactoryCount, 2);
	EXPECT_NE(ManagerA.GetActiveMode(), ManagerB.GetActiveMode());
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	Durin::FLevelEditorViewportInput Input;
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
	Durin::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	auto Target = std::make_shared<FProbeTransformTarget>();
	Target->Transform.Translation = Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0;
	Durin::FTransformGizmoTargetSet Targets{{Target}, "Probes"};
	Durin::FTransformGizmo Gizmo;
	Durin::FLevelEditorViewportInput Input;
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
	Durin::FEditorTransactionManager Transactions;
	Input.bFocused = true;
	Input.bHovered = true;
	Input.bLeftMousePressed = true;
	Input.bLeftMouseDown = true;
	Input.MousePosition = HandleScreen;
	Gizmo.Update(Targets, View, Input, &Transactions);
	ASSERT_TRUE(Gizmo.IsDragging());
	Input.bLeftMousePressed = false;
	Input.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Gizmo.Update(Targets, View, Input, &Transactions);
	EXPECT_GT(Durin::Math::Length(Target->Transform.Translation - InitialLocation), 0.001);
	Input.bLeftMouseDown = false;
	Gizmo.Update(Targets, View, Input, &Transactions);
	ASSERT_TRUE(Transactions.CanUndo());
	EXPECT_EQ(Transactions.GetUndoDescription(), "Translate 'Probe'");
	ASSERT_TRUE(Transactions.Undo());
	ExpectVectorNear(Target->Transform.Translation, InitialLocation);
	Target->Capabilities = Durin::ETransformGizmoCapability::Translate;
	Gizmo.SetMode(Durin::ETransformGizmoMode::Rotate);
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
	Durin::FLevelEditorViewportClient SixtyHzClient;
	Durin::FLevelEditorViewportClient OneTwentyHzClient;

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
	Durin::FLevelEditorViewportClient Client;
	const Durin::FVector3 InitialLocation = Client.GetCameraTransform().GetLocation();
	Durin::FLevelEditorViewportInput Input;
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
	Durin::DPackage* Package = MakeRevisionTestPackage("GizmoPackage");
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(Package, "GizmoWorld");
	ASSERT_TRUE(Package->SetAsset(World));
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
	Durin::FEditorTransactionManager TransformTransactions;
	TransformTransactions.EstablishSavedState(*Package);
	Durin::FLevelEditorViewportInput DragInput;
	DragInput.bFocused = true;
	DragInput.bHovered = true;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.ViewportSize = {800.0f, 600.0f};
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	EXPECT_GT(Durin::Math::Length(Actor->GetActorTransform().Translation - InitialLocation), 0.001);
	Durin::FSceneView DraggedView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, DraggedView));
	ExpectVectorNear(Durin::FVector3(DraggedView.OverlayPrimitives.front().LocalToWorld[3]), Actor->GetActorTransform().Translation);
	DragInput.bCancel = true;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	ExpectVectorNear(Actor->GetActorTransform().Translation, InitialLocation);
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(TransformTransactions.CanUndo());
	EXPECT_TRUE(TransformTransactions.ConsumeEvents().empty());

	DragInput.bCancel = false;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.bLeftMouseDown = false;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(TransformTransactions.CanUndo());
	EXPECT_TRUE(TransformTransactions.ConsumeEvents().empty());

	DragInput.bCancel = false;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.MousePosition = HandleScreen;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	DragInput.bLeftMouseDown = false;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, &TransformTransactions);
	const std::vector<Durin::FEditorTransactionEvent> TransformEvents = TransformTransactions.ConsumeEvents();
	ASSERT_EQ(TransformEvents.size(), 1);
	EXPECT_EQ(TransformEvents.front().Description, "Translate 'Selected'");
	EXPECT_NE(TransformEvents.front().Details.find("'Selected'"), std::string::npos);
	EXPECT_NE(TransformEvents.front().Details.find("Location"), std::string::npos);
	EXPECT_NE(TransformEvents.front().Details.find("Delta"), std::string::npos);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(TransformTransactions.Undo());
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(TransformTransactions.Redo());
	EXPECT_TRUE(Package->IsDirty());

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
	Client.GetTransformGizmo().SetMode(Durin::ETransformGizmoMode::Translate);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::ETransformGizmoSpace::Parent);

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
	NearActor->GetStaticMeshComponent()->SetWorldRotation(
		Durin::Math::MakeQuaternionFromAxisAngleDegrees(20.0, Forward));
	NearActor->GetStaticMeshComponent()->SetWorldScale3D({2.0, 0.5, 1.5});
	EXPECT_EQ(Client.PickActor(Level, {400.0f, 300.0f}, {800.0f, 600.0f}), NearActor);
	EXPECT_EQ(Client.PickActor(Level, {799.0f, 300.0f}, {800.0f, 600.0f}), nullptr);
}

TEST(FViewportSelectionTests, PrefersViewportClientAndFallsBackToPrimaryCamera)
{
	InitializeDObjectSystem();
	FTestEngine Engine;
	FTestViewportClient Client;
	Client.SetViewSettings({
		.bEnableFXAA = false,
		.RenderMode = Durin::ERenderMode::Unlit,
		.RasterMode = Durin::ERasterMode::Wireframe,
	});
	auto ClientViewport = std::make_shared<Durin::FSceneViewport>(&Client, std::shared_ptr<Durin::MViewport>{});
	Engine.SetTestViewport(ClientViewport);
	const Durin::FSceneView ClientView = Engine.BuildMainSceneView(640, 480);
	ExpectVectorNear(ClientView.ViewLocation, {11.0, 12.0, 13.0});
	EXPECT_FALSE(ClientView.Settings.bEnableFXAA);
	EXPECT_EQ(ClientView.Settings.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(ClientView.Settings.RasterMode, Durin::ERasterMode::Wireframe);

	Client.SetViewSettings({});
	EXPECT_EQ(ClientView.Settings.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(
		Engine.BuildMainSceneView(640, 480).Settings.RenderMode,
		Durin::ERenderMode::Lit);

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
