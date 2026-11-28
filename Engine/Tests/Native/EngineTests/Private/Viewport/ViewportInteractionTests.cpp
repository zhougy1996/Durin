#include "Viewport/TransformProxy.h"
#include "ViewportTestSupport.h"
#include "Viewport/ViewportPickingService.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Actors/GameMode.h"
#include "Actors/PlayerStart.h"
#include "EditorModeManager.h"
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

	class FProbeEditMode final : public Durin::LevelEditor::ILevelViewportEditMode
	{
	public:
		explicit FProbeEditMode(std::shared_ptr<FEditModeProbe> InProbe) : Probe(std::move(InProbe)) {}
		auto Enter(Durin::LevelEditor::FLevelEditorContext&) -> void override { ++Probe->EnterCount; }
		auto Exit(Durin::LevelEditor::FLevelEditorContext&, bool bForced) -> void override { ++Probe->ExitCount; Probe->ForcedExitCount += bForced; }
		auto Tick(Durin::LevelEditor::FLevelEditorContext&, Durin::LevelEditor::FLevelEditorViewportClient&, const Durin::FSceneView&,
			Durin::LevelEditor::FLevelEditorViewportInput&, Durin::DTransactor*) -> bool override { ++Probe->TickCount; return true; }
	private:
		std::shared_ptr<FEditModeProbe> Probe;
	};

	class FProbeTransformTarget final : public std::enable_shared_from_this<FProbeTransformTarget>
	{
	public:
		Durin::TStrongObjectPtr<Durin::DSceneComponent> Owner{
			Durin::NewObject<Durin::DSceneComponent>(nullptr, Durin::NAME_None, Durin::EObjectFlags::Transactional)};
		mutable size_t TransformReads = 0;
		Durin::LevelEditor::FTopologyRevisionReader RevisionReader = nullptr;
		template<typename TSelection>
		auto Bind(TSelection& Selection) -> void
		{
			using namespace Durin::LevelEditor;
			auto Self = shared_from_this();
			FTransformComponentOptions Options;
			Options.Identity = {this};
			Options.Label = "Probe";
			Options.Capabilities = Capabilities;
			Options.ReadRevision = RevisionReader;
			Options.Validate = [Self](uint64) { return Self->bValid; };
			Selection.AddComponentCustom(Owner.Get(), [Self] { return Self->GetTransform(); },
				[Self](const Durin::FTransform& Value, FTransformGizmoWriteContext Context) { return Self->SetTransform(Value, Context); },
				std::move(Options));
		}
		auto GetTransform() const -> Durin::FTransform { ++TransformReads; return Owner->GetRelativeTransform(); }
		auto SetTransform(const Durin::FTransform& Value, Durin::LevelEditor::FTransformGizmoWriteContext = {}) -> bool
		{
			if (Setter) return Setter(Value);
			Owner->SetRelativeTransform(Value);
			return bValid;
		}
		void BindPackage(Durin::DPackage* Package)
		{
			const auto Initial = GetTransform();
			Owner = Durin::NewObject<Durin::DSceneComponent>(Package, "ProbeComponent", Durin::EObjectFlags::Transactional);
			Owner->SetRelativeTransform(Initial);
		}
		std::function<bool(const Durin::FTransform&)> Setter;
		Durin::LevelEditor::ETransformGizmoCapability Capabilities = Durin::LevelEditor::ETransformGizmoCapability::All;
		bool bValid = true;
	};

	auto SimulateFlyNavigation(
		Durin::LevelEditor::FLevelEditorViewportClient& Client,
		Durin::DLevel* Level,
		float DeltaSeconds,
		int32 FrameCount
	) -> void
	{
		Durin::LevelEditor::FLevelEditorViewportInput Input;
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
	auto& Registry = Durin::LevelEditor::FLevelViewportEditModeRegistry::Get();
	const Durin::LevelEditor::FLevelViewportEditModeHandle Handle = Registry.Register({
		.Id = "Probe",
		.DisplayName = "Probe",
		.Priority = 10,
		.CanActivate = [&bAvailable](const Durin::LevelEditor::FLevelEditorContext&) { return bAvailable; },
		.Factory = [&] { return std::make_unique<FProbeEditMode>(FactoryCount++ == 0 ? ProbeA : ProbeB); },
	});
	ASSERT_TRUE(Handle);
	Durin::LevelEditor::FLevelEditorContext ContextA;
	Durin::LevelEditor::FLevelEditorContext ContextB;
	FScopedSelectModeRegistration SelectRegistration;
	Durin::LevelEditor::FEditorModeTools ManagerA;
	Durin::LevelEditor::FEditorModeTools ManagerB;
	ASSERT_TRUE(ManagerA.Activate("Probe", ContextA));
	ASSERT_TRUE(ManagerB.Activate("Probe", ContextB));
	EXPECT_EQ(FactoryCount, 2);
	EXPECT_NE(ManagerA.GetActiveMode(), ManagerB.GetActiveMode());
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	Durin::LevelEditor::FLevelEditorViewportInput Input;
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

TEST(FTransformProxyTests, SharedFrameRotatesAndScalesAboutTheInitialPivot)
{
	using namespace Durin;
	using namespace Durin::LevelEditor;
	auto First = std::make_shared<FProbeTransformTarget>();
	auto Second = std::make_shared<FProbeTransformTarget>();
	const FVector3 Pivot(5.0, 6.0, 7.0);
	const FQuat Basis = Math::MakeQuaternionFromAxisAngleRadians(Math::HalfPi<FReal>(), FVectorConstants::Up);
	const FVector3 Axis = Basis * FVectorConstants::Forward;
	First->Owner->SetRelativeRotation(Basis);
	First->Owner->SetRelativeLocation(Pivot + Axis);
	Second->Owner->SetRelativeLocation(Pivot - Axis);
	TStrongObjectPtr<DTransformProxy> Proxy(NewObject<DTransformProxy>(nullptr, NAME_None));
	First->Bind(*Proxy.Get());
	Second->Bind(*Proxy.Get());
	Proxy->BeginTransformEditSequence();
	auto Frame = Proxy->GetInitialTransform();
	Frame.Rotation = Basis * Frame.Rotation;
	ASSERT_TRUE(Proxy->SetTransform(Frame));
	ExpectVectorNear(First->GetTransform().Translation, Pivot + Basis * Axis);
	ExpectVectorNear(Second->GetTransform().Translation, Pivot - Basis * Axis);
	Frame = Proxy->GetInitialTransform();
	Frame.Scale3D.x = 2.0;
	ASSERT_TRUE(Proxy->SetTransform(Frame));
	ExpectVectorNear(First->GetTransform().Translation, Pivot + 2.0 * Axis);
	ExpectVectorNear(Second->GetTransform().Translation, Pivot - 2.0 * Axis);
}

TEST(FTransformProxyTests, ComponentRegistrationBindsTheSelectionPivotAndOriginalComponents)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DSceneComponent> First(NewObject<DSceneComponent>(nullptr, NAME_None));
	TStrongObjectPtr<DSceneComponent> Second(NewObject<DSceneComponent>(nullptr, NAME_None));
	Second->SetRelativeLocation(FVector3(4.0, 0.0, 0.0));
	TStrongObjectPtr<DTransformProxy> Proxy(NewObject<DTransformProxy>(nullptr, NAME_None));
	Proxy->AddComponent(First.Get());
	Proxy->AddComponent(Second.Get());
	auto Frame = Proxy->GetTransform();
	ExpectVectorNear(Frame.Translation, FVector3(2.0, 0.0, 0.0));
	Frame.Translation *= 2.0;
	ASSERT_TRUE(Proxy->SetTransform(Frame));
	ExpectVectorNear(First->GetWorldTransform().Translation, FVector3(2.0, 0.0, 0.0));
	ExpectVectorNear(Second->GetWorldTransform().Translation, FVector3(6.0, 0.0, 0.0));
}

