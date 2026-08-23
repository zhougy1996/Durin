#include "WorldTestSupport.h"

#include "DObject/Class.h"
#include "Editor/EditorEngine.h"
#include "Client/ViewportClient.h"
#include "Engine/GameEngine.h"
#include "Gameplay/PawnControlIntent.h"
#include "Input/GameInputState.h"
#include "Window/GenericWindow.h"

namespace Durin
{
	struct FEditorEngineTestAccess
	{
		static auto Initialize(DEditorEngine& Engine, DWorld* EditorWorld) -> void
		{
			Engine.EditorWorld = EditorWorld;
			Engine.SetWorld(EditorWorld);
		}

		static auto StartPlaySession(
			DEditorEngine& Engine,
			const Editor::FPlayRequest& Request,
			DClass* GameModeClass,
			std::string* OutError = nullptr) -> bool
		{
			return Engine.StartPlaySessionInternal(
				Request, std::optional<DClass*>{GameModeClass}, OutError);
		}

		static auto ConfigureMouseCapture(
			DEditorEngine& Engine,
			Editor::EPlayDestination Destination = Editor::EPlayDestination::EmbeddedViewport) -> void
		{
			Engine.PlayState = Editor::EPlayState::Playing;
			Engine.PlayDestination = Destination;
		}

		static auto InitializePlayWindowViewportClient(
			DEditorEngine& Engine,
			const FViewportClient* SourceClient) -> void
		{
			Engine.InitializePlayWindowViewportClient(SourceClient);
		}

		static auto KeyDown(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat = false) -> bool
		{
			return Engine.HandleGameInputKeyDown(Window, Key, bRepeat);
		}

