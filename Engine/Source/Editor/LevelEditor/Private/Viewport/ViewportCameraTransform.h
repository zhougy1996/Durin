#pragma once

#include "Math/DurinMath.h"

namespace Durin::Editor::Level
{
	// Persists level-viewport camera position, orbit target, and Euler angles.
	struct FLevelViewportCameraState
	{
		FVector3 Location{-5.0, -5.0, 3.0};
		FVector3 OrbitPivot{0.0};
		FReal OrbitDistance = 8.0;
		FReal Pitch = -20.0;
		FReal Yaw = 45.0;
	};

	// Applies fly, pan, orbit, dolly, and focus operations to an editor camera.
	class FViewportCameraTransform
	{
	public:
		auto SetFromTransform(const FVector3& InLocation, const FQuat& InRotation) -> void;
		auto SetState(const FLevelViewportCameraState& State) -> void;
		auto Reset() -> void;
		auto GetState() const -> FLevelViewportCameraState { return {Location, OrbitPivot, OrbitDistance, Pitch, Yaw}; }
		auto Rotate(float DeltaYawDegrees, float DeltaPitchDegrees) -> void;
		auto MoveLocal(const FVector3& LocalDelta) -> void;
		auto Pan(float DeltaRight, float DeltaUp) -> void;
		auto Orbit(float DeltaYawDegrees, float DeltaPitchDegrees) -> void;
		auto Dolly(float Distance) -> void;
		auto Focus(const FVector3& Target, float Distance) -> void;

		auto GetLocation() const -> const FVector3& { return Location; }
		auto GetOrbitPivot() const -> const FVector3& { return OrbitPivot; }
		auto GetOrbitDistance() const -> FReal { return OrbitDistance; }
		auto GetPitch() const -> FReal { return Pitch; }
		auto GetYaw() const -> FReal { return Yaw; }
		auto GetForwardVector() const -> FVector3;
		auto GetRightVector() const -> FVector3;
		auto GetUpVector() const -> FVector3;
		auto GetViewMatrix() const -> FMatrix;

	private:
		FVector3 Location{-5.0f, -5.0f, 3.0f};
		FVector3 OrbitPivot{0.0f};
		FReal OrbitDistance = 8.0;
		FReal Pitch = -20.0;
		FReal Yaw = 45.0;
	};
}
