#include "PlayerPawn.h"

#include "AssetLoad.h"
#include "Components/CameraComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "SandboxGameplayTuning.h"
#include "SimpleGroundMovementComponent.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Sandbox
{
	APlayerPawn::APlayerPawn(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		GroundMovementComponent = CreateDefaultComponent<DSimpleGroundMovementComponent>("GroundMovement");
		verify(SetMovementComponent(GroundMovementComponent.Get()));

		CapsuleComponent = CreateDefaultComponent<DCapsuleComponent>("CollisionCapsule");
		verify(CapsuleComponent->SetupAttachment(GetRootComponent()));
		verify(CapsuleComponent->SetCapsuleSize(
			GameplayTuning::CapsuleRadius, GameplayTuning::CapsuleHalfHeight));
		CapsuleComponent->SetRelativeLocation(FVector3(0.0, 0.0, GameplayTuning::CapsuleHalfHeight));
		verify(CapsuleComponent->SetCollisionProfileName(CollisionProfile::Pawn));

		VisualComponent = CreateDefaultComponent<DStaticMeshComponent>("GrayboxVisual");
		verify(VisualComponent->SetCollisionProfileName(CollisionProfile::NoCollision));
		verify(VisualComponent->SetupAttachment(GetRootComponent()));
		FTransform VisualTransform;
		VisualTransform.Translation = GameplayTuning::VisualOffset;
		VisualTransform.Scale3D = GameplayTuning::VisualScale;
		VisualComponent->SetRelativeTransform(VisualTransform);

		CameraComponent = CreateDefaultComponent<DCameraComponent>("GameplayCamera");
		verify(CameraComponent->SetupAttachment(GetRootComponent()));
		CameraComponent->SetRelativeLocation(GameplayTuning::CameraOffset);
	}

	auto APlayerPawn::BeginPlay() -> void
	{
		FAssetPath MeshPath;
		const bool bValidMeshPath = FAssetPath::TryCreate(GameplayTuning::GrayboxMeshPath, MeshPath);
		DStaticMesh* Mesh = nullptr;
		const Asset::FAssetResult LoadResult = bValidMeshPath
			? Asset::LoadAsset(MeshPath, Mesh)
			: Asset::FAssetResult{Asset::EAssetError::InvalidPath, "The configured graybox mesh path is invalid."};
		if (LoadResult && Mesh)
		{
			VisualComponent->SetStaticMesh(Mesh);
		}
		else
		{
			DURIN_WARN("Sandbox pawn graybox '{}' could not load: {} The pawn remains playable without a visual.",
				GameplayTuning::GrayboxMeshPath, LoadResult.Message);
		}
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