		static auto Focus(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void
		{
			Engine.HandleGameInputWindowFocus(Window, bFocused);
		}

		static auto MouseDown(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool
		{
			return Engine.HandleGameInputMouseDown(Window, Button);
		}

		static auto Close(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window) -> void
		{
			Engine.HandleGameInputWindowClose(Window);
		}
	};

	struct FEngineInputTestAccess
	{
		static auto IsTarget(const DEngine& Engine, const std::shared_ptr<FGenericWindow>& Window) -> bool
		{
			return Engine.GameInputWindow.lock() == Window;
		}
	};

	struct FGameEngineTestAccess
	{
		static auto KeyDown(DGameEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat = false) -> bool
		{
			return Engine.HandleGameInputKeyDown(Window, Key, bRepeat);
		}

		static auto Focus(DGameEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void
		{
			Engine.HandleGameInputWindowFocus(Window, bFocused);
		}

		static auto MouseDown(DGameEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool
		{
			return Engine.HandleGameInputMouseDown(Window, Button);
		}

		static auto Close(DGameEngine& Engine, const std::shared_ptr<FGenericWindow>& Window) -> void
		{
			Engine.HandleGameInputWindowClose(Window);
		}
	};

	struct FGameInputStateTestAccess
	{
		static auto EnableAndFocus(FGameInputState& Input) -> void
		{
			Input.SetEnabled(true);
			Input.SetFocused(true);
		}
		static auto SetFocused(FGameInputState& Input, bool bFocused) -> void { Input.SetFocused(bFocused); }
		static auto SetEnabled(FGameInputState& Input, bool bEnabled) -> void { Input.SetEnabled(bEnabled); }
		static auto SetKey(FGameInputState& Input, EKey Key, bool bDown) -> void { Input.SetKey(Key, bDown); }
		static auto SetMousePosition(FGameInputState& Input, FVector2d Position) -> void { Input.SetMousePosition(Position); }
		static auto FinishTick(FGameInputState& Input) -> void { Input.FinishGameTick(); }
	};
}

namespace
{
	class FFocusedTestWindow final : public Durin::FGenericWindow
	{
	public:
		auto IsFocused() const -> bool override { return bFocused; }
		auto SetFocused(bool bInFocused) -> void { bFocused = bInFocused; }

	private:
		bool bFocused = true;
	};

	template<typename TObject>
	auto ConstructTestObject(const Durin::FObjectInitializer& Initializer) -> void
	{
		new (Initializer.GetObj()) TObject(Initializer);
	}

	template<typename TObject, typename TBase>
	auto GetTestClass(std::string_view Name) -> Durin::DClass*
	{
		static Durin::DClass Class(
			Durin::EC_StaticConstructor,
			Durin::FName(Name),
			sizeof(TObject),
			alignof(TObject),
			Durin::EObjectFlags::Transient,
			Durin::EClassFlags::None,
			Durin::EClassCastFlags::DClass,
			&ConstructTestObject<TObject>);
		static bool bInitialized = [] {
			Class.SetSuperStructBase(TBase::StaticClass());
			return true;
		}();
		(void)bInitialized;
		return &Class;
	}

	class FIntentMovementComponent final : public Durin::DPawnMovementComponent
	{
	public:
		explicit FIntentMovementComponent(const Durin::FObjectInitializer& Initializer)
			: Durin::DPawnMovementComponent(Initializer)
		{
		}

		auto GetUpdateCount() const -> uint32 { return UpdateCount; }
		auto GetLastIntent() const -> const Durin::FPawnControlIntent& { return LastIntent; }

	protected:
		auto PerformMovement(const Durin::FPawnControlIntent& Intent, float) -> void override
		{
			LastIntent = Intent;
			++UpdateCount;
		}

	private:
		Durin::FPawnControlIntent LastIntent;
		uint32 UpdateCount = 0;
	};

	class FIntentPawn final : public Durin::APawn
	{
	public:
		explicit FIntentPawn(const Durin::FObjectInitializer& Initializer)
			: Durin::APawn(Initializer)
		{
		}

		auto SelectMovement(Durin::DPawnMovementComponent* Movement) -> bool
		{
			return SetMovementComponent(Movement);
		}
		auto WasPossessedAtBeginPlay() const -> bool { return bWasPossessedAtBeginPlay; }
		auto GetBeginPlayTransform() const -> const Durin::FTransform& { return BeginPlayTransform; }
		inline static std::function<void(FIntentPawn&)> BeginPlayCallback;

	protected:
		auto BeginPlay() -> void override
		{
			bWasPossessedAtBeginPlay = GetController() != nullptr;
			BeginPlayTransform = GetActorTransform();
			Durin::APawn::BeginPlay();
			if (BeginPlayCallback) BeginPlayCallback(*this);
		}

	private:
		Durin::FTransform BeginPlayTransform;
		bool bWasPossessedAtBeginPlay = false;
	};

	class FIntentPlayerController final : public Durin::APlayerController
	{
	public:
		explicit FIntentPlayerController(const Durin::FObjectInitializer& Initializer)
			: Durin::APlayerController(Initializer)
		{
		}

		auto SetNextIntent(Durin::FPawnControlIntent Intent) -> void { NextIntent = std::move(Intent); }
		auto SetUseRawMapping(bool bEnabled) -> void { bUseRawMapping = bEnabled; }
		auto SetWorldToClearDuringBuild(Durin::DWorld* World) -> void { WorldToClearDuringBuild = World; }
		auto GetBuildCount() const -> uint32 { return BuildCount; }
		auto SubmitDirect(const Durin::FPawnControlIntent& Intent) -> bool { return SubmitControlIntent(Intent); }

	protected:
		auto BuildControlIntent(const Durin::FGameInputState& Input) const -> Durin::FPawnControlIntent override
		{
			++BuildCount;
			if (Durin::DWorld* World = std::exchange(WorldToClearDuringBuild, nullptr))
			{
				World->RequestLevelTransition(nullptr);
				return {};
			}
			if (bUseRawMapping)
			{
				Durin::FPawnControlIntent Intent;
				Intent.Move.y = Input.IsKeyDown(Durin::EKey::W) ? 1.0 : 0.0;
				Intent.bJumpHeld = Input.IsKeyDown(Durin::EKey::Space);
				Intent.bJumpPressed = Input.WasKeyPressed(Durin::EKey::Space);
				Intent.bJumpReleased = Input.WasKeyReleased(Durin::EKey::Space);
				return Intent;
			}
			return NextIntent;
		}

	private:
		Durin::FPawnControlIntent NextIntent;
		mutable uint32 BuildCount = 0;
		mutable Durin::DWorld* WorldToClearDuringBuild = nullptr;
		bool bUseRawMapping = false;
	};

	auto IntentPawnClass() -> Durin::DClass*
	{
		return GetTestClass<FIntentPawn, Durin::APawn>("Durin::Tests::FIntentPawn");
	}

	auto IntentControllerClass() -> Durin::DClass*
	{
		return GetTestClass<FIntentPlayerController, Durin::APlayerController>("Durin::Tests::FIntentPlayerController");
	}

	auto IntentMovementClass() -> Durin::DClass*
	{
		return GetTestClass<FIntentMovementComponent, Durin::DPawnMovementComponent>("Durin::Tests::FIntentMovementComponent");
	}

	struct FIntentPawnCallbackScope
	{
		~FIntentPawnCallbackScope() { FIntentPawn::BeginPlayCallback = {}; }
	};

	class FIntentGameMode final : public Durin::AGameMode
	{
	public:
		explicit FIntentGameMode(const Durin::FObjectInitializer& Initializer)
			: Durin::AGameMode(Initializer)
		{
		}

		auto GetPlayerControllerClass() const -> Durin::DClass* override { return IntentControllerClass(); }
		auto GetDefaultPawnClass() const -> Durin::DClass* override { return IntentPawnClass(); }
	};

	auto IntentGameModeClass() -> Durin::DClass*
	{
		return GetTestClass<FIntentGameMode, Durin::AGameMode>("Durin::Tests::FIntentGameMode");
	}

	class FInvalidPawnGameMode final : public Durin::AGameMode
	{
	public:
		explicit FInvalidPawnGameMode(const Durin::FObjectInitializer& Initializer)
			: Durin::AGameMode(Initializer)
		{
		}

		auto GetDefaultPawnClass() const -> Durin::DClass* override { return Durin::AActor::StaticClass(); }
	};

	auto InvalidPawnGameModeClass() -> Durin::DClass*
	{
		return GetTestClass<FInvalidPawnGameMode, Durin::AGameMode>("Durin::Tests::FInvalidPawnGameMode");
	}

	class FUnplaceablePawn final : public Durin::APawn
	{
	public:
		explicit FUnplaceablePawn(const Durin::FObjectInitializer& Initializer)
			: Durin::APawn(Initializer)
		{
			SetRootComponent(nullptr);
		}
	};

	auto UnplaceablePawnClass() -> Durin::DClass*
	{
		return GetTestClass<FUnplaceablePawn, Durin::APawn>("Durin::Tests::FUnplaceablePawn");
	}

	class FUnplaceablePawnGameMode final : public Durin::AGameMode
	{
	public:
		explicit FUnplaceablePawnGameMode(const Durin::FObjectInitializer& Initializer)
			: Durin::AGameMode(Initializer)
		{
		}

		auto GetDefaultPawnClass() const -> Durin::DClass* override { return UnplaceablePawnClass(); }
	};

	auto UnplaceablePawnGameModeClass() -> Durin::DClass*
	{
		return GetTestClass<FUnplaceablePawnGameMode, Durin::AGameMode>("Durin::Tests::FUnplaceablePawnGameMode");
	}

	auto SpawnPlayerStart(Durin::DWorld& World, Durin::FName Name, const Durin::FVector3& Location) -> Durin::APlayerStart*
	{
		auto* Start = World.SpawnActor<Durin::APlayerStart>(Name);
		if (Start)
		{
			Durin::FTransform Transform;
			Transform.Translation = Location;
			Start->SetActorTransform(Transform);
		}
		return Start;
	}
}

TEST(FGameInputAuthorityTests, ReplacementAndExpiryClearHeldInputAndMouseBaseline)
{
	InitializeDObjectSystem();
	auto* Engine = Durin::NewObject<Durin::DEditorEngine>(nullptr, "GameInputAuthorityEngine");
	auto First = std::make_shared<FFocusedTestWindow>();
	auto Second = std::make_shared<FFocusedTestWindow>();
	Engine->SetGameInputWindow(First);
	Engine->SetGameInputEnabled(true);
	auto& Input = const_cast<Durin::FGameInputState&>(Engine->GetGameInputState());
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {10.0, 15.0});
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {14.0, 20.0});
	EXPECT_TRUE(Input.IsKeyDown(Durin::EKey::W));
	EXPECT_EQ(Input.GetMouseDelta(), Durin::FVector2d(4.0, 5.0));

	Engine->SetGameInputWindow(Second);
	EXPECT_FALSE(Input.IsEnabled());
	EXPECT_FALSE(Input.IsKeyDown(Durin::EKey::W));
	EXPECT_EQ(Input.GetMouseDelta(), Durin::FVector2d(0.0));
	EXPECT_FALSE(Durin::FEngineInputTestAccess::IsTarget(*Engine, First));
	EXPECT_TRUE(Durin::FEngineInputTestAccess::IsTarget(*Engine, Second));
	Engine->SetGameInputEnabled(true);
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {900.0, 700.0});
	EXPECT_EQ(Input.GetMouseDelta(), Durin::FVector2d(0.0));

