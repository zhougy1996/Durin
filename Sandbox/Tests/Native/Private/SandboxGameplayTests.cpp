#include "Actors/PlayerStart.h"
#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "Components/ShapeComponent.h"
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
#include "Physics/PhysicsScene.h"
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
	auto AddCollisionBox(
		Durin::DWorld& World,
		std::string_view Name,
		const Durin::FVector3& Center,
		const Durin::FVector3& HalfExtent,
		const Durin::FQuat& Rotation = Durin::FQuatConstants::Identity) -> Durin::DBoxComponent*
	{
		auto* Owner = World.SpawnActor<Durin::ACameraActor>(Durin::FName(Name));
		auto* Box = Durin::Cast<Durin::DBoxComponent>(
			Owner->AddInstanceComponent(Durin::DBoxComponent::StaticClass(), "Collision"));
		EXPECT_NE(Box, nullptr);
		EXPECT_TRUE(Box->SetBoxHalfExtent(HalfExtent));
		EXPECT_TRUE(Box->SetCollisionProfileName(Durin::CollisionProfile::WorldStatic));
		Durin::FTransform Transform;
		Transform.Translation = Center;
		Transform.Rotation = Rotation;
		Box->SetWorldTransform(Transform);
		return Box;
	}

	auto ResolveSandboxGameMode() -> Durin::DClass*
	{
		Durin::FProjectGameSettings Settings;
		Settings.NativeModule = "Sandbox";
		Settings.GameModeClass = "Durin::Sandbox::ADefaultGameMode";
		const Durin::FNativeGameModeResolution Resolution = Durin::ResolveNativeGameMode(Settings);
		EXPECT_TRUE(Resolution.Result) << Resolution.Result.Message;
		return Resolution.GameModeClass;
	}

	auto CreateGameplayWorld(bool bAddFloor = true) -> Durin::DWorld*
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		ResolveSandboxGameMode();
		auto* World = Durin::NewObject<Durin::DWorld>(nullptr, "SandboxGameplayWorld");
		auto* Level = Durin::NewObject<Durin::DLevel>(World, "SandboxGameplayLevel");
		EXPECT_TRUE(World->SetCurrentLevel(Level));
		auto* Start = Level->SpawnActor<Durin::APlayerStart>("PlayerStart");
		EXPECT_NE(Start, nullptr);
		if (bAddFloor) AddCollisionBox(*World, "Floor", {0.0, 0.0, -0.5}, {100.0, 100.0, 0.5});
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

	auto SimulateForward(
		double FrameRate,
		double Seconds,
		Durin::EPhysicsSceneQueryExecutionPolicy Policy = Durin::EPhysicsSceneQueryExecutionPolicy::Production)
		-> Durin::FVector3
	{
		Durin::DWorld* World = CreateGameplayWorld();
		EXPECT_TRUE(World->GetPhysicsScene().SetQueryExecutionPolicy(Policy));
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

	struct FSandboxMeasurementResult
	{
		Durin::FPhysicsSceneQueryDiagnostics Diagnostics;
		Durin::FVector3 Location{0.0};
		bool bGrounded = false;
		Durin::uint64 ElapsedNanoseconds = 0;
	};

	template <typename Setup, typename Prepare, typename Run>
	auto ExecuteSandboxMeasurement(
		bool bAddFloor,
		Setup&& SetupWorld,
		Prepare&& PrepareRun,
		Run&& RunSequence) -> FSandboxMeasurementResult
	{
		Durin::DWorld* World = CreateGameplayWorld(bAddFloor);
		SetupWorld(*World);
		Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
		EXPECT_NE(Pawn, nullptr);
		Durin::FGameInputState Input;
		Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
		PrepareRun(*World, *Pawn, Input);
		EXPECT_TRUE(World->GetPhysicsScene().SetDetailedQueryDiagnosticsEnabled(false));
		EXPECT_TRUE(World->GetPhysicsScene().ResetQueryDiagnostics());
		const auto Start = std::chrono::steady_clock::now();
		RunSequence(*World, *Pawn, Input);
		const Durin::uint64 Elapsed = static_cast<Durin::uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		FSandboxMeasurementResult Result{
			.Diagnostics = World->GetPhysicsScene().CaptureQueryDiagnostics(),
			.Location = Pawn->GetActorTransform().Translation,
			.bGrounded = Pawn->GetGroundMovementComponent()->IsGrounded(),
			.ElapsedNanoseconds = Elapsed};
		DestroyWorld(World);
		return Result;
	}

	template <typename Setup, typename Prepare, typename Run, typename Verify>
	auto MeasureSandboxCase(
		std::string_view CaseName,
		bool bAddFloor,
		Setup&& SetupWorld,
		Prepare&& PrepareRun,
		Run&& RunSequence,
		Verify&& VerifyResult) -> void
	{
		constexpr Durin::uint32 WarmupCount = 3;
		constexpr Durin::uint32 SampleCount = 11;
		for (Durin::uint32 Warmup = 0; Warmup < WarmupCount; ++Warmup)
		{
			const FSandboxMeasurementResult Result = ExecuteSandboxMeasurement(
				bAddFloor, SetupWorld, PrepareRun, RunSequence);
			VerifyResult(Result);
		}

		std::vector<Durin::uint64> Samples;
		Samples.reserve(SampleCount);
		FSandboxMeasurementResult LastResult;
		Durin::uint64 Checksum = 0;
		for (Durin::uint32 Sample = 0; Sample < SampleCount; ++Sample)
		{
			LastResult = ExecuteSandboxMeasurement(bAddFloor, SetupWorld, PrepareRun, RunSequence);
			VerifyResult(LastResult);
			Samples.push_back(LastResult.ElapsedNanoseconds);
			Checksum += static_cast<Durin::uint64>(
				std::abs(LastResult.Location.x) * 1'000.0
				+ std::abs(LastResult.Location.y) * 1'000.0
				+ std::abs(LastResult.Location.z) * 1'000.0)
				+ (LastResult.bGrounded ? 7u : 3u);
		}
		std::ranges::sort(Samples);
		const auto& Query = LastResult.Diagnostics.Queries;
		const Durin::FPhysicsSceneQueryCounters& Line =
			Query[static_cast<size_t>(Durin::EPhysicsSceneQueryKind::LineTraceSingle)];
		const Durin::FPhysicsSceneQueryCounters& Sweep =
			Query[static_cast<size_t>(Durin::EPhysicsSceneQueryKind::SweepSingle)];
		const Durin::FPhysicsSceneQueryCounters& Overlap =
			Query[static_cast<size_t>(Durin::EPhysicsSceneQueryKind::OverlapMulti)];
		auto Sum = [&](auto Member) {
			return Line.*Member + Sweep.*Member + Overlap.*Member;
		};
		std::cout << "SandboxAetherBaseline"
			<< ",case=" << CaseName
			<< ",warmups=" << WarmupCount
			<< ",samples=" << SampleCount
			<< ",median_ns=" << Samples[SampleCount / 2]
			<< ",p95_ns=" << Samples[SampleCount - 1]
			<< ",line_queries=" << Line.SubmittedQueries
			<< ",sweep_queries=" << Sweep.SubmittedQueries
			<< ",overlap_queries=" << Overlap.SubmittedQueries
			<< ",body_visits=" << Sum(&Durin::FPhysicsSceneQueryCounters::BodyVisits)
			<< ",filter_rejected=" << Sum(&Durin::FPhysicsSceneQueryCounters::FilterRejectedBodies)
			<< ",pair_tests=" << Sum(&Durin::FPhysicsSceneQueryCounters::NarrowPhasePairTests)
			<< ",distance_evaluations=" << Sum(&Durin::FPhysicsSceneQueryCounters::GeometryDistanceEvaluations)
			<< ",search_iterations=" << Sum(&Durin::FPhysicsSceneQueryCounters::GeometrySearchIterations)
			<< ",returned=" << Sum(&Durin::FPhysicsSceneQueryCounters::ReturnedResults)
			<< ",checksum=" << Checksum << '\n';
		EXPECT_GT(Checksum, 0u);
	}

	auto TickMeasurementFrames(
		Durin::DWorld& World,
		Durin::FGameInputState& Input,
		int FrameCount) -> void
	{
		for (int Frame = 0; Frame < FrameCount; ++Frame)
		{
			World.Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
			Durin::FGameInputStateTestAccess::FinishTick(Input);
		}
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
	EXPECT_DOUBLE_EQ(Intent.Look.x, 3.0 * Durin::Sandbox::GameplayTuning::MouseIntentPerPixel);
	EXPECT_DOUBLE_EQ(Intent.Look.y, 5.0 * Durin::Sandbox::GameplayTuning::MouseIntentPerPixel);
}

TEST(FSandboxGameplayInputTests, SuppressesOnlyMinorAxisMouseCrosstalk)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	ResolveSandboxGameMode();
	Durin::Sandbox::ADefaultPlayerController Controller(Durin::FObjectInitializer::Get());
	auto BuildLook = [&Controller](const Durin::FVector2d& Delta) {
		Durin::FGameInputState Input;
		Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
		Durin::FGameInputStateTestAccess::SetMousePosition(Input, {0.0, 0.0});
		Durin::FGameInputStateTestAccess::SetMousePosition(Input, Delta);
		return Durin::Sandbox::FDefaultPlayerControllerTestAccess::Build(Controller, Input).Look;
	};

	const double Scale = Durin::Sandbox::GameplayTuning::MouseIntentPerPixel;
	EXPECT_EQ(BuildLook({12.0, 1.0}), Durin::FVector2(12.0 * Scale, 0.0));
	EXPECT_EQ(BuildLook({1.0, 12.0}), Durin::FVector2(0.0, -12.0 * Scale));
	EXPECT_EQ(BuildLook({12.0, 2.0}), Durin::FVector2(12.0 * Scale, -2.0 * Scale));
	EXPECT_EQ(BuildLook({0.0, 1.0}), Durin::FVector2(0.0, -Scale));
}

TEST(FSandboxGameplayInputTests, HorizontalLookKeepsPitchAndCameraHeightStable)
{
	Durin::DWorld* World = CreateGameplayWorld();
	Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
	ASSERT_NE(Pawn, nullptr);
	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);

	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
	Durin::FGameInputStateTestAccess::FinishTick(Input);
	const double InitialPitch = Pawn->GetPitchDegrees();
	const double InitialPawnHeight = Pawn->GetActorTransform().Translation.z;
	const double InitialCameraHeight = Pawn->GetCameraComponent()->GetWorldLocation().z;

	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {0.0, 0.0});
	Durin::FGameInputStateTestAccess::SetMousePosition(Input, {12.0, 1.0});
	World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});

	EXPECT_DOUBLE_EQ(Pawn->GetPitchDegrees(), InitialPitch);
	EXPECT_NEAR(Pawn->GetActorTransform().Translation.z, InitialPawnHeight, 1.0e-7);
	EXPECT_NEAR(Pawn->GetCameraComponent()->GetWorldLocation().z, InitialCameraHeight, 1.0e-7);
	DestroyWorld(World);
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
	EXPECT_NE(Pawn->GetCapsuleComponent(), nullptr);
	EXPECT_EQ(Pawn->GetVisualComponent()->GetCollisionEnabled(), Durin::ECollisionEnabled::NoCollision);
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
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(
		Pawn->GetActorTransform().Rotation,
		Durin::Math::MakeQuaternionFromAxisAngleDegrees(
			Pawn->GetYawDegrees(), Durin::FVectorConstants::Up),
		1.e-8));
	EXPECT_TRUE(Durin::Math::AreRotationsEquivalent(
		Pawn->GetCameraComponent()->GetRelativeRotation(),
		Durin::Math::MakeQuaternionFromAxisAngleDegrees(
			-Pawn->GetPitchDegrees(), Durin::FVectorConstants::Right),
		1.e-8));
	Durin::FGameInputStateTestAccess::FinishTick(Input);
	for (int Frame = 0; Frame < 120; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
		Durin::FGameInputStateTestAccess::FinishTick(Input);
	}
	EXPECT_NEAR(Pawn->GetActorTransform().Translation.z, 0.0, 0.01);
	EXPECT_TRUE(Pawn->GetGroundMovementComponent()->IsGrounded());
	EXPECT_LE(Durin::Math::Length(Durin::FVector2(
		Pawn->GetGroundMovementComponent()->GetVelocity().x,
		Pawn->GetGroundMovementComponent()->GetVelocity().y)),
		Durin::Sandbox::GameplayTuning::MaximumHorizontalSpeed + 1.0e-9);
	DestroyWorld(World);
}