TEST(FTransformProxyTests, RestoreResetsAllTargetsAfterAnOutsideWrite)
{
	using namespace Durin;
	using namespace Durin::LevelEditor;
	InitializeDObjectSystem();
	auto First = std::make_shared<FProbeTransformTarget>();
	auto Second = std::make_shared<FProbeTransformTarget>();
	const auto Initial = First->GetTransform();
	TStrongObjectPtr<DTransformProxy> Proxy(NewObject<DTransformProxy>(nullptr, NAME_None));
	First->Bind(*Proxy.Get());
	Second->Bind(*Proxy.Get());
	Proxy->BeginTransformEditSequence();
	auto Frame = Proxy->GetInitialTransform();
	Frame.Translation.x += 5.0;
	ASSERT_TRUE(Proxy->SetTransform(Frame));
	Second->Owner->SetRelativeLocation(Second->Owner->GetRelativeLocation() + FVector3(2.0, 0.0, 0.0));
	ASSERT_TRUE(Proxy->Restore());
	ExpectVectorNear(First->GetTransform().Translation, Initial.Translation);
	ExpectVectorNear(Second->GetTransform().Translation, Initial.Translation);
}

TEST(FTransformGizmoTests, ManipulatesGenericTargetsAndCommitsWithoutActorKnowledge)
{
	InitializeDObjectSystem();
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, View));
	auto Target = std::make_shared<FProbeTransformTarget>();
	Target->Owner->SetRelativeLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::TStrongObjectPtr<Durin::DTransformProxy> Proxy(Durin::NewObject<Durin::DTransformProxy>(nullptr, Durin::NAME_None));
	Target->Bind(*Proxy.Get());
	auto& Gizmo = Client.GetTransformGizmo();
	Durin::Tests::FTestEditorTransactorOwner Transactions;
	Gizmo.SetActiveTarget(Proxy.Get(), Transactions.Get());
	Durin::LevelEditor::FLevelEditorViewportInput Input;
	Input.ViewportSize = {800.0f, 600.0f};
	Gizmo.Update(View, Input);
	Durin::FSceneView OverlayView = View;
	Gizmo.AppendOverlayPrimitives(OverlayView);
	ASSERT_FALSE(OverlayView.OverlayPrimitives.empty());
	const Durin::FVector3 InitialLocation = Target->GetTransform().Translation;
	const Durin::FVector3 HandlePoint = Durin::FVector3(OverlayView.OverlayPrimitives.front().LocalToWorld * Durin::FVector4(0.65, 0.0, 0.0, 1.0));
	Durin::FVector2f CenterScreen;
	Durin::FVector2f HandleScreen;
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, InitialLocation, CenterScreen));
	ASSERT_TRUE(Durin::SceneViewProjection::ProjectWorldToViewport(View, HandlePoint, HandleScreen));
	Input.bFocused = true;
	Input.bHovered = true;
	Input.bLeftMousePressed = true;
	Input.bLeftMouseDown = true;
	Input.MousePosition = HandleScreen;
	Gizmo.Update(View, Input);
	ASSERT_TRUE(Gizmo.IsDragging());
	Input.bLeftMousePressed = false;
	Input.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Gizmo.Update(View, Input);
	EXPECT_GT(Durin::Math::Length(Target->GetTransform().Translation - InitialLocation), 0.001);
	Input.bLeftMouseDown = false;
	Gizmo.Update(View, Input);
	ASSERT_TRUE(Transactions->CanUndo());
	EXPECT_EQ(Transactions->GetUndoContext(false).Description, "Translate 'Probe'");
	const auto BeforeRejectedUndo = Target->GetTransform();
	ASSERT_TRUE(Transactions->Undo());
	ExpectVectorNear(Target->GetTransform().Translation, InitialLocation);
	ASSERT_TRUE(Transactions->Redo());
	Target->bValid = false;
	EXPECT_FALSE(Transactions->Undo());
	ExpectVectorNear(Target->GetTransform().Translation, BeforeRejectedUndo.Translation);
	EXPECT_EQ(Transactions->GetUndoCount(), 1u);
	Target->bValid = true;
	Target->Capabilities = Durin::LevelEditor::ETransformGizmoCapability::Translate;
	Proxy = Durin::NewObject<Durin::DTransformProxy>(nullptr, Durin::NAME_None);
	Target->Bind(*Proxy.Get());
	Gizmo.SetActiveTarget(Proxy.Get());
	Gizmo.SetMode(Durin::LevelEditor::ETransformGizmoMode::Rotate);
	Gizmo.Update(View, {});
	Durin::FSceneView UnsupportedView = View;
	Gizmo.AppendOverlayPrimitives(UnsupportedView);
	EXPECT_TRUE(UnsupportedView.OverlayPrimitives.empty());
}

TEST(FLevelEditorViewportClientTests, SmoothsCombinedFlyNavigationConsistentlyAcrossFrameRates)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "SmoothFlyWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "SmoothFlyLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::LevelEditor::FLevelEditorViewportClient SixtyHzClient;
	Durin::LevelEditor::FLevelEditorViewportClient OneTwentyHzClient;

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
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "LongFlyFrameLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	const Durin::FVector3 InitialLocation = Client.GetCameraTransform().GetLocation();
	Durin::LevelEditor::FLevelEditorViewportInput Input;
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
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	Client.SetMovementSpeed(10.0f);
	EXPECT_FLOAT_EQ(Client.GetMovementSpeed(), 10.0f);
	Durin::LevelEditor::FLevelEditorViewportInput Input;
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
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	const Durin::FVector3 InitialLocation = Client.GetCameraTransform().GetLocation();
	const Durin::FVector3 Forward = Client.GetCameraTransform().GetForwardVector();
	Client.MoveCameraLocal({25.0, 0.0, 0.0});
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), InitialLocation + Forward * 25.0);
	Client.SetCameraLocation({100.0, 200.0, 300.0});
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), {100.0, 200.0, 300.0});
}

TEST(FLevelEditorViewportClientTests, SubmitsVisualizerForActorWithoutStaticMeshAsHitProxies)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::LevelEditor::FLevelEditorCustomizationRegistry::Get();
	FCustomizationGuard Guard{Registry.RegisterComponentVisualizer(Durin::DCameraComponent::StaticClass(), std::make_shared<FTestComponentVisualizer>())};
	ASSERT_TRUE(Guard.Handle);
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "VisualizerPickingWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "VisualizerPickingLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	Durin::ACameraActor* Camera = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Camera, nullptr);
	Camera->GetCameraComponent()->SetWorldLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::FSceneView PickView;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, PickView));
	auto Backend = std::make_unique<FCaptureHitProxyBackend>();
	auto* Capture = Backend.get();
	Client.SetPickingBackendForTesting(std::move(Backend));
	const Durin::LevelEditor::FViewportPickResult Pick = Client.PickViewport(Level, PickView, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Status, Durin::LevelEditor::EViewportPickStatus::Completed);
	ASSERT_FALSE(Capture->Captured.Overlays.empty());
	ASSERT_TRUE(Pick.Hit);
	EXPECT_EQ(Pick.Hit->Actor.Get(), Camera);
	EXPECT_EQ(Pick.Hit->Kind, Durin::LevelEditor::EViewportPickHitKind::EditorVisualization);
	EXPECT_FALSE(Capture->Captured.Overlays.front().bForeground);
}

