#include "Components/SceneComponent.h"

namespace Durin
{
	auto DSceneComponent::GetWorldLocation() const -> const FVector3&
	{
		return ComponentToWorld.Translation;
	}

	auto DSceneComponent::SetWorldLocation(const FVector3& InLocation) -> void
	{
		ComponentToWorld.Translation = InLocation;
	}

	auto DSceneComponent::GetWorldRotation() const -> const FQuat&
	{
		return ComponentToWorld.Rotation;
	}

	auto DSceneComponent::SetWorldRotation(const FQuat& InRotation) -> void
	{
		ComponentToWorld.Rotation = glm::normalize(InRotation);
	}

	auto DSceneComponent::GetWorldScale3D() const -> const FVector3&
	{
		return ComponentToWorld.Scale3D;
	}

	auto DSceneComponent::SetWorldScale3D(const FVector3& InScale) -> void
	{
		ComponentToWorld.Scale3D = InScale;
	}

	auto DSceneComponent::GetComponentToWorldMatrix() const -> FMatrix
	{
		return ComponentToWorld.ToMatrix();
	}
}