TEST(FSandboxGameplayMovementTests, MatchesTheFocusedFrameRateMatrix)
{
	const Durin::FVector3 ThirtyHz = SimulateForward(30.0, 2.0);
	const Durin::FVector3 SixtyHz = SimulateForward(60.0, 2.0);
	const Durin::FVector3 OneTwentyHz = SimulateForward(120.0, 2.0);
	EXPECT_NEAR(ThirtyHz.x, SixtyHz.x, 1.e-4);
	EXPECT_NEAR(SixtyHz.x, OneTwentyHz.x, 1.e-4);
	EXPECT_DOUBLE_EQ(ThirtyHz.y, 0.0);
	EXPECT_NEAR(ThirtyHz.z, 0.0, 0.01);
}

TEST(FSandboxGameplayMovementTests, ReferenceProductionAndComparePreserveTheMovementOutcome)
{
	const Durin::FVector3 Reference = SimulateForward(
		60.0, 2.0, Durin::EPhysicsSceneQueryExecutionPolicy::Reference);
	const Durin::FVector3 Production = SimulateForward(
		60.0, 2.0, Durin::EPhysicsSceneQueryExecutionPolicy::Production);
	const Durin::FVector3 Compare = SimulateForward(
		60.0, 2.0, Durin::EPhysicsSceneQueryExecutionPolicy::Compare);
	EXPECT_NEAR(Reference.x, Production.x, 1.0e-8);
	EXPECT_NEAR(Reference.y, Production.y, 1.0e-8);
	EXPECT_NEAR(Reference.z, Production.z, 1.0e-8);
	EXPECT_NEAR(Reference.x, Compare.x, 1.0e-8);
	EXPECT_NEAR(Reference.y, Compare.y, 1.0e-8);
	EXPECT_NEAR(Reference.z, Compare.z, 1.0e-8);
}

