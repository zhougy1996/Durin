#pragma once

#include "Actors/Pawn.h"
#include "SandboxAPI.h"

#include "PlayerPawn.gen.h"

namespace Durin
{
	class DCameraComponent;
	class DStaticMeshComponent;
}

namespace Durin::Sandbox
{
	class DSimpleGroundMovementComponent;

	// Owns the visible Sandbox graybox, its ground movement authority, and its gameplay camera.
	DCLASS()
	class APlayerPawn final : public APawn
	{
		GENERATED_BODY()
	public:
		SANDBOX_API explicit APlayerPawn(const FObjectInitializer& ObjectInitializer);
		SANDBOX_API auto BeginPlay() -> void override;

		auto GetGroundMovementComponent() const -> DSimpleGroundMovementComponent* { return GroundMovementComponent.Get(); }
		auto GetVisualComponent() const -> DStaticMeshComponent* { return VisualComponent.Get(); }
		auto GetCameraComponent() const -> DCameraComponent* { return CameraComponent.Get(); }
		auto GetYawDegrees() const -> double { return YawDegrees; }
		auto GetPitchDegrees() const -> double { return PitchDegrees; }

		// Applies one already-bounded semantic look sample to pawn yaw and camera pitch.
		SANDBOX_API auto ApplyLookIntent(const FVector2& Look) -> void;

	private:
		DPROPERTY()
		TObjectPtr<DSimpleGroundMovementComponent> GroundMovementComponent;

		DPROPERTY()
		TObjectPtr<DStaticMeshComponent> VisualComponent;

		DPROPERTY()
		TObjectPtr<DCameraComponent> CameraComponent;

		double YawDegrees = 0.0;
		double PitchDegrees = 0.0;
	};
}