	Second.reset();
	Engine->Tick(0.0f, false);
	EXPECT_FALSE(Input.IsEnabled());
	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage();
}

TEST(FEditorMouseCaptureTests, EscapeAndFocusLossRestoreWithoutAutomaticRecapture)
{
	InitializeDObjectSystem();
	auto* Engine = Durin::NewObject<Durin::DEditorEngine>(nullptr, "MouseCaptureEngine");
	auto Window = std::make_shared<FFocusedTestWindow>();
	Durin::FEditorEngineTestAccess::ConfigureMouseCapture(*Engine);
	Engine->SetGameInputWindow(Window);
	ASSERT_TRUE(Engine->RequestPlayMouseCapture(Window));
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Captured);
	EXPECT_TRUE(Engine->GetGameInputState().IsEnabled());
	EXPECT_TRUE(Durin::FEditorEngineTestAccess::KeyDown(*Engine, Window, Durin::EKey::Escape));
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Free);
	EXPECT_EQ(Engine->GetMouseCaptureState(), Durin::Editor::EMouseCaptureState::Released);
	EXPECT_FALSE(Engine->GetGameInputState().IsEnabled());
	EXPECT_FALSE(Durin::FEditorEngineTestAccess::KeyDown(*Engine, Window, Durin::EKey::Escape));

	ASSERT_TRUE(Engine->RequestPlayMouseCapture(Window));
	Durin::FEditorEngineTestAccess::Focus(*Engine, Window, false);
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Free);
	EXPECT_EQ(Engine->GetMouseCaptureState(), Durin::Editor::EMouseCaptureState::Suspended);
	Durin::FEditorEngineTestAccess::Focus(*Engine, Window, true);
	EXPECT_EQ(Engine->GetMouseCaptureState(), Durin::Editor::EMouseCaptureState::Released);
	EXPECT_FALSE(Engine->GetGameInputState().IsEnabled());

	Engine->StopPlaySession();
	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage();
}

TEST(FGameMouseCaptureTests, StartupEscapeClickFocusAndCloseManageStandaloneCapture)
{
	InitializeDObjectSystem();
	auto* Engine = Durin::NewObject<Durin::DGameEngine>(nullptr, "StandaloneMouseCaptureEngine");
	auto Window = std::make_shared<FFocusedTestWindow>();
	Engine->SetGameInputWindow(Window);

	ASSERT_TRUE(Engine->RequestGameMouseCapture(Window));
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Captured);
	EXPECT_TRUE(Engine->IsGameMouseCaptured());
	EXPECT_TRUE(Engine->GetGameInputState().IsEnabled());

	EXPECT_TRUE(Durin::FGameEngineTestAccess::KeyDown(*Engine, Window, Durin::EKey::Escape));
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Free);
	EXPECT_FALSE(Engine->IsGameMouseCaptured());
	EXPECT_FALSE(Engine->GetGameInputState().IsEnabled());
	EXPECT_FALSE(Durin::FGameEngineTestAccess::KeyDown(*Engine, Window, Durin::EKey::Escape));

	EXPECT_FALSE(Durin::FGameEngineTestAccess::MouseDown(*Engine, Window, Durin::EMouseButton::Right));
	ASSERT_TRUE(Durin::FGameEngineTestAccess::MouseDown(*Engine, Window, Durin::EMouseButton::Left));
	Durin::FGameEngineTestAccess::Focus(*Engine, Window, false);
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Free);
	EXPECT_FALSE(Engine->IsGameMouseCaptured());
	EXPECT_FALSE(Engine->GetGameInputState().IsEnabled());

	Window->SetFocused(false);
	EXPECT_FALSE(Durin::FGameEngineTestAccess::MouseDown(*Engine, Window, Durin::EMouseButton::Left));
	Window->SetFocused(true);
	ASSERT_TRUE(Durin::FGameEngineTestAccess::MouseDown(*Engine, Window, Durin::EMouseButton::Left));
	Durin::FGameEngineTestAccess::Close(*Engine, Window);
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Free);
	EXPECT_FALSE(Engine->IsGameMouseCaptured());

	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage();
}