TEST(FLevelEditorViewportClientTests, SubmitsPlayerStartActorVisualizerWithoutSceneGeometryAsHitProxies)
{
	InitializeDObjectSystem();
	auto& Registry = Durin::LevelEditor::FLevelEditorCustomizationRegistry::Get();
	FCustomizationGuard Guard{Registry.RegisterActorVisualizer(
		Durin::APlayerStart::StaticClass(), Durin::LevelEditor::CreatePlayerStartActorVisualizer())};
	ASSERT_TRUE(Guard.Handle);
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "PlayerStartVisualizerPickingWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "PlayerStartVisualizerPickingLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	auto* PlayerStart = Level->SpawnActor<Durin::APlayerStart>("PlayerStart");
	ASSERT_NE(PlayerStart, nullptr);
	PlayerStart->GetRootComponent()->SetWorldLocation(
		Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
	Durin::FSceneView PickView;
	ASSERT_TRUE(Client.BuildViewMatrices(800, 600, PickView));
	auto Backend = std::make_unique<FCaptureHitProxyBackend>();
	auto* Capture = Backend.get();
	Client.SetPickingBackendForTesting(std::move(Backend));
	const Durin::LevelEditor::FViewportPickResult Pick =
		Client.PickViewport(Level, PickView, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Status, Durin::LevelEditor::EViewportPickStatus::Completed);
	ASSERT_FALSE(Capture->Captured.Overlays.empty());
	ASSERT_TRUE(Pick.Hit);
	EXPECT_EQ(Pick.Hit->Actor.Get(), PlayerStart);
	EXPECT_EQ(Pick.Hit->Kind, Durin::LevelEditor::EViewportPickHitKind::EditorVisualization);
	EXPECT_TRUE(Capture->Captured.Overlays.front().bForeground);
}

TEST(FLevelEditorViewportClientTests, ResetsIndependentViewUnlessSavedStateExists)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "ViewportResetWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportResetLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Camera = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Camera, nullptr);
	Camera->GetCameraComponent()->SetWorldLocation({100.0, 200.0, 300.0});
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	Client.InitializeForLevel(Level);
	ExpectVectorNear(Client.GetCameraTransform().GetLocation(), Durin::LevelEditor::FLevelViewportCameraState{}.Location);
	EXPECT_FLOAT_EQ(Client.GetNearClip(), Durin::LevelEditor::FLevelEditorViewportClient::DefaultNearClip);
	EXPECT_FLOAT_EQ(Client.GetFarClip(), Durin::LevelEditor::FLevelEditorViewportClient::DefaultFarClip);
	Durin::LevelEditor::FLevelViewportCameraState Saved;
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
	EXPECT_FLOAT_EQ(Client.GetNearClip(), Durin::LevelEditor::FLevelEditorViewportClient::DefaultNearClip);
	EXPECT_FLOAT_EQ(Client.GetFarClip(), Durin::LevelEditor::FLevelEditorViewportClient::DefaultFarClip);
}