TEST(FSandboxGameplayCollisionTests, FallsWithoutCollisionAndRejectsAirborneJump)
{
	Durin::DWorld* World = CreateGameplayWorld(false);
	Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
	ASSERT_NE(Pawn, nullptr);
	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::Space, true);
	for (int Frame = 0; Frame < 30; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
		Durin::FGameInputStateTestAccess::FinishTick(Input);
	}
	EXPECT_LT(Pawn->GetActorTransform().Translation.z, -1.0);
	EXPECT_FALSE(Pawn->GetGroundMovementComponent()->IsGrounded());
	EXPECT_LT(Pawn->GetGroundMovementComponent()->GetVelocity().z, 0.0);
	DestroyWorld(World);
}

TEST(FSandboxGameplayCollisionTests, StopsAtWallsAndLandsOnRaisedPlatforms)
{
	Durin::DWorld* World = CreateGameplayWorld();
	AddCollisionBox(*World, "Wall", {3.0, 0.0, 1.5}, {0.25, 4.0, 1.5});
	Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	for (int Frame = 0; Frame < 120; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
		Durin::FGameInputStateTestAccess::FinishTick(Input);
	}
	EXPECT_LT(Pawn->GetActorTransform().Translation.x, 2.37);
	DestroyWorld(World);

	World = CreateGameplayWorld(false);
	AddCollisionBox(*World, "Platform", {0.0, 0.0, 1.0}, {3.0, 3.0, 0.5});
	Pawn = BeginGameplay(*World);
	Durin::FTransform Start = Pawn->GetActorTransform();
	Start.Translation.z = 4.0;
	ASSERT_TRUE(Pawn->SetActorTransform(Start));
	Durin::FGameInputState EmptyInput;
	Durin::FGameInputStateTestAccess::EnableAndFocus(EmptyInput);
	for (int Frame = 0; Frame < 120; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &EmptyInput});
		Durin::FGameInputStateTestAccess::FinishTick(EmptyInput);
	}
	EXPECT_NEAR(Pawn->GetActorTransform().Translation.z, 1.5, 0.02);
	EXPECT_TRUE(Pawn->GetGroundMovementComponent()->IsGrounded());
	DestroyWorld(World);
}

