#include "Components/CameraComponent.h"

#include "DObject/Property.h"
#include "Math/Operations.h"
#include "SceneViewProjection.h"

namespace Durin
{
	namespace
	{
		auto MakeCameraBasisRotation(const FVector3& Forward) -> FQuat
		{
			FVector3 UnitForward = Math::Normalize(Forward);
			FVector3 UnitRight = Math::Cross(FVectorConstants::Up, UnitForward);
			if (Math::LengthSquared(UnitRight) < kSmallNumber)
			{
				UnitRight = FVectorConstants::Right;
			}
			else
			{
				UnitRight = Math::Normalize(UnitRight);
			}

			const FVector3 UnitUp = Math::Normalize(Math::Cross(UnitForward, UnitRight));
			FMatrix Basis(1.0);
			Basis[0] = FVector4(UnitForward, 0.0);
			Basis[1] = FVector4(UnitRight, 0.0);
			Basis[2] = FVector4(UnitUp, 0.0);
			return Math::Normalize(Math::QuaternionFromMatrix(Basis));
		}

		auto GetForwardVector(const FQuat& Rotation) -> FVector3
		{
			return Math::Normalize(Math::RotateVector(Rotation, FVectorConstants::Forward));
		}

		auto GetRightVector(const FQuat& Rotation) -> FVector3
		{
			return Math::Normalize(Math::RotateVector(Rotation, FVectorConstants::Right));
		}

		auto GetUpVector(const FQuat& Rotation) -> FVector3
		{
			return Math::Normalize(Math::RotateVector(Rotation, FVectorConstants::Up));
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
		ProjectionSettings.FieldOfViewDegrees = static_cast<float>(
			SceneViewProjection::ClampFieldOfViewDegrees(InFieldOfViewDegrees));
		double NearClip = 0.0;
		double FarClip = 0.0;
		SceneViewProjection::ClampPerspectiveClipRange(InNearClip, InFarClip, NearClip, FarClip);
		ProjectionSettings.NearClip = static_cast<float>(NearClip);
		ProjectionSettings.FarClip = static_cast<float>(FarClip);
		SetTerrainDistance(ProjectionSettings.TerrainFadeStart,
			ProjectionSettings.TerrainRenderDistance);
		MarkPackageDirty();
	}

	auto DCameraComponent::SetTerrainDistance(float InFadeStart,
		float InRenderDistance) -> void
	{
		double FadeStart = 0.0;
		double RenderDistance = 0.0;
		SceneViewProjection::ClampTerrainDistances(ProjectionSettings.FarClip,
			InFadeStart, InRenderDistance, FadeStart, RenderDistance);
		ProjectionSettings.TerrainFadeStart = static_cast<float>(FadeStart);
		ProjectionSettings.TerrainRenderDistance = static_cast<float>(RenderDistance);
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
		SetTerrainDistance(InSettings.TerrainFadeStart,
			InSettings.TerrainRenderDistance);
		SetAspectRatio(InSettings.AspectRatioMode, InSettings.CustomAspectRatio);
	}

	auto DCameraComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.MemberProperty->NamePrivate != FName("ProjectionSettings")
			|| Proposal.DraftRootProperty != Proposal.MemberProperty || !Proposal.DraftRootContainer) return true;
		auto* Settings = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FCameraProjectionSettings>(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		Settings->FieldOfViewDegrees = static_cast<float>(
			SceneViewProjection::ClampFieldOfViewDegrees(Settings->FieldOfViewDegrees));
		double NearClip = 0.0;
		double FarClip = 0.0;
		SceneViewProjection::ClampPerspectiveClipRange(
			Settings->NearClip, Settings->FarClip, NearClip, FarClip);
		Settings->NearClip = static_cast<float>(NearClip);
		Settings->FarClip = static_cast<float>(FarClip);
		double FadeStart = 0.0;
		double RenderDistance = 0.0;
		SceneViewProjection::ClampTerrainDistances(Settings->FarClip,
			Settings->TerrainFadeStart, Settings->TerrainRenderDistance,
			FadeStart, RenderDistance);
		Settings->TerrainFadeStart = static_cast<float>(FadeStart);
		Settings->TerrainRenderDistance = static_cast<float>(RenderDistance);
		Settings->CustomAspectRatio = std::clamp(Settings->CustomAspectRatio, 0.1f, 10.0f);
		return true;
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
		if (Math::LengthSquared(Forward) < kSmallNumber)
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
		View[3][0] = -Math::Dot(Forward, Eye);

		View[0][1] = Right.x;
		View[1][1] = Right.y;
		View[2][1] = Right.z;
		View[3][1] = -Math::Dot(Right, Eye);

		View[0][2] = Up.x;
		View[1][2] = Up.y;
		View[2][2] = Up.z;
		View[3][2] = -Math::Dot(Up, Eye);
		return View;
	}

	auto DCameraComponent::GetProjectionMatrix(float AspectRatio) const -> FMatrix
	{
		FMatrix Projection;
		const bool bValid = SceneViewProjection::BuildPerspectiveProjection(
			ProjectionSettings.FieldOfViewDegrees, AspectRatio,
			ProjectionSettings.NearClip, ProjectionSettings.FarClip,
			ESceneDepthConvention::ReversedZ, Projection);
		check(bValid);
		return Projection;
	}
}