TEST(FEditorMouseCaptureTests, NewWindowClickPauseCloseAndRepeatedReleaseAreIdempotent)
{
	InitializeDObjectSystem();
	auto* Engine = Durin::NewObject<Durin::DEditorEngine>(nullptr, "NewWindowMouseCaptureEngine");
	auto Window = std::make_shared<FFocusedTestWindow>();
	Durin::FEditorEngineTestAccess::ConfigureMouseCapture(*Engine, Durin::Editor::EPlayDestination::NewWindow);
	Engine->SetGameInputWindow(Window);
	EXPECT_FALSE(Durin::FEditorEngineTestAccess::MouseDown(*Engine, Window, Durin::EMouseButton::Right));
	ASSERT_TRUE(Durin::FEditorEngineTestAccess::MouseDown(*Engine, Window, Durin::EMouseButton::Left));
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Captured);
	Engine->SetPlaySessionPaused(true);
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Free);
	EXPECT_EQ(Engine->GetMouseCaptureState(), Durin::Editor::EMouseCaptureState::Released);

	Durin::FEditorEngineTestAccess::ConfigureMouseCapture(*Engine, Durin::Editor::EPlayDestination::NewWindow);
	ASSERT_TRUE(Durin::FEditorEngineTestAccess::MouseDown(*Engine, Window, Durin::EMouseButton::Left));
	Durin::FEditorEngineTestAccess::Close(*Engine, Window);
	EXPECT_EQ(Window->GetCursorMode(), Durin::ECursorMode::Free);
	Engine->ReleasePlayMouseCapture();
	Engine->ReleasePlayMouseCapture();
	EXPECT_EQ(Engine->GetMouseCaptureState(), Durin::Editor::EMouseCaptureState::Released);

	Engine->StopPlaySession();
	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayReflectionTests, PublishesExactRootIdentitiesAndInheritance)
{
	InitializeDObjectSystem();
	EXPECT_EQ(Durin::APawn::StaticClass()->GetQualifiedName().ToString(), "Durin::APawn");
	EXPECT_EQ(Durin::AController::StaticClass()->GetQualifiedName().ToString(), "Durin::AController");
	EXPECT_EQ(Durin::APlayerController::StaticClass()->GetQualifiedName().ToString(), "Durin::APlayerController");
	EXPECT_EQ(Durin::AGameMode::StaticClass()->GetQualifiedName().ToString(), "Durin::AGameMode");
	EXPECT_EQ(Durin::APlayerStart::StaticClass()->GetQualifiedName().ToString(), "Durin::APlayerStart");
	EXPECT_EQ(Durin::DPawnMovementComponent::StaticClass()->GetQualifiedName().ToString(), "Durin::DPawnMovementComponent");
	EXPECT_EQ(Durin::FPawnControlIntent::StaticStruct()->GetQualifiedName().ToString(), "Durin::FPawnControlIntent");
	EXPECT_TRUE(Durin::APlayerController::StaticClass()->IsChildOf(Durin::AController::StaticClass()));
	EXPECT_TRUE(Durin::DPawnMovementComponent::StaticClass()->HasAnyClassFlags(Durin::EClassFlags::Abstract));
	EXPECT_FALSE(Durin::CanConstructObjectOfClass(
		Durin::DPawnMovementComponent::StaticClass(), Durin::DActorComponent::StaticClass()));
	ASSERT_NE(Durin::APawn::StaticClass()->FindPropertyByName("Controller"), nullptr);
	EXPECT_TRUE(Durin::APawn::StaticClass()->FindPropertyByName("Controller")
		->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient));
	ASSERT_NE(Durin::AController::StaticClass()->FindPropertyByName("Pawn"), nullptr);
	EXPECT_TRUE(Durin::AController::StaticClass()->FindPropertyByName("Pawn")
		->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient));
}

