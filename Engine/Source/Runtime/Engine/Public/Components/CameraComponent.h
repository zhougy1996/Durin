#pragma once

#include "Components/SceneComponent.h"

#include "CameraComponent.gen.h"

namespace Durin
{
	// Selects viewport-derived or fixed camera projection framing.
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

	// Defines the perspective projection inputs shared by runtime and editor cameras.
	DSTRUCT()
	struct FCameraProjectionSettings
	{
		GENERATED_BODY()

		// Vertical field of view in degrees.
		DPROPERTY(Edit)
		float FieldOfViewDegrees = 60.0f;

		DPROPERTY(Edit)
		float NearClip = 0.1f;

		DPROPERTY(Edit)
		float FarClip = 500000.0f;

		// Preserve the historical viewport-driven framing unless a camera explicitly opts into a fixed output shape.
		DPROPERTY(Edit)
		ECameraAspectRatioMode AspectRatioMode = ECameraAspectRatioMode::Viewport;

		// Used only when AspectRatioMode is Custom.
		DPROPERTY(Edit)
		float CustomAspectRatio = 16.0f / 9.0f;

		auto operator==(const FCameraProjectionSettings&) const -> bool = default;
	};

	// View-level distance policy: where distance-based fade begins and geometry is culled.
	// Kept separate from projection because it is a view policy, not a camera intrinsic.
	DSTRUCT()
	struct FViewDistanceSettings
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		float FadeStart = 180000.0f;

		DPROPERTY(Edit)
		float RenderDistance = 200000.0f;

		auto operator==(const FViewDistanceSettings&) const -> bool = default;
	};

	// Provides a scene transform plus validated perspective projection and view matrices.
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

		ENGINE_API auto GetViewDistance() const -> const FViewDistanceSettings&;
		ENGINE_API auto SetViewDistance(const FViewDistanceSettings& InSettings) -> void;
		ENGINE_API auto SetViewDistance(float InFadeStart, float InRenderDistance) -> void;
		ENGINE_API auto GetViewFadeStart() const -> float;
		ENGINE_API auto GetViewRenderDistance() const -> float;

		ENGINE_API auto GetAspectRatioMode() const -> ECameraAspectRatioMode;
		ENGINE_API auto GetCustomAspectRatio() const -> float;
		ENGINE_API auto SetAspectRatio(ECameraAspectRatioMode InMode, float InCustomAspectRatio) -> void;
		ENGINE_API auto GetProjectionSettings() const -> const FCameraProjectionSettings&;
		ENGINE_API auto SetProjectionSettings(const FCameraProjectionSettings& InSettings) -> void;
		ENGINE_API auto ResolveAspectRatio(float ViewportAspectRatio) const -> float;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;

		ENGINE_API auto SetLookAt(const FVector3& InLocation, const FVector3& InTarget) -> void;
		ENGINE_API auto GetViewMatrix() const -> FMatrix;
		ENGINE_API auto GetProjectionMatrix(float AspectRatio) const -> FMatrix;

	private:
		auto CommitSettings(
			FCameraProjectionSettings InProjectionSettings,
			FViewDistanceSettings InViewDistance) -> void;

		DPROPERTY(Edit)
		FCameraProjectionSettings ProjectionSettings;

		DPROPERTY(Edit)
		FViewDistanceSettings ViewDistance;
	};
}
