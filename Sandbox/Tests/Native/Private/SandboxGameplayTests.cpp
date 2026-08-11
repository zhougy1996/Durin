#include "Actors/PlayerStart.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DefaultGameMode.h"
#include "DefaultPlayerController.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/World.h"
#include "Input/GameInputState.h"
#include "Input/InputCoreTypes.h"
#include "NativeDObjectTestSupport.h"
#include "PlayerPawn.h"
#include "SandboxGameplayTuning.h"
#include "SimpleGroundMovementComponent.h"

#include <gtest/gtest.h>

namespace Durin
{
	struct FGameInputStateTestAccess
	{
		static auto EnableAndFocus(FGameInputState& Input) -> void
		{
			Input.SetEnabled(true);
			Input.SetFocused(true);
		}

		static auto SetKey(FGameInputState& Input, EKey Key, bool bDown) -> void
		{
			Input.SetKey(Key, bDown);
		}

		static auto SetMousePosition(FGameInputState& Input, FVector2d Position) -> void
		{
			Input.SetMousePosition(Position);
		}

		static auto FinishTick(FGameInputState& Input) -> void
		{
			Input.FinishGameTick();
		}
	};
}

namespace Durin::Sandbox
{
	struct FDefaultPlayerControllerTestAccess
	{
		static auto Build(const ADefaultPlayerController& Controller, const FGameInputState& Input) -> FPawnControlIntent
		{
			return Controller.BuildControlIntent(Input);
		}
	};
}

namespace
{
	auto ResolveSandboxGameMode() -> Durin::DClass*
	{
		Durin::FProjectGameSettings Settings;
		Settings.NativeModule = "Sandbox";
		Settings.GameModeClass = "Durin::Sandbox::ADefaultGameMode";
		const Durin::FNativeGameModeResolution Resolution = Durin::ResolveNativeGameMode(Settings);
		EXPECT_TRUE(Resolution.Result) << Resolution.Result.Message;
		return Resolution.GameModeClass;
	}

	auto CreateGameplayWorld() -> Durin::DWorld*
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		ResolveSandboxGameMode();
		auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SandboxGameplayWorld");
		auto* Level = Durin::NewObject<Durin::DLevel>(World, "SandboxGameplayLevel");
		EXPECT_TRUE(World->SetCurrentLevel(Level));
		auto* Start = Level->SpawnActor<Durin::APlayerStart>("PlayerStart");
		EXPECT_NE(Start, nullptr);
		return World;
	}

	auto BeginGameplay(Durin::DWorld& World) -> Durin::Sandbox::APlayerPawn*
	{
		const Durin::FWorldPlayResult Result = World.BeginPlay({.GameModeClass = ResolveSandboxGameMode()});
		EXPECT_TRUE(Result) << Result.Message;
		return Durin::Cast<Durin::Sandbox::APlayerPawn>(World.GetDefaultPawn());
	}

	auto DestroyWorld(Durin::DWorld* World) -> void
	{
		if (World && World->HasBegunPlay()) World->EndPlay();
		Durin::MarkObjectHierarchyAsGarbage(World);
		Durin::CollectGarbage();
	}

	auto SimulateForward(double FrameRate, double Seconds) -> Durin::FVector3
	{
		Durin::DWorld* World = CreateGameplayWorld();
		Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
		Durin::FGameInputState Input;
		Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
		Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
		const int FrameCount = static_cast<int>(FrameRate * Seconds);
		for (int Frame = 0; Frame < FrameCount; ++Frame)
		{
			World->Tick({.DeltaSeconds = static_cast<float>(1.0 / FrameRate), .GameInput = &Input});
			Durin::FGameInputStateTestAccess::FinishTick(Input);
		}
		const Durin::FVector3 Location = Pawn->GetActorTransform().Translation;
		DestroyWorld(World);
		return Location;
	}
}

TEST(FSandboxGameplayReflectionTests, ResolvesExactConcreteRolesAcrossTheProjectModule)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::DClass* GameModeClass = ResolveSandboxGameMode();
	ASSERT_NE(GameModeClass, nullptr);
	EXPECT_EQ(GameModeClass->GetQualifiedName().ToString(), "Durin::Sandbox::ADefaultGameMode");
	EXPECT_EQ(Durin::Sandbox::ADefaultPlayerController::StaticClass()->GetQualifiedName().ToString(),
		"Durin::Sandbox::ADefaultPlayerController");
	EXPECT_EQ(Durin::Sandbox::APlayerPawn::StaticClass()->GetQualifiedName().ToString(),
		"Durin::Sandbox::APlayerPawn");
	EXPECT_EQ(Durin::Sandbox::DSimpleGroundMovementComponent::StaticClass()->GetQualifiedName().ToString(),
		"Durin::Sandbox::DSimpleGroundMovementComponent");
	EXPECT_TRUE(GameModeClass->IsChildOf(Durin::AGameMode::StaticClass()));
}