TEST(FNativeGameplayPossessionTests, PairsTransfersAndUnpossessesSymmetrically)
{
	Durin::DWorld* World = CreateWorld();
	auto* FirstController = World->SpawnActor<Durin::AController>("FirstController");
	auto* SecondController = World->SpawnActor<Durin::AController>("SecondController");
	auto* FirstPawn = World->SpawnActor<Durin::APawn>("FirstPawn");
	auto* SecondPawn = World->SpawnActor<Durin::APawn>("SecondPawn");
	ASSERT_TRUE(FirstController->Possess(FirstPawn));
	EXPECT_EQ(FirstController->GetPawn(), FirstPawn);
	EXPECT_EQ(FirstPawn->GetController(), FirstController);
	EXPECT_TRUE(FirstController->Possess(FirstPawn));
	ASSERT_TRUE(FirstController->Possess(SecondPawn));
	EXPECT_EQ(FirstPawn->GetController(), nullptr);
	EXPECT_EQ(FirstController->GetPawn(), SecondPawn);
	ASSERT_TRUE(SecondController->Possess(SecondPawn));
	EXPECT_EQ(FirstController->GetPawn(), nullptr);
	EXPECT_EQ(SecondController->GetPawn(), SecondPawn);
	EXPECT_EQ(SecondPawn->GetController(), SecondController);
	SecondController->UnPossess();
	SecondController->UnPossess();
	EXPECT_EQ(SecondController->GetPawn(), nullptr);
	EXPECT_EQ(SecondPawn->GetController(), nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayPossessionTests, RejectsCrossWorldWithoutMutatingExistingPairs)
{
	Durin::DWorld* FirstWorld = CreateWorld();
	Durin::DWorld* SecondWorld = CreateWorld();
	auto* Controller = FirstWorld->SpawnActor<Durin::AController>("Controller");
	auto* FirstPawn = FirstWorld->SpawnActor<Durin::APawn>("FirstPawn");
	auto* ForeignPawn = SecondWorld->SpawnActor<Durin::APawn>("ForeignPawn");
	ASSERT_TRUE(Controller->Possess(FirstPawn));
	const Durin::FPossessionResult Result = Controller->Possess(ForeignPawn);
	EXPECT_EQ(Result.Error, Durin::EPossessionError::InvalidMembership);
	EXPECT_EQ(Controller->GetPawn(), FirstPawn);
	EXPECT_EQ(FirstPawn->GetController(), Controller);
	EXPECT_EQ(ForeignPawn->GetController(), nullptr);
	auto* DestroyedPawn = FirstWorld->SpawnActor<Durin::APawn>("DestroyedPawn");
	ASSERT_TRUE(FirstWorld->DestroyActor(DestroyedPawn));
	const Durin::FPossessionResult Destroyed = Controller->Possess(DestroyedPawn);
	EXPECT_EQ(Destroyed.Error, Durin::EPossessionError::PawnUnavailable);
	EXPECT_EQ(Controller->GetPawn(), FirstPawn);
	EXPECT_EQ(FirstPawn->GetController(), Controller);
	Durin::MarkObjectHierarchyAsGarbage(FirstWorld);
	Durin::MarkObjectHierarchyAsGarbage(SecondWorld);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayPossessionTests, ClearsPairsOnDestructionEndPlayAndLevelReplacement)
{
	Durin::DWorld* World = CreateWorld();
	auto* Controller = World->SpawnActor<Durin::AController>("Controller");
	auto* Pawn = World->SpawnActor<Durin::APawn>("Pawn");
	ASSERT_TRUE(Controller->Possess(Pawn));
	ASSERT_TRUE(World->DestroyActor(Pawn));
	EXPECT_EQ(Controller->GetPawn(), nullptr);
	Pawn = World->SpawnActor<Durin::APawn>("ReplacementPawn");
	ASSERT_TRUE(Controller->Possess(Pawn));
	ASSERT_TRUE(World->BeginPlay({}));
	World->EndPlay();
	EXPECT_EQ(Controller->GetPawn(), nullptr);
	EXPECT_EQ(Pawn->GetController(), nullptr);
	ASSERT_TRUE(Controller->Possess(Pawn));
	Durin::DLevel* ReplacementLevel = Durin::NewObject<Durin::DLevel>(World, "ReplacementLevel");
	ASSERT_TRUE(World->SetCurrentLevel(ReplacementLevel));
	EXPECT_EQ(Controller->GetPawn(), nullptr);
	EXPECT_EQ(Pawn->GetController(), nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayBootstrapTests, LifecycleOnlyPlayCreatesNoGameplayActors)
{
	Durin::DWorld* World = CreateWorld();
	World->SpawnActor<Durin::ACameraActor>("AuthoredCamera");
	const size_t ActorCount = World->GetActors().size();
	ASSERT_TRUE(World->BeginPlay({}));
	EXPECT_EQ(World->GetActors().size(), ActorCount);
	EXPECT_EQ(World->GetGameMode(), nullptr);
	EXPECT_EQ(World->GetLocalPlayerController(), nullptr);
	EXPECT_EQ(World->GetDefaultPawn(), nullptr);
	World->EndPlay();
	EXPECT_EQ(World->GetActors().size(), ActorCount);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayBootstrapTests, UsesFirstStablePlayerStartAndTearsDownOnlyRuntimeActors)
{
	Durin::DWorld* World = CreateWorld();
	auto* FirstStart = SpawnPlayerStart(*World, "FirstStart", {1.0, 2.0, 3.0});
	auto* SecondStart = SpawnPlayerStart(*World, "SecondStart", {8.0, 9.0, 10.0});
	ASSERT_NE(FirstStart, nullptr);
	ASSERT_NE(SecondStart, nullptr);
	const size_t AuthoredCount = World->GetActors().size();
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = Durin::AGameMode::StaticClass()}));
	ASSERT_NE(World->GetGameMode(), nullptr);
	ASSERT_NE(World->GetLocalPlayerController(), nullptr);
	ASSERT_NE(World->GetDefaultPawn(), nullptr);
	EXPECT_EQ(World->GetActors().size(), AuthoredCount + 3);
	ExpectVectorNear(World->GetDefaultPawn()->GetActorTransform().Translation, {1.0, 2.0, 3.0});
	EXPECT_EQ(World->GetLocalPlayerController()->GetPawn(), World->GetDefaultPawn());
	EXPECT_EQ(World->GetDefaultPawn()->GetController(), World->GetLocalPlayerController());
	EXPECT_EQ(World->GetLocalPlayerController()->GetViewTarget(), World->GetDefaultPawn());
	World->EndPlay();
	EXPECT_EQ(World->GetActors().size(), AuthoredCount);
	EXPECT_EQ(World->GetGameMode(), nullptr);
	EXPECT_EQ(World->GetLocalPlayerController(), nullptr);
	EXPECT_EQ(World->GetDefaultPawn(), nullptr);
	EXPECT_TRUE(World->ContainsActor(FirstStart));
	EXPECT_TRUE(World->ContainsActor(SecondStart));
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayBootstrapTests, RollsBackMissingStartAndInvalidSelectedClass)
{
	Durin::DWorld* World = CreateWorld();
	auto* Authored = World->SpawnActor<Durin::ACameraActor>("AuthoredCamera");
	const size_t AuthoredCount = World->GetActors().size();
	Durin::FWorldPlayResult Result = World->BeginPlay({.GameModeClass = Durin::AGameMode::StaticClass()});
	EXPECT_EQ(Result.Error, Durin::EWorldPlayError::MissingPlayerStart);
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_EQ(World->GetActors().size(), AuthoredCount);
	EXPECT_TRUE(World->ContainsActor(Authored));
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	const size_t CountWithStart = World->GetActors().size();
	Result = World->BeginPlay({.GameModeClass = InvalidPawnGameModeClass()});
	EXPECT_EQ(Result.Error, Durin::EWorldPlayError::InvalidPawnClass);
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_EQ(World->GetActors().size(), CountWithStart);
	Result = World->BeginPlay({.GameModeClass = UnplaceablePawnGameModeClass()});
	EXPECT_EQ(Result.Error, Durin::EWorldPlayError::PawnPlacementFailed);
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_EQ(World->GetActors().size(), CountWithStart);
	Durin::DWorld* ForeignWorld = CreateWorld();
	auto* ForeignCamera = ForeignWorld->SpawnActor<Durin::ACameraActor>("ForeignCamera");
	Result = World->BeginPlay({
		.GameModeClass = Durin::AGameMode::StaticClass(),
		.ViewTargetOverride = ForeignCamera});
	EXPECT_EQ(Result.Error, Durin::EWorldPlayError::ViewTargetRejected);
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_EQ(World->GetActors().size(), CountWithStart);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkObjectHierarchyAsGarbage(ForeignWorld);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayBootstrapTests, RestartsPawnAndLeavesControllerUnpossessedOnFailure)
{
	Durin::DWorld* World = CreateWorld();
	auto* Start = SpawnPlayerStart(*World, "Start", {3.0, 4.0, 5.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = IntentGameModeClass()}));
	auto* Controller = World->GetLocalPlayerController();
	auto* OldPawn = static_cast<FIntentPawn*>(World->GetDefaultPawn());
	ASSERT_NE(Controller, nullptr);
	ASSERT_NE(OldPawn, nullptr);
	EXPECT_TRUE(OldPawn->WasPossessedAtBeginPlay());
	ExpectVectorNear(OldPawn->GetBeginPlayTransform().Translation, {3.0, 4.0, 5.0});
	const Durin::FPlayerRestartResult Restart = World->RestartPlayer();
	ASSERT_TRUE(Restart);
	EXPECT_EQ(World->GetLocalPlayerController(), Controller);
	EXPECT_NE(Restart.Pawn, OldPawn);
	EXPECT_FALSE(Durin::IsValid(OldPawn));
	EXPECT_EQ(Controller->GetPawn(), Restart.Pawn);
	EXPECT_EQ(Controller->GetViewTarget(), Restart.Pawn);
	ExpectVectorNear(Restart.Pawn->GetActorTransform().Translation, {3.0, 4.0, 5.0});
	auto* RestartedPawn = static_cast<FIntentPawn*>(Restart.Pawn);
	EXPECT_TRUE(RestartedPawn->WasPossessedAtBeginPlay());
	ExpectVectorNear(RestartedPawn->GetBeginPlayTransform().Translation, {3.0, 4.0, 5.0});
	ASSERT_TRUE(World->DestroyActor(Start));
	const Durin::FPlayerRestartResult FailedRestart = World->RestartPlayer();
	EXPECT_EQ(FailedRestart.Error, Durin::EPlayerRestartError::MissingPlayerStart);
	EXPECT_EQ(World->GetLocalPlayerController(), Controller);
	EXPECT_EQ(World->GetDefaultPawn(), nullptr);
	EXPECT_EQ(Controller->GetPawn(), nullptr);
	World->EndPlay();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayBootstrapTests, RestartDefersLevelReplacementRequestedByPawnBeginPlay)
{
	FIntentPawnCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = IntentGameModeClass()}));
	Durin::DLevel* OriginalLevel = World->GetCurrentLevel();
	Durin::DLevel* Replacement = Durin::NewObject<Durin::DLevel>(World, "ReplacementLevel");
	ASSERT_NE(Replacement, nullptr);
	ASSERT_NE(Replacement->SpawnActor<Durin::APlayerStart>("ReplacementStart"), nullptr);
	FIntentPawn::BeginPlayCallback = [World, Replacement](FIntentPawn&)
	{
		EXPECT_TRUE(World->RequestLevelTransition(Replacement));
	};

	const Durin::FPlayerRestartResult Restart = World->RestartPlayer();

	EXPECT_EQ(Restart.Error, Durin::EPlayerRestartError::RestartAborted);
	EXPECT_EQ(World->GetCurrentLevel(), OriginalLevel);
	FIntentPawn::BeginPlayCallback = {};
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Replacement);
	EXPECT_TRUE(World->HasBegunPlay());
	EXPECT_NE(World->GetDefaultPawn(), nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayBootstrapTests, RestartDefersLevelClearRequestedByPawnBeginPlay)
{
	FIntentPawnCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = IntentGameModeClass()}));
	Durin::DLevel* OriginalLevel = World->GetCurrentLevel();
	FIntentPawn::BeginPlayCallback = [World](FIntentPawn&)
	{
		EXPECT_TRUE(World->RequestLevelTransition(nullptr));
	};

	const Durin::FPlayerRestartResult Restart = World->RestartPlayer();

	EXPECT_EQ(Restart.Error, Durin::EPlayerRestartError::RestartAborted);
	EXPECT_EQ(World->GetCurrentLevel(), OriginalLevel);
	FIntentPawn::BeginPlayCallback = {};
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), nullptr);
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_EQ(World->GetDefaultPawn(), nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayBootstrapTests, RestartRejectsSynchronousLevelClearFromPawnBeginPlay)
{
	FIntentPawnCallbackScope CallbackScope;
	Durin::DWorld* World = CreateWorld();
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = IntentGameModeClass()}));
	Durin::DLevel* OriginalLevel = World->GetCurrentLevel();
	bool bClearRejected = false;
	FIntentPawn::BeginPlayCallback = [World, &bClearRejected](FIntentPawn&)
	{
		bClearRejected = !World->SetCurrentLevel(nullptr);
	};

	const Durin::FPlayerRestartResult Restart = World->RestartPlayer();

	EXPECT_TRUE(bClearRejected);
	ASSERT_TRUE(Restart);
	EXPECT_EQ(World->GetCurrentLevel(), OriginalLevel);
	EXPECT_EQ(World->GetDefaultPawn(), Restart.Pawn);
	EXPECT_EQ(World->GetLocalPlayerController()->GetPawn(), Restart.Pawn);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayControlTests, ClampsConsumesAndSuppressesIntentAcrossPause)
{
	Durin::DWorld* World = CreateWorld();
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = IntentGameModeClass()}));
	auto* Controller = static_cast<FIntentPlayerController*>(World->GetLocalPlayerController());
	auto* Pawn = static_cast<FIntentPawn*>(World->GetDefaultPawn());
	auto* Movement = static_cast<FIntentMovementComponent*>(
		Pawn->AddInstanceComponent(IntentMovementClass(), "IntentMovement"));
	auto* ForeignPawn = static_cast<FIntentPawn*>(
		World->SpawnActor(IntentPawnClass(), "ForeignMovementPawn"));
	auto* ForeignMovement = static_cast<FIntentMovementComponent*>(
		ForeignPawn->AddInstanceComponent(IntentMovementClass(), "ForeignIntentMovement"));
	ASSERT_NE(Controller, nullptr);
	ASSERT_NE(Pawn, nullptr);
	ASSERT_NE(Movement, nullptr);
	ASSERT_NE(ForeignPawn, nullptr);
	ASSERT_NE(ForeignMovement, nullptr);
	EXPECT_FALSE(Pawn->SelectMovement(ForeignMovement));
	ASSERT_TRUE(Pawn->SelectMovement(Movement));
	EXPECT_FALSE(Pawn->SelectMovement(Movement));

	Durin::FGameInputState Input;
	Durin::FPawnControlIntent Intent;
	Intent.Move = {5.0, -2.0};
	Intent.Look = {1.5, -9.0};
	Intent.bJumpHeld = true;
	Intent.bJumpPressed = true;
	Controller->SetNextIntent(Intent);
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_EQ(Movement->GetUpdateCount(), 1u);
	EXPECT_EQ(Movement->GetLastIntent().Move, Durin::FVector2(1.0, -1.0));
	EXPECT_EQ(Movement->GetLastIntent().Look, Durin::FVector2(1.0, -1.0));
	EXPECT_TRUE(Movement->GetLastIntent().bJumpPressed);
	EXPECT_FALSE(Pawn->HasPendingControlIntent());

	World->SetPaused(true);
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_EQ(Movement->GetUpdateCount(), 1u);
	EXPECT_EQ(Controller->GetBuildCount(), 1u);
	World->RequestSingleStep();
	Intent.bJumpPressed = false;
	Controller->SetNextIntent(Intent);
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_EQ(Movement->GetUpdateCount(), 2u);
	EXPECT_EQ(Controller->GetBuildCount(), 2u);
	EXPECT_FALSE(Movement->GetLastIntent().bJumpPressed);

	Durin::FPawnControlIntent Invalid;
	Invalid.Move.x = std::numeric_limits<double>::quiet_NaN();
	Controller->SetNextIntent(Invalid);
	World->RequestSingleStep();
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_EQ(Movement->GetUpdateCount(), 2u);
	World->SetPaused(false);
	Durin::FPawnControlIntent Direct;
	Direct.Move = {0.25, 0.5};
	ASSERT_TRUE(Controller->SubmitDirect(Direct));
	World->Tick({.DeltaSeconds = 1.0f / 60.0f});
	EXPECT_EQ(Movement->GetUpdateCount(), 3u);
	EXPECT_EQ(Movement->GetLastIntent().Move, Direct.Move);
	World->EndPlay();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayControlTests, RawTransitionsBecomeOneUseIntentAndFocusLossResetsState)
{
	Durin::DWorld* World = CreateWorld();
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = IntentGameModeClass()}));
	auto* Controller = static_cast<FIntentPlayerController*>(World->GetLocalPlayerController());
	auto* Pawn = static_cast<FIntentPawn*>(World->GetDefaultPawn());
	auto* Movement = static_cast<FIntentMovementComponent*>(
		Pawn->AddInstanceComponent(IntentMovementClass(), "RawIntentMovement"));
	ASSERT_NE(Controller, nullptr);
	ASSERT_NE(Pawn, nullptr);
	ASSERT_NE(Movement, nullptr);
	ASSERT_TRUE(Pawn->SelectMovement(Movement));
	Controller->SetUseRawMapping(true);

	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::Space, true);
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_EQ(Movement->GetLastIntent().Move.y, 1.0);
	EXPECT_TRUE(Movement->GetLastIntent().bJumpHeld);
	EXPECT_TRUE(Movement->GetLastIntent().bJumpPressed);
	Durin::FGameInputStateTestAccess::FinishTick(Input);
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_TRUE(Movement->GetLastIntent().bJumpHeld);
	EXPECT_FALSE(Movement->GetLastIntent().bJumpPressed);
	Durin::FGameInputStateTestAccess::FinishTick(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::Space, false);
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_FALSE(Movement->GetLastIntent().bJumpHeld);
	EXPECT_TRUE(Movement->GetLastIntent().bJumpReleased);
	Durin::FGameInputStateTestAccess::SetFocused(Input, false);
	EXPECT_FALSE(Input.IsKeyDown(Durin::EKey::W));
	EXPECT_FALSE(Input.WasKeyReleased(Durin::EKey::Space));
	Durin::FGameInputStateTestAccess::SetEnabled(Input, false);
	EXPECT_FALSE(Input.IsEnabled());
	World->EndPlay();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayControlTests, DefersLevelReplacementRequestedByInputMapping)
{
	Durin::DWorld* World = CreateWorld();
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = IntentGameModeClass()}));
	auto* Controller = static_cast<FIntentPlayerController*>(World->GetLocalPlayerController());
	ASSERT_NE(Controller, nullptr);
	Controller->SetWorldToClearDuringBuild(World);
	Durin::FGameInputState Input;
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_NE(World->GetCurrentLevel(), nullptr);
	EXPECT_TRUE(World->HasBegunPlay());
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_EQ(World->GetCurrentLevel(), nullptr);
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_EQ(World->GetLocalPlayerController(), nullptr);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayViewTargetTests, RejectsForeignTargetsAndClearsDestroyedTargets)
{
	Durin::DWorld* World = CreateWorld();
	SpawnPlayerStart(*World, "Start", {0.0, 0.0, 0.0});
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = Durin::AGameMode::StaticClass()}));
	auto* Controller = World->GetLocalPlayerController();
	auto* Camera = World->SpawnActor<Durin::ACameraActor>("OverrideCamera");
	ASSERT_TRUE(Controller->SetViewTarget(Camera));
	EXPECT_EQ(Controller->GetViewTarget(), Camera);
	Durin::DWorld* ForeignWorld = CreateWorld();
	auto* ForeignCamera = ForeignWorld->SpawnActor<Durin::ACameraActor>("ForeignCamera");
	const Durin::FViewTargetResult Rejected = Controller->SetViewTarget(ForeignCamera);
	EXPECT_EQ(Rejected.Error, Durin::EViewTargetError::InvalidMembership);
	EXPECT_EQ(Controller->GetViewTarget(), Camera);
	ASSERT_TRUE(World->DestroyActor(Camera));
	EXPECT_EQ(Controller->GetViewTarget(), nullptr);
	World->EndPlay();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::MarkObjectHierarchyAsGarbage(ForeignWorld);
	Durin::CollectGarbage();
}