TEST(FTransformGizmoTests, BuildsNativeOverlayForSelectedActorModes)
{
	InitializeDObjectSystem();
	FScopedSelectModeRegistration SelectRegistration;
	Durin::DPackage* Package = MakeRevisionTestPackage("GizmoPackage");
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(Package, "GizmoWorld");
	ASSERT_TRUE(World->SetWorldType(Durin::EWorldType::Editor));
	EXPECT_NO_THROW(World->InitializeSubsystems());
	ASSERT_EQ(Package->FindTopLevelAsset(World->GetFName()), World);
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "GizmoLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* Actor = Level->SpawnActor<Durin::ACameraActor>("Selected");
	ASSERT_NE(Actor, nullptr);
	Durin::LevelEditor::FLevelEditorContext Context;
	Context.Synchronize(World);
	Context.SelectActor(Actor);
	Durin::LevelEditor::FLevelEditorViewportClient Client;
	Durin::FSceneView View;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, View));
	Durin::LevelEditor::FLevelEditorViewportInput Input;
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
	Durin::Tests::FTestEditorTransactorOwner TransformTransactions;
	Package->ClearDirty();
	Durin::LevelEditor::FLevelEditorViewportInput DragInput;
	DragInput.bFocused = true;
	DragInput.bHovered = true;
	DragInput.bLeftMousePressed = true;
	DragInput.bLeftMouseDown = true;
	DragInput.ViewportSize = {800.0f, 600.0f};
	DragInput.MousePosition = HandleScreen;
	Durin::LevelEditor::FEditorModeTools Modes;
	Modes.Tick(Context, Client, TranslateView, DragInput, TransformTransactions.Get());
	ASSERT_TRUE(Client.GetTransformGizmo().IsDragging());
	DragInput.bLeftMousePressed = false;
	DragInput.MousePosition += Durin::Math::Normalize(HandleScreen - CenterScreen) * 30.0f;
	Client.GetTransformGizmo().Update(Context, TranslateView, DragInput, TransformTransactions.Get());
	EXPECT_GT(Durin::Math::Length(Actor->GetActorTransform().Translation - InitialLocation), 0.001);
	Durin::FSceneView DraggedView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, DraggedView));
	ExpectVectorNear(Durin::FVector3(DraggedView.OverlayPrimitives.front().LocalToWorld[3]), Actor->GetActorTransform().Translation);
	EXPECT_TRUE(Package->IsDirty());
	Modes.DeactivateAllModes();
	EXPECT_EQ(Modes.GetActiveMode(), nullptr);
	EXPECT_FALSE(Client.GetTransformGizmo().IsDragging());
	ExpectVectorNear(Actor->GetActorTransform().Translation, InitialLocation);
	EXPECT_TRUE(Package->IsDirty());
	Package->ClearDirty();
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(TransformTransactions->CanUndo());

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
	ASSERT_TRUE(TransformTransactions->CanUndo());
	ASSERT_EQ(TransformTransactions->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(TransformTransactions->Undo());
	EXPECT_FALSE(Package->IsDirty());

	auto* Peer = Level->SpawnActor<Durin::ACameraActor>("Peer");
	ASSERT_NE(Peer, nullptr);
	auto* Third = Level->SpawnActor<Durin::ACameraActor>("OtherSelection");
	ASSERT_NE(Third, nullptr);
	const Durin::FVector3 ThirdLocation(7.0, 3.0, 5.0);
	ASSERT_TRUE(Third->SetActorLocation(ThirdLocation));
	Context.SetSelectedActors({Actor, Peer}, Actor);
	Package->ClearDirty();
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
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(TransformTransactions->GetUndoContext().Description, "Translate 2 Actors");
	const auto* Edit = TransformTransactions->GetTransaction(0);
	ASSERT_NE(Edit, nullptr);
	EXPECT_EQ(Edit->GetRecordCount(), 3u);
	Durin::DTransformProxy* RecordedProxy = nullptr;
	for (auto* Object : Edit->GetTransactionObjects())
	{
		if (auto* Proxy = Durin::Cast<Durin::DTransformProxy>(Object))
		{
			EXPECT_EQ(RecordedProxy, nullptr);
			RecordedProxy = Proxy;
		}
	}
	ASSERT_NE(RecordedProxy, nullptr);
	EXPECT_FALSE(Edit->ContainsObject(Actor));
	EXPECT_FALSE(Edit->ContainsObject(Peer));
	EXPECT_TRUE(Edit->ContainsObject(Actor->GetRootComponent()));
	EXPECT_TRUE(Edit->ContainsObject(Peer->GetRootComponent()));
	const auto Dragged = Actor->GetActorTransform().Translation;
	EXPECT_GT(Durin::Math::Length(Dragged - InitialLocation), 0.001);
	ExpectVectorNear(Peer->GetActorTransform().Translation, Dragged);
	EXPECT_TRUE(Edit->GetDetails().empty());
	Context.SelectActor(Third);
	ASSERT_TRUE(Client.GetTransformGizmo().Update(Context, TranslateView, Input, TransformTransactions.Get()));
	const auto FinalFrame = RecordedProxy->GetTransform();
	ASSERT_TRUE(TransformTransactions->Undo());
	EXPECT_FALSE(Package->IsDirty());
	ExpectVectorNear(RecordedProxy->GetTransform().Translation, InitialLocation);
	ExpectVectorNear(Actor->GetActorTransform().Translation, InitialLocation);
	ExpectVectorNear(Peer->GetActorTransform().Translation, InitialLocation);
	ExpectVectorNear(Third->GetActorTransform().Translation, ThirdLocation);
	ASSERT_TRUE(TransformTransactions->Redo());
	ExpectVectorNear(RecordedProxy->GetTransform().Translation, FinalFrame.Translation);
	ExpectVectorNear(Actor->GetActorTransform().Translation, Dragged);
	ExpectVectorNear(Peer->GetActorTransform().Translation, Dragged);
	ExpectVectorNear(Third->GetActorTransform().Translation, ThirdLocation);
	Context.SelectActor(Actor);
	EXPECT_TRUE(Package->IsDirty());

	Client.GetTransformGizmo().SetMode(Durin::LevelEditor::ETransformGizmoMode::Rotate);
	Durin::FSceneView RotateView;
	ASSERT_TRUE(Client.CalcSceneView(800, 600, RotateView));
	EXPECT_EQ(RotateView.OverlayPrimitives.size(), 3u);
	Client.GetTransformGizmo().SetMode(Durin::LevelEditor::ETransformGizmoMode::Scale);
	EXPECT_EQ(Client.GetTransformGizmo().GetSpace(), Durin::LevelEditor::ETransformGizmoSpace::World);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::LevelEditor::ETransformGizmoSpace::Local);
	Client.GetTransformGizmo().SetSpace(Durin::LevelEditor::ETransformGizmoSpace::Parent);
	EXPECT_EQ(Client.GetTransformGizmo().GetSpace(), Durin::LevelEditor::ETransformGizmoSpace::Parent);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::LevelEditor::ETransformGizmoSpace::Local);
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
	Client.GetTransformGizmo().SetMode(Durin::LevelEditor::ETransformGizmoMode::Translate);
	EXPECT_EQ(Client.GetTransformGizmo().GetEffectiveSpace(), Durin::LevelEditor::ETransformGizmoSpace::Parent);

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
	auto* Replacement = Durin::Cast<Durin::DSceneComponent>(Actor->AddInstanceComponent(Durin::DSceneComponent::StaticClass(), "ReplacementRoot"));
	ASSERT_NE(Replacement, nullptr);
	ASSERT_TRUE(Actor->SetRootComponent(Replacement));
	EXPECT_FALSE(TransformTransactions->Undo());
	ExpectVectorNear(Peer->GetActorTransform().Translation, Dragged);
}

TEST(FLevelEditorViewportClientTests, BuildsComponentOrientedSelectionBounds)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "SelectionBoundsWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
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
	Durin::LevelEditor::FLevelEditorViewportClient Client;
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
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportTestLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(CameraActor, nullptr);
	CameraActor->GetCameraComponent()->SetWorldLocation({7.0, 8.0, 9.0});
	Durin::ACameraActor* ControllerCamera = Level->SpawnActor<Durin::ACameraActor>("ControllerCamera");
	ASSERT_NE(ControllerCamera, nullptr);
	ControllerCamera->GetCameraComponent()->SetWorldLocation({21.0, 22.0, 23.0});
	ASSERT_NE(Level->SpawnActor<Durin::APlayerStart>("Start"), nullptr);
	World->BeginPlay({
		.GameModeClass = Durin::AGameMode::StaticClass(),
		.ViewTargetOverride = ControllerCamera});
	ASSERT_EQ(World->GetPlayState(), Durin::EWorldPlayState::Playing);
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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FViewportSelectionTests, ConstrainedCameraBuildsCenteredContentRect)
{
	InitializeDObjectSystem();
	FTestEngine Engine;
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(&Engine, "ConstrainedViewportWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FCameraPreviewViewportClientTests, BuildsViewFromAssignedCameraAndRejectsMissingCamera)
{
	InitializeDObjectSystem();
	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(nullptr, "CameraPreviewWorld");
	EXPECT_NO_THROW(World->InitializeSubsystems());
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "CameraPreviewLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("PreviewCamera");
	ASSERT_NE(CameraActor, nullptr);
	CameraActor->GetCameraComponent()->SetWorldLocation({3.0, 4.0, 5.0});

	Durin::LevelEditor::FCameraPreviewViewportClient Client;
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

namespace
{
	struct FGizmoGestureProbe
	{
		Durin::LevelEditor::FLevelEditorViewportClient Client;
		Durin::FSceneView View;
		Durin::DTransformGizmo& Gizmo = Client.GetTransformGizmo();
		Durin::LevelEditor::FLevelEditorViewportInput Input;
		Durin::LevelEditor::FTransformGizmoTargetSet Targets;
		std::vector<std::shared_ptr<FProbeTransformTarget>> Objects;
		Durin::FVector2f Motion;
		auto Initialize(size_t Count) -> bool
		{
			if (!Client.BuildViewMatrices(800, 600, View)) return false;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				auto Target = std::make_shared<FProbeTransformTarget>();
				Target->Owner->SetRelativeLocation(Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 5.0);
				Objects.push_back(Target); Target->Bind(Targets);
			}
			return AimAtHandle();
		}
		bool AimAtHandle()
		{
			Gizmo.Update(Targets, View, {}, nullptr);
			auto Overlay = View; Gizmo.AppendOverlayPrimitives(Overlay);
			if (Overlay.OverlayPrimitives.empty()) return false;
			const auto& Matrix = Overlay.OverlayPrimitives.front().LocalToWorld;
			Durin::FVector2f Center, Handle;
			if (!Durin::SceneViewProjection::ProjectWorldToViewport(View, Durin::FVector3(Matrix[3]), Center)
				|| !Durin::SceneViewProjection::ProjectWorldToViewport(View, Durin::FVector3(Matrix * Durin::FVector4(0.65, 0, 0, 1)), Handle)) return false;
			Input.bFocused = Input.bHovered = Input.bLeftMousePressed = Input.bLeftMouseDown = true;
			Input.ViewportSize = {800, 600}; Input.MousePosition = Handle;
			Motion = Durin::Math::Normalize(Handle - Center) * 30.0f;
			return true;
		}
		auto Begin(Durin::DTransactor* Buffer) -> void
		{
			Targets.Targets.clear();
			for (const auto& Object : Objects) Object->Bind(Targets);
			Gizmo.Update(Targets, View, Input, Buffer);
		}
		auto Move(Durin::DTransactor* Buffer) -> void
		{ Input.bLeftMousePressed = false; Input.MousePosition += Motion; Gizmo.Update(Targets, View, Input, Buffer); }
		auto Release(Durin::DTransactor* Buffer) -> void
		{ Input.bLeftMouseDown = false; Gizmo.Update(Targets, View, Input, Buffer); }
	};
}

TEST(FTransformGizmoTests, ReleaseRecordsOneProxyChangeForTheSelection)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(3));
	Probe.Objects[2]->Owner = Probe.Objects[0]->Owner;
	Tests::FTestEditorTransactorOwner Buffer;
	const auto Before = Probe.Objects.front()->GetTransform();
	Probe.Begin(Buffer.Get());
	ASSERT_TRUE(Buffer->IsActive());
	const auto Id = GUndo->GetContext().TransactionId;
	Probe.Move(Buffer.Get());
	EXPECT_TRUE(Buffer->IsActive());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	const auto After = Probe.Objects.front()->GetTransform();
	ASSERT_NE(After.Translation, Before.Translation);
	Probe.Release(Buffer.Get());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	const auto* Entry = Buffer->GetTransaction(0);
	ASSERT_NE(Entry, nullptr);
	EXPECT_EQ(Entry->GetId(), Id);
	EXPECT_FALSE(Buffer->IsActive());
	ASSERT_EQ(Entry->GetRecordCount(), 3u);
	EXPECT_TRUE(Entry->ContainsObject(Probe.Objects[0]->Owner.Get(), true));
	EXPECT_TRUE(Entry->ContainsObject(Probe.Objects[1]->Owner.Get(), true));
	CollectGarbage(EObjectFlags::Standalone);
	ASSERT_TRUE(Buffer->Undo());
	for (const auto& Target : Probe.Objects) ExpectVectorNear(Target->GetTransform().Translation, Before.Translation);
	ASSERT_TRUE(Buffer->Redo());
	for (const auto& Target : Probe.Objects) ExpectVectorNear(Target->GetTransform().Translation, After.Translation);
	Probe.Objects[0]->bValid = false;
	EXPECT_FALSE(Buffer->Undo());
	for (const auto& Target : Probe.Objects) ExpectVectorNear(Target->GetTransform().Translation, After.Translation);
}

