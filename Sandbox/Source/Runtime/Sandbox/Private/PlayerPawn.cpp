#include "PlayerPawn.h"

#include "AssetSystem.h"
#include "Components/CameraComponent.h"
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

		VisualComponent = CreateDefaultComponent<DStaticMeshComponent>("GrayboxVisual");
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

	auto APlayerPawn::ApplyLookIntent(const FVector2& Look) -> void
	{
		YawDegrees += Look.x * GameplayTuning::LookDegreesPerIntent;
		PitchDegrees = std::clamp(
			PitchDegrees + Look.y * GameplayTuning::LookDegreesPerIntent,
			GameplayTuning::MinimumPitchDegrees,
			GameplayTuning::MaximumPitchDegrees);
		GetRootComponent()->SetRelativeRotation(Math::MakeQuaternionFromAxisAngleDegrees(
			YawDegrees, FVectorConstants::Up));
		CameraComponent->SetRelativeRotation(Math::MakeQuaternionFromAxisAngleDegrees(
			-PitchDegrees, FVectorConstants::Right));
	}
}
