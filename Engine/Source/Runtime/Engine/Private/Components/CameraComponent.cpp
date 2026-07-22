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
		return ProjectionSettings.FieldOfViewDegrees;
	}

	auto DCameraComponent::SetFieldOfViewDegrees(float InFieldOfViewDegrees) -> void
	{
		SetProjectionParameters(InFieldOfViewDegrees, ProjectionSettings.NearClip, ProjectionSettings.FarClip);
	}

	auto DCameraComponent::GetNearClip() const -> float
	{
		return ProjectionSettings.NearClip;
	}

	auto DCameraComponent::SetNearClip(float InNearClip) -> void
	{
		SetProjectionParameters(ProjectionSettings.FieldOfViewDegrees, InNearClip, ProjectionSettings.FarClip);
	}

	auto DCameraComponent::GetFarClip() const -> float
	{
		return ProjectionSettings.FarClip;
	}

	auto DCameraComponent::SetFarClip(float InFarClip) -> void
	{
		SetProjectionParameters(ProjectionSettings.FieldOfViewDegrees, ProjectionSettings.NearClip, InFarClip);
	}

	auto DCameraComponent::SetProjectionParameters(float InFieldOfViewDegrees, float InNearClip, float InFarClip) -> void
	{
		ProjectionSettings.FieldOfViewDegrees = std::clamp(InFieldOfViewDegrees, 1.0f, 170.0f);
		ProjectionSettings.NearClip = std::max(InNearClip, 0.001f);
		ProjectionSettings.FarClip = std::max(InFarClip, ProjectionSettings.NearClip + 1.0f);
		MarkPackageDirty();
	}

	auto DCameraComponent::GetAspectRatioMode() const -> ECameraAspectRatioMode
	{
		return ProjectionSettings.AspectRatioMode;
	}

	auto DCameraComponent::GetCustomAspectRatio() const -> float
	{
		return ProjectionSettings.CustomAspectRatio;
	}

	auto DCameraComponent::SetAspectRatio(ECameraAspectRatioMode InMode, float InCustomAspectRatio) -> void
	{
		ProjectionSettings.AspectRatioMode = InMode;
		ProjectionSettings.CustomAspectRatio = std::clamp(InCustomAspectRatio, 0.1f, 10.0f);
		MarkPackageDirty();
	}

	auto DCameraComponent::GetProjectionSettings() const -> const FCameraProjectionSettings&
	{
		return ProjectionSettings;
	}

	auto DCameraComponent::SetProjectionSettings(const FCameraProjectionSettings& InSettings) -> void
	{
		SetProjectionParameters(InSettings.FieldOfViewDegrees, InSettings.NearClip, InSettings.FarClip);
		SetAspectRatio(InSettings.AspectRatioMode, InSettings.CustomAspectRatio);
	}

	auto DCameraComponent::ResolveAspectRatio(float ViewportAspectRatio) const -> float
	{
		switch (ProjectionSettings.AspectRatioMode)
		{
		case ECameraAspectRatioMode::Ratio16By9: return 16.0f / 9.0f;
		case ECameraAspectRatioMode::Ratio16By10: return 16.0f / 10.0f;
		case ECameraAspectRatioMode::Ratio4By3: return 4.0f / 3.0f;
		case ECameraAspectRatioMode::Ratio1By1: return 1.0f;
		case ECameraAspectRatioMode::Custom: return std::clamp(ProjectionSettings.CustomAspectRatio, 0.1f, 10.0f);
		case ECameraAspectRatioMode::Viewport:
		default: return std::max(ViewportAspectRatio, 0.001f);
		}
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
		const float HalfFovRadians = glm::radians(ProjectionSettings.FieldOfViewDegrees) * 0.5f;
		const float YScale = 1.0f / std::tan(HalfFovRadians);
		const float XScale = YScale / SafeAspectRatio;
		const float DepthScale = ProjectionSettings.FarClip / (ProjectionSettings.FarClip - ProjectionSettings.NearClip);
		const float DepthBias = -ProjectionSettings.NearClip * ProjectionSettings.FarClip / (ProjectionSettings.FarClip - ProjectionSettings.NearClip);

		FMatrix Projection(0.0);
		Projection[1][0] = XScale;
		Projection[2][1] = -YScale;
		Projection[0][2] = DepthScale;
		Projection[3][2] = DepthBias;
		Projection[0][3] = 1.0;
		return Projection;
	}
}