TEST(FTransformGizmoTests, ConsecutiveDragsUseTheRestoredSelectionFrame)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(2));
	Tests::FTestEditorTransactorOwner Buffer;
	auto* Proxy = Probe.Gizmo.GetActiveTarget();
	ASSERT_NE(Proxy, nullptr);
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	Probe.Release(Buffer.Get());
	const auto FirstFrame = Proxy->GetTransform();
	const auto FirstPosition = Probe.Objects[0]->GetTransform().Translation;
	ASSERT_TRUE(Probe.AimAtHandle());
	Probe.Begin(Buffer.Get());
	ASSERT_EQ(Probe.Gizmo.GetActiveTarget(), Proxy);
	Probe.Move(Buffer.Get());
	Probe.Release(Buffer.Get());
	ASSERT_EQ(Buffer->GetQueueLength(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	ExpectVectorNear(Proxy->GetTransform().Translation, FirstFrame.Translation);
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, FirstPosition);
	auto Overlay = Probe.View;
	Probe.Gizmo.AppendOverlayPrimitives(Overlay);
	ASSERT_FALSE(Overlay.OverlayPrimitives.empty());
	ExpectVectorNear(FVector3(Overlay.OverlayPrimitives.front().LocalToWorld[3]), FirstFrame.Translation);

	ASSERT_TRUE(Probe.AimAtHandle());
	Probe.Begin(Buffer.Get());
	ASSERT_TRUE(Probe.Gizmo.IsDragging());
	ExpectVectorNear(Proxy->GetInitialTransform().Translation, FirstFrame.Translation);
	Probe.Move(Buffer.Get());
	Probe.Gizmo.CancelUpdate();
	ExpectVectorNear(Proxy->GetTransform().Translation, FirstFrame.Translation);
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, FirstPosition);
}

TEST(FTransformGizmoTests, RebasedDragAfterClampingUndoesThroughEachOriginalProxy)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(2));
	Tests::FTestEditorTransactorOwner Buffer;
	for (auto& Object : Probe.Objects) Object->Owner->ClearFlags(EObjectFlags::Transactional);
	auto* Original = Probe.Gizmo.GetActiveTarget();
	const auto Start = Original->GetTransform();
	const auto OriginalPosition = Probe.Objects[1]->GetTransform().Translation;
	Probe.Objects[1]->Setter = [](const FTransform&) { return true; };
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	Probe.Release(Buffer.Get());
	const auto FirstFrame = Original->GetTransform();
	ASSERT_NE(Start.Translation, FirstFrame.Translation);
	ExpectVectorNear(Probe.Objects[1]->GetTransform().Translation, OriginalPosition);

	Probe.Objects[1]->Setter = {};
	ASSERT_TRUE(Probe.AimAtHandle());
	Probe.Begin(Buffer.Get());
	auto* Rebased = Probe.Gizmo.GetActiveTarget();
	ASSERT_NE(Rebased, Original);
	ExpectVectorNear(Rebased->GetTransform().Translation, FirstFrame.Translation);
	Probe.Move(Buffer.Get());
	Probe.Release(Buffer.Get());
	const auto SecondPosition = Probe.Objects[1]->GetTransform().Translation;
	ASSERT_NE(SecondPosition, OriginalPosition);
	ASSERT_EQ(Buffer->GetQueueLength(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Probe.Gizmo.GetActiveTarget(), Rebased);
	ExpectVectorNear(Rebased->GetTransform().Translation, FirstFrame.Translation);
	ExpectVectorNear(Probe.Objects[1]->GetTransform().Translation, OriginalPosition);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Probe.Gizmo.GetActiveTarget(), Original);
	ExpectVectorNear(Original->GetTransform().Translation, Start.Translation);
	ExpectVectorNear(Probe.Objects[1]->GetTransform().Translation, OriginalPosition);
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Start.Translation);
	Probe.Objects[1]->Setter = [](const FTransform&) { return true; };
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Probe.Gizmo.GetActiveTarget(), Original);
	ExpectVectorNear(Probe.Objects[1]->GetTransform().Translation, OriginalPosition);
	Probe.Objects[1]->Setter = {};
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Probe.Gizmo.GetActiveTarget(), Rebased);
	ExpectVectorNear(Probe.Objects[1]->GetTransform().Translation, SecondPosition);
}

