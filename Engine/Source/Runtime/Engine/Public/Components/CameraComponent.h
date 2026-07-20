#pragma once

#include "Components/SceneComponent.h"

#include "CameraComponent.gen.h"

namespace Durin
{
	DENUM()
	enum class ECameraAspectRatioMode : uint8
	{
		Viewport,
		Ratio16By9,
		Ratio16By10,
		Ratio4By3,
		Ratio1By1,
		Custom
	};

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
		ENGINE_API auto SetProjectionParameters(float InFieldOfViewDegrees, float InNearClip, float InFarClip) -> void;

		ENGINE_API auto GetAspectRatioMode() const -> ECameraAspectRatioMode;
		ENGINE_API auto GetCustomAspectRatio() const -> float;
		ENGINE_API auto SetAspectRatio(ECameraAspectRatioMode InMode, float InCustomAspectRatio) -> void;
		ENGINE_API auto ResolveAspectRatio(float ViewportAspectRatio) const -> float;

		ENGINE_API auto SetLookAt(const FVector3& InLocation, const FVector3& InTarget) -> void;
		ENGINE_API auto GetViewMatrix() const -> FMatrix;
		ENGINE_API auto GetProjectionMatrix(float AspectRatio) const -> FMatrix;

private:
		DPROPERTY(Edit)
		float FieldOfViewDegrees = 60.0f;
		DPROPERTY(Edit)
		float NearClip = 0.1f;
		DPROPERTY(Edit)
		float FarClip = 1000.0f;
		DPROPERTY(Edit)
		// Preserve the historical viewport-driven framing unless a camera explicitly opts into a fixed output shape.
		ECameraAspectRatioMode AspectRatioMode = ECameraAspectRatioMode::Viewport;
		DPROPERTY(Edit)
		float CustomAspectRatio = 16.0f / 9.0f;
	};
}