TEST(FSandboxGameplayCollisionTests, TraversesWalkableRotatedBoxRamp)
{
	Durin::DWorld* World = CreateGameplayWorld();
	AddCollisionBox(*World, "Ramp", {3.0, 0.0, 0.75}, {3.0, 2.0, 0.25},
		Durin::Math::MakeQuaternionFromAxisAngleDegrees(-12.0, Durin::FVectorConstants::Right));
	Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	for (int Frame = 0; Frame < 90; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
		Durin::FGameInputStateTestAccess::FinishTick(Input);
	}
	EXPECT_GT(Pawn->GetActorTransform().Translation.z, 0.2);
	DestroyWorld(World);
}

TEST(FSandboxGameplayCollisionTests, RejectsCeilingsAndStepsAcrossSupportedHeight)
{
	Durin::DWorld* World = CreateGameplayWorld();
	AddCollisionBox(*World, "Ceiling", {0.0, 0.0, 3.0}, {2.0, 2.0, 0.5});
	Durin::Sandbox::APlayerPawn* Pawn = BeginGameplay(*World);
	Durin::FGameInputState Input;
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::Space, true);
	double HighestZ = Pawn->GetActorTransform().Translation.z;
	for (int Frame = 0; Frame < 120; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
		Durin::FGameInputStateTestAccess::FinishTick(Input);
		HighestZ = std::max(HighestZ, Pawn->GetActorTransform().Translation.z);
	}
	EXPECT_LT(HighestZ, 0.51);
	EXPECT_TRUE(Pawn->GetGroundMovementComponent()->IsGrounded());
	DestroyWorld(World);

	World = CreateGameplayWorld();
	AddCollisionBox(*World, "Step", {2.0, 0.0, 0.2}, {0.5, 2.0, 0.2});
	Pawn = BeginGameplay(*World);
	Input = {};
	Durin::FGameInputStateTestAccess::EnableAndFocus(Input);
	Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	HighestZ = 0.0;
	for (int Frame = 0; Frame < 90; ++Frame)
	{
		World->Tick({.DeltaSeconds = 1.0f / 60.0f, .GameInput = &Input});
		Durin::FGameInputStateTestAccess::FinishTick(Input);
		HighestZ = std::max(HighestZ, Pawn->GetActorTransform().Translation.z);
	}
	EXPECT_GT(HighestZ, 0.25);
	EXPECT_GT(Pawn->GetActorTransform().Translation.x, 3.0);
	DestroyWorld(World);
}