TEST(FTransformGizmoTests, SuccessiveScaleSequencesApplyOnlyTheNewScaleRatio)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(1));
	auto* Proxy = Probe.Gizmo.GetActiveTarget();
	for (double Scale : {2.0, 3.0})
	{
		Proxy->BeginTransformEditSequence();
		auto Frame = Proxy->GetTransform();
		Frame.Scale3D = FVector3(Scale);
		ASSERT_TRUE(Proxy->SetTransform(Frame));
		Proxy->EndTransformEditSequence();
		ExpectVectorNear(Probe.Objects.front()->GetTransform().Scale3D, Frame.Scale3D);
	}
}

TEST(FTransformGizmoTests, UndoAfterSelectionChangeDrivesOnlyTheOriginalTargets)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(1));
	Tests::FTestEditorTransactorOwner Buffer;
	Probe.Objects[0]->Owner->ClearFlags(EObjectFlags::Transactional);
	auto* Original = Probe.Gizmo.GetActiveTarget();
	const auto Before = Original->GetTransform();
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	Probe.Release(Buffer.Get());
	ASSERT_NE(Original->GetTransform().Translation, Before.Translation);
	const auto After = Original->GetTransform();
	const auto PreviousTargets = Probe.Targets;
	auto Other = std::make_shared<FProbeTransformTarget>();
	Other->Owner->SetRelativeLocation(Before.Translation + FVectorConstants::Right);
	Probe.Targets.Targets.clear();
	Other->Bind(Probe.Targets);
	ASSERT_TRUE(Probe.Gizmo.Update(Probe.Targets, Probe.View, {}, Buffer.Get()));
	auto* Current = Probe.Gizmo.GetActiveTarget();
	ASSERT_NE(Current, Original);
	const auto OtherPosition = Other->GetTransform().Translation;
	ASSERT_TRUE(Buffer->Undo());
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Before.Translation);
	ExpectVectorNear(Other->GetTransform().Translation, OtherPosition);
	ExpectVectorNear(Original->GetTransform().Translation, Before.Translation);
	EXPECT_EQ(Probe.Gizmo.GetActiveTarget(), Current);
	auto Overlay = Probe.View;
	Probe.Gizmo.AppendOverlayPrimitives(Overlay);
	ASSERT_FALSE(Overlay.OverlayPrimitives.empty());
	ExpectVectorNear(FVector3(Overlay.OverlayPrimitives.front().LocalToWorld[3]), Other->GetTransform().Translation);
	ASSERT_TRUE(Buffer->Redo());
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, After.Translation);
	ExpectVectorNear(Other->GetTransform().Translation, OtherPosition);
	ASSERT_TRUE(Probe.Gizmo.Update(PreviousTargets, Probe.View, {}, Buffer.Get()));
	EXPECT_NE(Probe.Gizmo.GetActiveTarget(), Original);
}

TEST(FTransformGizmoTests, RegistersSharedTopologyOnFirstWriteAndOnlyReadsFrameAtRelease)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(2));
	static size_t RevisionReads;
	static uint64 Revision;
	RevisionReads = 0;
	Revision = 1;
	Probe.Objects[1]->Owner = Probe.Objects[0]->Owner;
	for (auto& Element : Probe.Objects)
		Element->RevisionReader = [](const DObject&, uint64) { ++RevisionReads; return Revision; };

	TStrongObjectPtr<DTransformProxy> Proxy(NewObject<DTransformProxy>(nullptr, NAME_None));
	for (const auto& Element : Probe.Objects) Element->Bind(*Proxy.Get());
	Proxy->BeginTransformEditSequence();
	Tests::FTestEditorTransactorOwner Buffer;
	ASSERT_GE(Buffer->Begin("TransformGizmo", "Move two elements"), 0);
	FTransformProxyChangeSource Source(*Proxy.Get(), Buffer->GetHistoryId(), "Translate");
	auto Frame = Proxy->GetTransform();
	Frame.Translation += FVectorConstants::Right;
	ASSERT_TRUE(Proxy->SetTransform(Frame));
	EXPECT_EQ(RevisionReads, 1u);
	ASSERT_TRUE(Proxy->SetTransform(Frame));
	for (auto& Element : Probe.Objects) Element->TransformReads = 0;
	++Revision; // A change before release must invalidate the original topology.
	auto Change = Source.EndChange();
	for (const auto& Element : Probe.Objects) EXPECT_EQ(Element->TransformReads, 0u);
	EXPECT_EQ(RevisionReads, 1u);
	ASSERT_TRUE(Buffer->StoreUndo(Proxy.Get(), std::move(Change)));
	Buffer->End();
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 2u); // One shared owner snapshot and one proxy change.

	RevisionReads = 0;
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(RevisionReads, 1u);
}

TEST(FTransformGizmoTests, MixedSelectionReplaysComponentsWithAndWithoutSnapshots)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(2));
	auto& First = *Probe.Objects[0];
	auto& Second = *Probe.Objects[1];
	const auto BeforeFirst = First.GetTransform();
	const auto BeforeSecond = Second.GetTransform();
	Second.Owner->ClearFlags(EObjectFlags::Transactional);
	Tests::FTestEditorTransactorOwner Buffer;
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	const auto AfterFirst = First.GetTransform();
	const auto AfterSecond = Second.GetTransform();
	ASSERT_NE(AfterFirst.Translation, BeforeFirst.Translation);
	ASSERT_NE(AfterSecond.Translation, BeforeSecond.Translation);
	Probe.Release(Buffer.Get());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	const auto* Entry = Buffer->GetTransaction(0);
	ASSERT_NE(Entry, nullptr);
	EXPECT_TRUE(Entry->ContainsObject(First.Owner.Get(), true));
	EXPECT_FALSE(Entry->ContainsObject(Second.Owner.Get(), true));
	EXPECT_EQ(Entry->GetRecordCount(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	ExpectVectorNear(First.GetTransform().Translation, BeforeFirst.Translation);
	ExpectVectorNear(Second.GetTransform().Translation, BeforeSecond.Translation);
	ASSERT_TRUE(Buffer->Redo());
	ExpectVectorNear(First.GetTransform().Translation, AfterFirst.Translation);
	ExpectVectorNear(Second.GetTransform().Translation, AfterSecond.Translation);
}

TEST(FTransformGizmoTests, FailedDependencyRegistrationRestoresAllPreviews)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(2));
	Tests::FTestEditorTransactorOwner Buffer;
	const auto Before = Probe.Objects.front()->GetTransform();
	Probe.Objects[1]->RevisionReader = [](const DObject&, uint64) -> uint64 { throw std::runtime_error("Injected revision failure"); };
	Probe.Gizmo.SetActiveTarget(nullptr);
	Probe.Begin(Buffer.Get());
	size_t CanceledRecords = 0;
	const auto Listener = Buffer->OnTransactionStateChanged().AddLambda(
		[&](const FTransactionContext&, ETransactionStateEventType Event) {
			if (Event == ETransactionStateEventType::TransactionCanceled) CanceledRecords = static_cast<FTransaction*>(GUndo)->GetRecordCount();
		});
	Probe.Move(Buffer.Get());
	Buffer->OnTransactionStateChanged().Remove(Listener);
	EXPECT_EQ(CanceledRecords, 2u);
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_FALSE(Probe.Gizmo.IsDragging());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	for (const auto& Target : Probe.Objects) ExpectVectorNear(Target->GetTransform().Translation, Before.Translation);
}