TEST(FSandboxGameplayInputTests, MapsDigitalCancellationJumpEdgesAndMouseDelta)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	ResolveSandboxGameMode();
	Durin::Sandbox::ADefaultPlayerController Controller(Durin::FObjectInitializer::Get());
	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::S, true);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::D, true);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::Space, true);
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {10.0, 20.0});
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {13.0, 15.0});
	const Durin::FPawnControlIntent Intent =
		Durin::Sandbox::FDefaultPlayerControllerTestAccess::Build(Controller, Input);
	EXPECT_DOUBLE_EQ(Intent.Move.x, 1.0);
	EXPECT_DOUBLE_EQ(Intent.Move.y, 0.0);
	EXPECT_TRUE(Intent.bJumpHeld);
	EXPECT_TRUE(Intent.bJumpPressed);
	EXPECT_FALSE(Intent.bJumpReleased);
	EXPECT_DOUBLE_EQ(Intent.Look.x, 0.3);
	EXPECT_DOUBLE_EQ(Intent.Look.y, 0.5);
}

TEST(FSandboxGameplayBootstrapTests, SelectsOwnsRestartsAndTearsDownConcreteRoles)
{
	Durin::DWorld* World = CreateGameplayWorld();
	const size_t AuthoredActorCount = World->GetActors().size();
	Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
	ASSERT_NE(Pawn, nullptr);
	ASSERT_NE(World->GetLocalPlayerController(), nullptr);
	EXPECT_EQ(World->GetActors().size(), AuthoredActorCount + 3);
	EXPECT_EQ(World->GetLocalPlayerController()->GetClass(),
		Durin::Sandbox::ADefaultPlayerController::StaticClass());
	EXPECT_EQ(World->GetLocalPlayerController()->GetPawn(), Pawn);
	EXPECT_EQ(World->GetLocalPlayerController()->GetViewTarget(), Pawn);
	EXPECT_EQ(Pawn->GetMovementComponent(), Pawn->GetGroundMovementComponent());
	EXPECT_NE(Pawn->GetVisualComponent(), nullptr);
	EXPECT_NE(Pawn->GetCameraComponent(), nullptr);
	EXPECT_EQ(Pawn->FindComponentsByClass<Durin::DPawnMovementComponent>().size(), 1u);
	const Durin::FPlayerRestartResult Restart = World->RestartPlayer();
	ASSERT_TRUE(Restart) << Restart.Message;
	EXPECT_NE(Restart.Pawn, Pawn);
	EXPECT_EQ(World->GetLocalPlayerController()->GetPawn(), Restart.Pawn);
	World->EndPlay();
	EXPECT_EQ(World->GetActors().size(), AuthoredActorCount);
	DestroyWorld(World);
}

TEST(FSandboxGameplayMovementTests, AcceleratesJumpsLandsAndAppliesBoundedLook)
{
	Durin::DWorld* World = CreateGameplayWorld();
	Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
	ASSERT_NE(Pawn, nullptr);
	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::Space, true);
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {0.0, 0.0});
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {1000.0, -1000.0});
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	EXPECT_GT(Pawn->GetActorTransform().Translation.x, 0.0);
	EXPECT_GT(Pawn->GetActorTransform().Translation.z, 0.0);
	EXPECT_GT(Pawn->GetGroundMovementComponent()->GetVelocity().z, 0.0);
	EXPECT_DOUBLE_EQ(Pawn->GetYawDegrees(), Durin::Sandbox::GameplayTuning::LookDegreesPerIntent);
	EXPECT_DOUBLE_EQ(Pawn->GetPitchDegrees(), Durin::Sandbox::GameplayTuning::LookDegreesPerIntent);
	Durin::FGameInputStateTestAccess::FinishTick(Input);
	for (int Frame = 0; Frame < 120; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
		Durin::FGameInputStateTestAccess::FinishTick(Input);
	}
	EXPECT_DOUBLE_EQ(Pawn->GetActorTransform().Translation.z, Durin::Sandbox::GameplayTuning::GroundHeight);
	EXPECT_TRUE(Pawn->GetGroundMovementComponent()->IsGrounded());
	EXPECT_LE(Durin::Math::Length(Durin::FVector2(
		Pawn->GetGroundMovementComponent()->GetVelocity().x,
		Pawn->GetGroundMovementComponent()->GetVelocity().y)),
		Durin::Sandbox::GameplayTuning::MaximumHorizontalSpeed);
	DestroyWorld(World);
}

TEST(FSandboxGameplayMovementTests, MatchesTheFocusedFrameRateMatrix)
{
	const Durin::FVector3 ThirtyHz = SimulateForward(30.0, 2.0);
	const Durin::FVector3 SixtyHz = SimulateForward(60.0, 2.0);
	const Durin::FVector3 OneTwentyHz = SimulateForward(120.0, 2.0);
	EXPECT_NEAR(ThirtyHz.x, SixtyHz.x, 1.e-5);
	EXPECT_NEAR(SixtyHz.x, OneTwentyHz.x, 1.e-5);
	EXPECT_DOUBLE_EQ(ThirtyHz.y, 0.0);
	EXPECT_DOUBLE_EQ(ThirtyHz.z, Durin::Sandbox::GameplayTuning::GroundHeight);
}
