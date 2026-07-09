#pragma once

#include "Components/SceneComponent.h"

#include "CameraComponent.gen.h"

namespace Durin
{
	DCLASS()
	class DCameraComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto GetFieldOfViewDegrees() const -> float;
		ENGINE_API auto SetFieldOfViewDegrees(float InFieldOfViewDegrees) -> void;

		ENGINE_API auto GetNearClip() const -> float;
		ENGINE_API auto SetNearClip(float InNearClip) -> void;

		ENGINE_API auto GetFarClip() const -> float;
		ENGINE_API auto SetFarClip(float InFarClip) -> void;

		ENGINE_API auto SetLookAt(const FVector3& InLocation, const FVector3& InTarget) -> void;
		ENGINE_API auto GetViewMatrix() const -> FMatrix;
		ENGINE_API auto GetProjectionMatrix(float AspectRatio) const -> FMatrix;

	private:
		float FieldOfViewDegrees = 60.0f;
		float NearClip = 0.1f;
		float FarClip = 1000.0f;
	};
}