TEST(FTransformGizmoTests, DestroyingAnActiveGizmoCancelsItsRecording)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Buffer;
	std::shared_ptr<FProbeTransformTarget> Target;
	FTransform Initial;
	{
		FGizmoGestureProbe Probe;
		ASSERT_TRUE(Probe.Initialize(1));
		Target = Probe.Objects.front();
		Initial = Target->GetTransform();
		Probe.Begin(Buffer.Get());
		ASSERT_TRUE(Buffer->IsActive());
		CollectGarbage(EObjectFlags::Standalone);
		Probe.Move(Buffer.Get());
		ASSERT_NE(Target->GetTransform().Translation, Initial.Translation);
	}
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_FALSE(Buffer->CanUndo());
	ExpectVectorNear(Target->GetTransform().Translation, Initial.Translation);
}

TEST(FTransformGizmoTests, FailedCancelRetainsRetryStateAndNeverClearsDirty)
{
	InitializeDObjectSystem(); FGizmoGestureProbe Probe; ASSERT_TRUE(Probe.Initialize(1));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	Durin::TStrongObjectPtr<Durin::DPackage> Package(Durin::NewObject<Durin::DPackage>(nullptr, "FailedGizmoCancel"));
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/GestureTests/", Durin::Testing::GetTestWorkDirectory().generic_string() + "/");
	Durin::FPackagePath Path; ASSERT_TRUE(Durin::FPackagePath::TryCreate("/GestureTests/FailedCancel", Path)); Package->InitializeAssetPackage(Path);
	Probe.Objects[0]->BindPackage(Package.Get()); Package->ClearDirty();
	const auto Initial = Probe.Objects[0]->GetTransform();
	Probe.Begin(Buffer.Get()); Probe.Move(Buffer.Get());
	Probe.Objects[0]->Setter = [](const Durin::FTransform&) { return false; };
	Probe.Gizmo.CancelUpdate();
	EXPECT_TRUE(Probe.Gizmo.IsDragging()); EXPECT_TRUE(Package->IsDirty()); EXPECT_FALSE(Buffer->CanUndo());
	Probe.Objects[0]->Setter = [](const Durin::FTransform&) { return true; };
	Probe.Gizmo.CancelUpdate(); EXPECT_TRUE(Probe.Gizmo.IsDragging()); EXPECT_TRUE(Package->IsDirty());
	Probe.Objects[0]->Setter = {};
	Probe.Gizmo.CancelUpdate(); EXPECT_FALSE(Probe.Gizmo.IsDragging());
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Initial.Translation);
}

TEST(FTransformGizmoTests, FailedPreviewRollsBackAllTargetsWithoutHistory)
{
	InitializeDObjectSystem(); FGizmoGestureProbe Probe; ASSERT_TRUE(Probe.Initialize(2));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	Probe.Objects[1]->Owner->ClearFlags(Durin::EObjectFlags::Transactional);
	const auto Initial = Probe.Objects[0]->GetTransform();
	Probe.Begin(Buffer.Get());
	bool bFail = true; auto Target = Probe.Objects[1];
	Target->Setter = [Target, &bFail](const Durin::FTransform& Value) { Target->Owner->SetRelativeTransform(Value); return !std::exchange(bFail, false); };
	Probe.Move(Buffer.Get());
	EXPECT_FALSE(Probe.Gizmo.IsDragging());
	for (const auto& Object : Probe.Objects) ExpectVectorNear(Object->GetTransform().Translation, Initial.Translation);
	EXPECT_FALSE(Buffer->CanUndo());
	Target->Setter = {};
}

TEST(FTransformGizmoTests, EscapeRestoresTheEntireSelectionIncludingOutsideWrites)
{
	InitializeDObjectSystem(); FGizmoGestureProbe Probe; ASSERT_TRUE(Probe.Initialize(2));
	Durin::Tests::FTestEditorTransactorOwner Buffer; const auto Initial = Probe.Objects[1]->GetTransform();
	Probe.Begin(Buffer.Get()); Probe.Move(Buffer.Get());
	Probe.Objects[0]->Owner->SetRelativeLocation(Probe.Objects[0]->Owner->GetRelativeLocation() + Durin::FVector3(100, 200, 300));
	Probe.Input.bCancel = true;
	ASSERT_TRUE(Probe.Gizmo.Update(Probe.Targets, Probe.View, Probe.Input, Buffer.Get()));
	EXPECT_FALSE(Buffer->IsActive());
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Initial.Translation);
	ExpectVectorNear(Probe.Objects[1]->GetTransform().Translation, Initial.Translation);
	EXPECT_FALSE(Buffer->CanUndo());
}

TEST(FTransformGizmoTests, UndoAndRedoDriveEachTargetThroughItsSetter)
{
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(2));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	const auto Before = Probe.Objects.front()->GetTransform();
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	const auto After = Probe.Objects.front()->GetTransform();
	Probe.Release(Buffer.Get());
	size_t Writes = 0;
	for (auto& Target : Probe.Objects)
		Target->Setter = [Owner = Target->Owner, &Writes](const Durin::FTransform& Value) {
			++Writes;
			Owner->SetRelativeTransform(Value);
			return true;
		};
	ASSERT_TRUE(Buffer->Undo());
	for (auto& Target : Probe.Objects) ExpectVectorNear(Target->GetTransform().Translation, Before.Translation);
	ASSERT_TRUE(Buffer->Redo());
	for (auto& Target : Probe.Objects) ExpectVectorNear(Target->GetTransform().Translation, After.Translation);
	EXPECT_EQ(Writes, 4u);
}

TEST(FTransformGizmoTests, EmptyGestureUndoAndRedoNeverWriteComponents)
{
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(1));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	Probe.Objects.front()->Setter = [](const Durin::FTransform&) {
		ADD_FAILURE() << "Empty gesture wrote a component";
		return true;
	};
	Probe.Begin(Buffer.Get());
	Probe.Release(Buffer.Get());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_TRUE(Buffer->Redo());
}

TEST(FTransformGizmoTests, ClampedWriteDirtiesPackageAndRecordsFrame)
{
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(1));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	auto* Package = MakeRevisionTestPackage("ClampedGizmoEdit");
	Probe.Objects[0]->BindPackage(Package);
	Package->ClearDirty();
	Probe.Objects[0]->Setter = [Package](const Durin::FTransform&) { if (!Durin::GIsTransacting) EXPECT_TRUE(Package->IsDirty()); return true; };
	Probe.Begin(Buffer.Get());
	EXPECT_FALSE(Package->IsDirty());
	auto* Proxy = Probe.Gizmo.GetActiveTarget();
	const auto Before = Proxy->GetTransform();
	Probe.Move(Buffer.Get());
	const auto After = Proxy->GetTransform();
	ASSERT_NE(Before.Translation, After.Translation);
	Probe.Release(Buffer.Get());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Undo());
	ExpectVectorNear(Proxy->GetTransform().Translation, Before.Translation);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo());
	ExpectVectorNear(Proxy->GetTransform().Translation, After.Translation);
	EXPECT_TRUE(Package->IsDirty());
}

