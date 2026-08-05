#include "Viewport/ViewportCameraTransform.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		constexpr FReal kMaxPitch = 89.0;
		constexpr FReal kMinOrbitDistance = 0.05;
	}

	auto FViewportCameraTransform::SetFromTransform(const FVector3& InLocation, const FQuat& InRotation) -> void
	{
		Location = InLocation;
		const FVector3 Forward = Math::Normalize(InRotation * FVectorConstants::Forward);
		Yaw = Math::RadiansToDegrees(std::atan2(Forward.y, Forward.x));
		Pitch = Math::RadiansToDegrees(std::asin(std::clamp(Forward.z, -1.0, 1.0)));
		OrbitPivot = Location + Forward * OrbitDistance;
	}

	auto FViewportCameraTransform::SetState(const FLevelViewportCameraState& State) -> void
	{
		Location = State.Location;
		OrbitPivot = State.OrbitPivot;
		OrbitDistance = std::max(kMinOrbitDistance, State.OrbitDistance);
		Pitch = std::clamp(State.Pitch, -kMaxPitch, kMaxPitch);
		Yaw = State.Yaw;
	}

	auto FViewportCameraTransform::Reset() -> void
	{
		SetState(FLevelViewportCameraState{});
	}

	auto FViewportCameraTransform::Rotate(float DeltaYawDegrees, float DeltaPitchDegrees) -> void
	{
		Yaw += DeltaYawDegrees;
		Pitch = std::clamp(Pitch + DeltaPitchDegrees, -kMaxPitch, kMaxPitch);
		OrbitPivot = Location + GetForwardVector() * OrbitDistance;
	}

	auto FViewportCameraTransform::MoveLocal(const FVector3& LocalDelta) -> void
	{
		const FVector3 Delta = GetForwardVector() * LocalDelta.x + GetRightVector() * LocalDelta.y + FVectorConstants::Up * LocalDelta.z;
		Location += Delta;
		OrbitPivot += Delta;
	}

	auto FViewportCameraTransform::Pan(float DeltaRight, float DeltaUp) -> void
	{
		const FVector3 Delta = GetRightVector() * static_cast<FReal>(DeltaRight) + GetUpVector() * static_cast<FReal>(DeltaUp);
		Location += Delta;
		OrbitPivot += Delta;
	}

	auto FViewportCameraTransform::Orbit(float DeltaYawDegrees, float DeltaPitchDegrees) -> void
	{
		Yaw += DeltaYawDegrees;
		Pitch = std::clamp(Pitch + DeltaPitchDegrees, -kMaxPitch, kMaxPitch);
		Location = OrbitPivot - GetForwardVector() * OrbitDistance;
	}

	auto FViewportCameraTransform::Dolly(float Distance) -> void
	{
		const FReal NewDistance = std::max(kMinOrbitDistance, OrbitDistance - static_cast<FReal>(Distance));
		Location = OrbitPivot - GetForwardVector() * NewDistance;
		OrbitDistance = NewDistance;
	}

	auto FViewportCameraTransform::Focus(const FVector3& Target, float Distance) -> void
	{
		OrbitPivot = Target;
		OrbitDistance = std::max(kMinOrbitDistance, static_cast<FReal>(Distance));
		Location = OrbitPivot - GetForwardVector() * OrbitDistance;
	}

	auto FViewportCameraTransform::GetForwardVector() const -> FVector3
	{
		const FReal PitchRadians = Math::DegreesToRadians(Pitch);
		const FReal YawRadians = Math::DegreesToRadians(Yaw);
		return Math::Normalize(FVector3(std::cos(PitchRadians) * std::cos(YawRadians), std::cos(PitchRadians) * std::sin(YawRadians), std::sin(PitchRadians)));
	}

	auto FViewportCameraTransform::GetRightVector() const -> FVector3
	{
		return Math::Normalize(Math::Cross(FVectorConstants::Up, GetForwardVector()));
	}

	auto FViewportCameraTransform::GetUpVector() const -> FVector3
	{
		return Math::Normalize(Math::Cross(GetForwardVector(), GetRightVector()));
	}

	auto FViewportCameraTransform::GetViewMatrix() const -> FMatrix
	{
		const FVector3 Forward = GetForwardVector();
		const FVector3 Right = GetRightVector();
		const FVector3 Up = GetUpVector();
		FMatrix View(1.0f);
		View[0][0] = Forward.x; View[1][0] = Forward.y; View[2][0] = Forward.z; View[3][0] = -Math::Dot(Forward, Location);
		View[0][1] = Right.x; View[1][1] = Right.y; View[2][1] = Right.z; View[3][1] = -Math::Dot(Right, Location);
		View[0][2] = Up.x; View[1][2] = Up.y; View[2][2] = Up.z; View[3][2] = -Math::Dot(Up, Location);
		return View;
	}

}
