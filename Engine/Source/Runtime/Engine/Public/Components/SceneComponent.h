#pragma once

#include "Components/ActorComponent.h"

#include "SceneComponent.gen.h"

namespace Durin
{
	DCLASS()
	class DSceneComponent : public DActorComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto GetWorldLocation() const -> const FVector3&;
		ENGINE_API auto SetWorldLocation(const FVector3& InLocation) -> void;

		ENGINE_API auto GetWorldRotation() const -> const FQuat&;
		ENGINE_API auto SetWorldRotation(const FQuat& InRotation) -> void;

		ENGINE_API auto GetWorldScale3D() const -> const FVector3&;
		ENGINE_API auto SetWorldScale3D(const FVector3& InScale) -> void;

		ENGINE_API auto GetComponentToWorldMatrix() const -> FMatrix;

	protected:
		FTransform ComponentToWorld;
	};
}