TEST(FTransformGizmoTests, UndoRestoresPackageCheckpointFromFirstWrite)
{
	InitializeDObjectSystem();
	for (const bool bInitiallyDirty : {false, true})
	{
		FGizmoGestureProbe Probe;
		ASSERT_TRUE(Probe.Initialize(1));
		Durin::Tests::FTestEditorTransactorOwner Buffer;
		auto* Package = MakeRevisionTestPackage("GizmoCheckpoint");
		Probe.Objects[0]->BindPackage(Package);
		Package->ClearDirty();
		if (bInitiallyDirty) Package->MarkDirty();
		const auto Initial = Probe.Objects[0]->GetTransform();
		Probe.Begin(Buffer.Get());
		Probe.Move(Buffer.Get());
		EXPECT_TRUE(Package->IsDirty());
		Package->MarkDirty(); // A later write does not replace the first capture.
		Probe.Release(Buffer.Get());
		ASSERT_TRUE(Buffer->Undo());
		ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Initial.Translation);
		EXPECT_EQ(Package->IsDirty(), bInitiallyDirty);
		ASSERT_TRUE(Buffer->Redo());
		EXPECT_TRUE(Package->IsDirty());
	}
}

TEST(FTransformGizmoTests, HistoryFreePreviewMarksSharedOwnerOnEachWrite)
{
	InitializeDObjectSystem();
	for (const bool bCancel : {false, true})
	{
		FGizmoGestureProbe Probe;
		ASSERT_TRUE(Probe.Initialize(2));
		auto* Package = MakeRevisionTestPackage("UnrecordedGizmoEdit");
		Probe.Objects[0]->BindPackage(Package);
		Probe.Objects[1]->Owner = Probe.Objects[0]->Owner;
		const auto Initial = Probe.Objects[0]->GetTransform();
		Package->ClearDirty();
		const auto Revision = Package->GetEditRevision();
		Probe.Begin(nullptr);
		EXPECT_FALSE(Package->IsDirty());
		Probe.Move(nullptr);
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_EQ(Package->GetEditRevision(), Revision + 2);
		Probe.Move(nullptr);
		const auto PreviewRevision = Package->GetEditRevision();
		EXPECT_EQ(PreviewRevision, Revision + 4);
		const auto Preview = Probe.Objects[0]->GetTransform();
		ASSERT_NE(Preview.Translation, Initial.Translation);
		if (bCancel) Probe.Gizmo.CancelUpdate();
		else Probe.Release(nullptr);
		EXPECT_FALSE(Probe.Gizmo.IsDragging());
		ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, bCancel ? Initial.Translation : Preview.Translation);
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_EQ(Package->GetEditRevision(), PreviewRevision);
	}
}

TEST(FTransformGizmoTests, DiscardedHistoryLeavesReleasedTransformAppliedAndDirty)
{
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(1));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	auto* Package = MakeRevisionTestPackage("DiscardedGizmoEdit");
	Package->SetPlayInEditor(true);
	Probe.Objects.front()->BindPackage(Package);
	Package->ClearDirty();
	const auto Initial = Probe.Objects.front()->GetTransform();
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	const auto Preview = Probe.Objects.front()->GetTransform();
	ASSERT_NE(Initial.Translation, Preview.Translation);
	ASSERT_TRUE(Package->IsDirty());
	const auto DirtyRevision = Package->GetEditRevision();
	Probe.Input.bLeftMouseDown = false;
	EXPECT_TRUE(Probe.Gizmo.Update(Probe.Targets, Probe.View, Probe.Input, Buffer.Get()));
	EXPECT_FALSE(Probe.Gizmo.IsDragging());
	EXPECT_FALSE(Buffer->CanUndo());
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Package->GetEditRevision(), DirtyRevision);
	ExpectVectorNear(Probe.Objects.front()->GetTransform().Translation, Preview.Translation);
}

TEST(FTransformGizmoTests, RedoUsesReleasedFrameAfterAnOutsideWriteDuringDrag)
{
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(1));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	const auto Before = Probe.Objects[0]->GetTransform();
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	const auto Released = Probe.Objects[0]->GetTransform().Translation;
	Probe.Objects[0]->Owner->SetRelativeLocation(Probe.Objects[0]->Owner->GetRelativeLocation() + Durin::FVector3(100, 200, 300));
	Probe.Release(Buffer.Get());
	EXPECT_FALSE(Probe.Gizmo.IsDragging());
	ASSERT_TRUE(Buffer->Undo());
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Before.Translation);
	ASSERT_TRUE(Buffer->Redo());
	ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Released);
}

TEST(FTransformGizmoTests, RedoUsesReleasedFrameAfterAnOutsideWriteBeforeUndo)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FGizmoGestureProbe Probe;
	ASSERT_TRUE(Probe.Initialize(1));
	Tests::FTestEditorTransactorOwner Buffer;
	auto& Owner = *Probe.Objects.front()->Owner;
	const auto Before = Owner.GetRelativeLocation();
	Probe.Begin(Buffer.Get());
	Probe.Move(Buffer.Get());
	Probe.Release(Buffer.Get());
	const auto Released = Owner.GetRelativeLocation();
	const auto Outside = Released + FVector3(100, 200, 300);
	Owner.SetRelativeLocation(Outside);
	ASSERT_TRUE(Buffer->Undo());
	ExpectVectorNear(Owner.GetRelativeLocation(), Before);
	ASSERT_TRUE(Buffer->Redo());
	ExpectVectorNear(Owner.GetRelativeLocation(), Released);
}

TEST(FTransformGizmoTests, ForeignRecordingDefersRestoreWithoutJoiningItsHistory)
{
	InitializeDObjectSystem(); FGizmoGestureProbe Probe; ASSERT_TRUE(Probe.Initialize(1));
	Durin::Tests::FTestEditorTransactorOwner Buffer; const auto Initial = Probe.Objects[0]->GetTransform();
	Probe.Begin(Buffer.Get()); Probe.Move(Buffer.Get()); const auto Preview = Probe.Objects[0]->GetTransform();
	Buffer->Cancel();
	ASSERT_EQ(Buffer->Begin("ForeignEdit", "Foreign edit"), 0);
	Probe.Input.bCancel = true;
	EXPECT_TRUE(Probe.Gizmo.Update(Probe.Targets, Probe.View, Probe.Input, Buffer.Get()));
	EXPECT_TRUE(Probe.Gizmo.IsDragging()); ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Preview.Translation);
	Buffer->End();
	EXPECT_TRUE(Probe.Gizmo.Update(Probe.Targets, Probe.View, Probe.Input, Buffer.Get()));
	EXPECT_FALSE(Probe.Gizmo.IsDragging()); ExpectVectorNear(Probe.Objects[0]->GetTransform().Translation, Initial.Translation);
	EXPECT_FALSE(Buffer->CanUndo());
}

TEST(FTransformGizmoTests, IdleGizmoYieldsToPropertyRecordingWithoutReportingFailure)
{
	InitializeDObjectSystem(); FGizmoGestureProbe Probe; ASSERT_TRUE(Probe.Initialize(1));
	Durin::Tests::FTestEditorTransactorOwner Buffer;
	ASSERT_GE(Buffer->Begin("ReflectedProperty", "Edit Color"), 0);
	EXPECT_TRUE(Probe.Gizmo.Update(Probe.Targets, Probe.View, Probe.Input, Buffer.Get()));
	EXPECT_TRUE(Probe.Gizmo.Update(Probe.Targets, Probe.View, Probe.Input, Buffer.Get()));
	EXPECT_FALSE(Probe.Gizmo.IsDragging());
	EXPECT_FALSE(Probe.Gizmo.IsHovered());
	EXPECT_TRUE(Buffer->IsActive());
	Buffer->End();
	EXPECT_FALSE(Buffer->CanUndo());
}