TEST(DISABLED_FSandboxGameplayMeasurementBenchmarks, RecordsRealMovementQueryMixAndTiming)
{
	auto NoSetup = [](Durin::DWorld&) {};
	auto EmptyPrepare = [](Durin::DWorld&, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState&) {};
	auto ForwardPrepare = [](Durin::DWorld&, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
		Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::W, true);
	};

	MeasureSandboxCase(
		"grounded_forward", true, NoSetup, ForwardPrepare,
		[](Durin::DWorld& World, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			TickMeasurementFrames(World, Input, 120);
		},
		[](const FSandboxMeasurementResult& Result) {
			EXPECT_GT(Result.Location.x, 1.0);
			EXPECT_TRUE(Result.bGrounded);
		});

	MeasureSandboxCase(
		"wall_stop", true,
		[](Durin::DWorld& World) {
			AddCollisionBox(World, "Wall", {3.0, 0.0, 1.5}, {0.25, 4.0, 1.5});
		},
		ForwardPrepare,
		[](Durin::DWorld& World, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			TickMeasurementFrames(World, Input, 120);
		},
		[](const FSandboxMeasurementResult& Result) { EXPECT_LT(Result.Location.x, 2.37); });

	MeasureSandboxCase(
		"rotated_ramp", true,
		[](Durin::DWorld& World) {
			AddCollisionBox(World, "Ramp", {3.0, 0.0, 0.75}, {3.0, 2.0, 0.25},
				Durin::Math::MakeQuaternionFromAxisAngleDegrees(-12.0, Durin::FVectorConstants::Right));
		},
		ForwardPrepare,
		[](Durin::DWorld& World, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			TickMeasurementFrames(World, Input, 90);
		},
		[](const FSandboxMeasurementResult& Result) { EXPECT_GT(Result.Location.z, 0.2); });

	MeasureSandboxCase(
		"supported_step", true,
		[](Durin::DWorld& World) {
			AddCollisionBox(World, "Step", {2.0, 0.0, 0.2}, {0.5, 2.0, 0.2});
		},
		ForwardPrepare,
		[](Durin::DWorld& World, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			TickMeasurementFrames(World, Input, 90);
		},
		[](const FSandboxMeasurementResult& Result) { EXPECT_GT(Result.Location.x, 3.0); });

	MeasureSandboxCase(
		"jump_ceiling_land", true,
		[](Durin::DWorld& World) {
			AddCollisionBox(World, "Ceiling", {0.0, 0.0, 3.0}, {2.0, 2.0, 0.5});
		},
		[](Durin::DWorld&, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			Durin::FGameInputStateTestAccess::SetKey(Input, Durin::EKey::Space, true);
		},
		[](Durin::DWorld& World, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			TickMeasurementFrames(World, Input, 120);
		},
		[](const FSandboxMeasurementResult& Result) { EXPECT_TRUE(Result.bGrounded); });

	MeasureSandboxCase(
		"raised_platform_land", false,
		[](Durin::DWorld& World) {
			AddCollisionBox(World, "Platform", {0.0, 0.0, 1.0}, {3.0, 3.0, 0.5});
		},
		[](Durin::DWorld&, Durin::Sandbox::APlayerPawn& Pawn, Durin::FGameInputState&) {
			Durin::FTransform Start = Pawn.GetActorTransform();
			Start.Translation.z = 4.0;
			EXPECT_TRUE(Pawn.SetActorTransform(Start));
		},
		[](Durin::DWorld& World, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			TickMeasurementFrames(World, Input, 120);
		},
		[](const FSandboxMeasurementResult& Result) {
			EXPECT_NEAR(Result.Location.z, 1.5, 0.02);
			EXPECT_TRUE(Result.bGrounded);
		});

	MeasureSandboxCase(
		"empty_world_fall", false, NoSetup, EmptyPrepare,
		[](Durin::DWorld& World, Durin::Sandbox::APlayerPawn&, Durin::FGameInputState& Input) {
			TickMeasurementFrames(World, Input, 30);
		},
		[](const FSandboxMeasurementResult& Result) {
			EXPECT_LT(Result.Location.z, -1.0);
			EXPECT_FALSE(Result.bGrounded);
		});
}
