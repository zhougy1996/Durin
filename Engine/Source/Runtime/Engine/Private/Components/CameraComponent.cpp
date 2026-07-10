#include "Components/CameraComponent.h"

#include <glm/gtc/quaternion.hpp>

namespace Durin
{
	namespace
	{
		auto MakeCameraBasisRotation(const FVector3& Forward) -> FQuat
		{
			FVector3 UnitForward = glm::normalize(Forward);
			FVector3 UnitRight = glm::cross(FVectorConstants::Up, UnitForward);
			if (glm::dot(UnitRight, UnitRight) < kSmallNumber)
			{
				UnitRight = FVectorConstants::Right;
			}
			else
			{
				UnitRight = glm::normalize(UnitRight);
			}

			const FVector3 UnitUp = glm::normalize(glm::cross(UnitForward, UnitRight));
			FMatrix Basis(1.0);
			Basis[0] = FVector4(UnitForward, 0.0);
			Basis[1] = FVector4(UnitRight, 0.0);
			Basis[2] = FVector4(UnitUp, 0.0);
			return glm::normalize(glm::quat_cast(Basis));
		}

		auto GetForwardVector(const FQuat& Rotation) -> FVector3
		{
			return glm::normalize(Rotation * FVectorConstants::Forward);
		}

		auto GetRightVector(const FQuat& Rotation) -> FVector3
		{
			return glm::normalize(Rotation * FVectorConstants::Right);
		}

		auto GetUpVector(const FQuat& Rotation) -> FVector3
		{
			return glm::normalize(Rotation * FVectorConstants::Up);
		}
	}

	auto DCameraComponent::GetFieldOfViewDegrees() const -> float
	{
		return FieldOfViewDegrees;
	}

	auto DCameraComponent::SetFieldOfViewDegrees(float InFieldOfViewDegrees) -> void
	{
		FieldOfViewDegrees = std::clamp(InFieldOfViewDegrees, 1.0f, 170.0f);
		MarkPackageDirty();
	}

	auto DCameraComponent::GetNearClip() const -> float
	{
		return NearClip;
	}

	auto DCameraComponent::SetNearClip(float InNearClip) -> void
	{
		NearClip = std::max(InNearClip, 0.001f);
		if (FarClip <= NearClip)
		{
			FarClip = NearClip + 1.0f;
		}
		MarkPackageDirty();
	}

	auto DCameraComponent::GetFarClip() const -> float
	{
		return FarClip;
	}

	auto DCameraComponent::SetFarClip(float InFarClip) -> void
	{
		FarClip = std::max(InFarClip, NearClip + 1.0f);
		MarkPackageDirty();
	}

	auto DCameraComponent::SetLookAt(const FVector3& InLocation, const FVector3& InTarget) -> void
	{
		const FVector3 Forward = InTarget - InLocation;
		if (glm::dot(Forward, Forward) < kSmallNumber)
		{
			SetWorldLocation(InLocation);
			return;
		}

		SetWorldLocation(InLocation);
		SetWorldRotation(MakeCameraBasisRotation(Forward));
	}

	auto DCameraComponent::GetViewMatrix() const -> FMatrix
	{
		const FVector3 Eye = GetWorldLocation();
		const FVector3 Forward = GetForwardVector(GetWorldRotation());
		const FVector3 Right = GetRightVector(GetWorldRotation());
		const FVector3 Up = GetUpVector(GetWorldRotation());

		FMatrix View(1.0);
		View[0][0] = Forward.x;
		View[1][0] = Forward.y;
		View[2][0] = Forward.z;
		View[3][0] = -glm::dot(Forward, Eye);

		View[0][1] = Right.x;
		View[1][1] = Right.y;
		View[2][1] = Right.z;
		View[3][1] = -glm::dot(Right, Eye);

		View[0][2] = Up.x;
		View[1][2] = Up.y;
		View[2][2] = Up.z;
		View[3][2] = -glm::dot(Up, Eye);
		return View;
	}

	auto DCameraComponent::GetProjectionMatrix(float AspectRatio) const -> FMatrix
	{
		const float SafeAspectRatio = std::max(AspectRatio, 0.001f);
		const float HalfFovRadians = glm::radians(FieldOfViewDegrees) * 0.5f;
		const float YScale = 1.0f / std::tan(HalfFovRadians);
		const float XScale = YScale / SafeAspectRatio;
		const float DepthScale = FarClip / (FarClip - NearClip);
		const float DepthBias = -NearClip * FarClip / (FarClip - NearClip);

		FMatrix Projection(0.0);
		Projection[1][0] = XScale;
		Projection[2][1] = -YScale;
		Projection[0][2] = DepthScale;
		Projection[3][2] = DepthBias;
		Projection[0][3] = 1.0;
		return Projection;
	}
}