TEST(FEditorPlayViewportTests, SeedsAndIsolatesNewWindowRenderSettingsPerSession)
{
	InitializeDObjectSystem();
	auto* Engine = Durin::NewObject<Durin::DEditorEngine>(nullptr, "PlayViewportSettingsEngine");
	Durin::FViewportClient EditorClient;
	EditorClient.SetViewSettings({
		.Mode = {
			.RenderMode = Durin::ERenderMode::Unlit,
			.RasterMode = Durin::ERasterMode::Wireframe,
		},
		.PostProcess = {.bEnableFXAA = false, .ExposureEV = -1.25f},
		.DirectionalShadow = {.bEnableContactShadows = true},
	});

	Durin::FEditorEngineTestAccess::InitializePlayWindowViewportClient(*Engine, &EditorClient);
	Durin::FViewportClient* PlayClient = Engine->GetPlayViewportRenderSettingsClient();
	ASSERT_NE(PlayClient, nullptr);
	EXPECT_NE(PlayClient, &EditorClient);
	EXPECT_FALSE(PlayClient->GetViewSettings().PostProcess.bEnableFXAA);
	EXPECT_FLOAT_EQ(PlayClient->GetViewSettings().PostProcess.ExposureEV, -1.25f);
	EXPECT_EQ(PlayClient->GetViewSettings().Mode.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(PlayClient->GetViewSettings().Mode.RasterMode, Durin::ERasterMode::Wireframe);
	EXPECT_TRUE(PlayClient->GetViewSettings().DirectionalShadow.bEnableContactShadows);

	Durin::FSceneViewSettings PlaySettings = PlayClient->GetViewSettings();
	PlaySettings.Mode.RenderMode = Durin::ERenderMode::Lit;
	PlaySettings.Mode.RasterMode = Durin::ERasterMode::Solid;
	PlaySettings.PostProcess.ExposureEV = 2.0f;
	PlayClient->SetViewSettings(PlaySettings);
	EXPECT_EQ(EditorClient.GetViewSettings().Mode.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(EditorClient.GetViewSettings().Mode.RasterMode, Durin::ERasterMode::Wireframe);
	EXPECT_FLOAT_EQ(EditorClient.GetViewSettings().PostProcess.ExposureEV, -1.25f);

	Durin::FEditorEngineTestAccess::InitializePlayWindowViewportClient(*Engine, &EditorClient);
	PlayClient = Engine->GetPlayViewportRenderSettingsClient();
	ASSERT_NE(PlayClient, nullptr);
	EXPECT_EQ(PlayClient->GetViewSettings().Mode.RenderMode, Durin::ERenderMode::Unlit);
	EXPECT_EQ(PlayClient->GetViewSettings().Mode.RasterMode, Durin::ERasterMode::Wireframe);
	EXPECT_FLOAT_EQ(PlayClient->GetViewSettings().PostProcess.ExposureEV, -1.25f);
	Durin::FEditorEngineTestAccess::InitializePlayWindowViewportClient(*Engine, nullptr);
	PlayClient = Engine->GetPlayViewportRenderSettingsClient();
	ASSERT_NE(PlayClient, nullptr);
	EXPECT_TRUE(PlayClient->GetViewSettings().PostProcess.bEnableFXAA);
	EXPECT_EQ(PlayClient->GetViewSettings().Mode.RenderMode, Durin::ERenderMode::Lit);
	EXPECT_EQ(PlayClient->GetViewSettings().Mode.RasterMode, Durin::ERasterMode::Solid);
	EXPECT_FLOAT_EQ(PlayClient->GetViewSettings().PostProcess.ExposureEV, 0.0f);

	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage();
}

TEST(FNativeGameplayPIETests, RepeatsNativeLevelStartAndEditorCameraSessionsWithoutStaleRoles)
{
	InitializeDObjectSystem();
	auto* Engine = Durin::NewObject<Durin::DEditorEngine>(nullptr, "NativeGameplayPIETestEngine");
	auto* EditorWorld = Durin::NewObject<Durin::DWorld>(Engine, "EditorWorld");
	EditorWorld->SetWorldType(Durin::EWorldType::Editor);
	auto* EditorLevel = Durin::NewObject<Durin::DLevel>(EditorWorld, "EditorLevel");
	ASSERT_TRUE(EditorWorld->SetCurrentLevel(EditorLevel));
	ASSERT_NE(EditorLevel->SpawnActor<Durin::APlayerStart>("Start"), nullptr);
	auto* AuthoredCamera = EditorLevel->SpawnActor<Durin::ACameraActor>("AuthoredCamera");
	ASSERT_NE(AuthoredCamera, nullptr);
	Durin::FEditorEngineTestAccess::Initialize(*Engine, EditorWorld);

	Durin::Editor::FPlayRequest Request;
	Request.SourceLevel = EditorLevel;
	std::string Error;
	ASSERT_TRUE(Durin::FEditorEngineTestAccess::StartPlaySession(
		*Engine, Request, Durin::AGameMode::StaticClass(), &Error)) << Error;
	ASSERT_EQ(Engine->GetPlayState(), Durin::Editor::EPlayState::Playing);
	ASSERT_NE(Engine->GetPlayWorld(), nullptr);
	ASSERT_NE(Engine->GetPlayWorld()->GetLocalPlayerController(), nullptr);
	ASSERT_NE(Engine->GetPlayWorld()->GetDefaultPawn(), nullptr);
	EXPECT_EQ(
		Engine->GetPlayWorld()->GetLocalPlayerController()->GetViewTarget(),
		Engine->GetPlayWorld()->GetDefaultPawn());
	EXPECT_FALSE(EditorWorld->IsCollisionDebugDrawEnabled());
	EXPECT_FALSE(Engine->GetPlayWorld()->IsCollisionDebugDrawEnabled());
	Engine->GetPlayWorld()->SetCollisionDebugDrawEnabled(true);
	EXPECT_TRUE(Engine->GetPlayWorld()->IsCollisionDebugDrawEnabled());
	EXPECT_FALSE(EditorWorld->IsCollisionDebugDrawEnabled());
	Engine->SetPlaySessionPaused(true);
	EXPECT_TRUE(Engine->GetPlayWorld()->IsPaused());
	Engine->StepPlaySession();
	Engine->Tick(1.0f / 60.0f, false);
	ASSERT_TRUE(Engine->GetPlayWorld()->RestartPlayer());
	Engine->StopPlaySession();
	EXPECT_EQ(Engine->GetPlayState(), Durin::Editor::EPlayState::Stopped);
	EXPECT_EQ(Engine->GetWorld(), EditorWorld);
	EXPECT_EQ(EditorWorld->GetCurrentLevel(), EditorLevel);
	EXPECT_FALSE(EditorWorld->IsCollisionDebugDrawEnabled());

	Request.StartLocation = Durin::Editor::EPlayStartLocation::EditorCamera;
	Request.CameraLocation = {11.0, 12.0, 13.0};
	Request.CameraTarget = {12.0, 12.0, 13.0};
	ASSERT_TRUE(Durin::FEditorEngineTestAccess::StartPlaySession(
		*Engine, Request, Durin::AGameMode::StaticClass(), &Error)) << Error;
	auto* Controller = Engine->GetPlayWorld()->GetLocalPlayerController();
	ASSERT_NE(Controller, nullptr);
	ASSERT_NE(Controller->GetViewTarget(), nullptr);
	EXPECT_EQ(Controller->GetViewTarget()->GetFName(), Durin::FName("PIE_EditorCamera"));
	EXPECT_NE(Controller->GetViewTarget()->FindComponentByClass<Durin::DCameraComponent>(), nullptr);
	Engine->StopPlaySession();
	EXPECT_EQ(Engine->GetWorld(), EditorWorld);
	EXPECT_EQ(EditorWorld->GetCurrentLevel(), EditorLevel);
	EXPECT_EQ(EditorLevel->GetActors().size(), 2u);

	ASSERT_FALSE(Durin::FEditorEngineTestAccess::StartPlaySession(
		*Engine, Request, Durin::AActor::StaticClass(), &Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Engine->GetPlayState(), Durin::Editor::EPlayState::Stopped);
	EXPECT_EQ(Engine->GetPlayWorld(), nullptr);
	EXPECT_EQ(Engine->GetWorld(), EditorWorld);

	Durin::MarkObjectHierarchyAsGarbage(Engine);
	Durin::CollectGarbage();
}
