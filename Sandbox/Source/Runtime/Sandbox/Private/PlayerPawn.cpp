#include "PlayerPawn.h"

#include "Asset/Asset.h"
#include "Components/CameraComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "SandboxGameplayTuning.h"
#include "SimpleGroundMovementComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "PlayerPawnDefaults.h"
#include "Engine/ProjectGameSettings.h"
#include "Project/Project.h"

namespace Durin::Sandbox
{
	APlayerPawn::APlayerPawn(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer, /*bCreateDefaultRoot=*/false)
	{
		GroundMovementComponent = CreateDefaultComponent<DSimpleGroundMovementComponent>("GroundMovement");
		verify(SetMovementComponent(GroundMovementComponent.Get()));

		CapsuleComponent = CreateDefaultComponent<DCapsuleComponent>("CollisionCapsule");
		verify(SetRootComponent(CapsuleComponent.Get()));
		verify(CapsuleComponent->SetCapsuleSize(
			GameplayTuning::CapsuleRadius, GameplayTuning::CapsuleHalfHeight));
		FTransform CapsulePose;
		CapsulePose.Translation.z = GameplayTuning::CapsuleHalfHeight;
		verify(CapsuleComponent->SetCapsuleLocalPose(CapsulePose));
		verify(CapsuleComponent->SetCollisionProfileName(CollisionProfile::Pawn));

		VisualComponent = CreateDefaultComponent<DStaticMeshComponent>("GrayboxVisual");
		verify(VisualComponent->SetCollisionProfileName(CollisionProfile::NoCollision));
		verify(VisualComponent->SetupAttachment(CapsuleComponent.Get()));
		FTransform VisualTransform;
		VisualTransform.Translation = GameplayTuning::VisualOffset;
		VisualTransform.Scale3D = GameplayTuning::VisualScale;
		VisualComponent->SetRelativeTransform(VisualTransform);

		CameraComponent = CreateDefaultComponent<DCameraComponent>("GameplayCamera");
		verify(CameraComponent->SetupAttachment(CapsuleComponent.Get()));
		CameraComponent->SetRelativeLocation(GameplayTuning::CameraOffset);
	}

	auto APlayerPawn::BeginPlay() -> void
	{
        FProjectGameSettings Settings;
        const auto* Project = GetCurrentProject();
        const bool bConfigured = Project && FProjectGameSettingsStore::ForProject(*Project).Load(Settings);
        FSoftObjectPath DefaultData;
        auto* Defaults = bConfigured && FSoftObjectPath::TryCreate(Settings.DefaultData, DefaultData) && DefaultData.IsTopLevelAsset()
            ? LoadObject<DPlayerPawnDefaults>(DefaultData) : nullptr;
        if (Defaults && Defaults->Mesh)
            VisualComponent->SetStaticMesh(Defaults->Mesh.Get());
        else
            DURIN_WARN("Sandbox pawn defaults '{}' could not supply a mesh; the pawn remains playable without a visual.", Settings.DefaultData);

		Super::BeginPlay();
	}

	auto APlayerPawn::ConsumeLookIntent(const FVector2& Look) -> bool
	{
		if (Look.x == 0.0 && Look.y == 0.0) return false;
		const double PreviousYawDegrees = YawDegrees;
		const double PreviousPitchDegrees = PitchDegrees;
		YawDegrees += Look.x * GameplayTuning::LookDegreesPerIntent;
		PitchDegrees = std::clamp(
			PitchDegrees + Look.y * GameplayTuning::LookDegreesPerIntent,
			GameplayTuning::MinimumPitchDegrees,
			GameplayTuning::MaximumPitchDegrees);
		if (PitchDegrees != PreviousPitchDegrees)
		{
			CameraComponent->SetRelativeRotation(Math::MakeQuaternionFromAxisAngleDegrees(
				-PitchDegrees, FVectorConstants::Right));
		}
		return YawDegrees != PreviousYawDegrees;
	}
}
