#include "Viewport/LevelEditorViewportClient.h"

#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "IRendererModule.h"

namespace Durin
{
	namespace
	{
		constexpr float kLookSensitivity = 0.15f;
		constexpr float kOrbitSensitivity = 0.2f;
		constexpr float kPanSensitivity = 0.01f;
		constexpr float kSpeedWheelScale = 1.2f;
		constexpr float kMinMovementSpeed = 0.05f;
		constexpr float kMaxMovementSpeed = 10000.0f;
		constexpr float kShiftSpeedMultiplier = 4.0f;
		constexpr float kFocusDistance = 5.0f;
	}

	auto FLevelEditorViewportClient::CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		const float AspectRatio = Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f;
		const float HalfFovRadians = glm::radians(FieldOfViewDegrees) * 0.5f;
		const float YScale = 1.0f / std::tan(HalfFovRadians);
		const float XScale = YScale / std::max(AspectRatio, 0.001f);
		const float DepthScale = FarClip / (FarClip - NearClip);
		const float DepthBias = -NearClip * FarClip / (FarClip - NearClip);

		OutView.ViewportWidth = Width;
		OutView.ViewportHeight = Height;
		OutView.ViewMatrix = CameraTransform.GetViewMatrix();
		OutView.ProjectionMatrix = FMatrix(0.0f);
		OutView.ProjectionMatrix[1][0] = XScale;
		OutView.ProjectionMatrix[2][1] = -YScale;
		OutView.ProjectionMatrix[0][2] = DepthScale;
		OutView.ProjectionMatrix[3][2] = DepthBias;
		OutView.ProjectionMatrix[0][3] = 1.0f;
		OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
		OutView.ViewLocation = CameraTransform.GetLocation();
		return true;
	}

	auto FLevelEditorViewportClient::Update(DLevel* Level, AActor* SelectedActor, const FLevelEditorViewportInput& Input) -> void
	{
		if (Level != CurrentLevel) InitializeForLevel(Level);
		if (!Input.bFocused)
		{
			ResetNavigation();
			return;
		}

		if (Input.bRightMousePressed && Input.bHovered) bFlyNavigation = true;
		if (Input.bLeftMousePressed && Input.bHovered && Input.bAlt) bOrbitNavigation = true;
		if (Input.bMiddleMousePressed && Input.bHovered) bPanNavigation = true;
		if (!Input.bRightMouseDown) bFlyNavigation = false;
		if (!Input.bLeftMouseDown || !Input.bAlt) bOrbitNavigation = false;
		if (!Input.bMiddleMouseDown) bPanNavigation = false;

		if (bFlyNavigation)
		{
			CameraTransform.Rotate(Input.MouseDelta.x * kLookSensitivity, -Input.MouseDelta.y * kLookSensitivity);
			if (Input.MouseWheel != 0.0f)
			{
				MovementSpeed = std::clamp(MovementSpeed * std::pow(kSpeedWheelScale, Input.MouseWheel), kMinMovementSpeed, kMaxMovementSpeed);
			}
			if (!Input.bWantTextInput)
			{
				FVector3 Direction(0.0f);
				Direction.x = static_cast<float>(Input.bMoveForward) - static_cast<float>(Input.bMoveBackward);
				Direction.y = static_cast<float>(Input.bMoveRight) - static_cast<float>(Input.bMoveLeft);
				Direction.z = static_cast<float>(Input.bMoveUp) - static_cast<float>(Input.bMoveDown);
				if (glm::dot(Direction, Direction) > 0.0f)
				{
					Direction = glm::normalize(Direction);
					const float Speed = MovementSpeed * (Input.bShift ? kShiftSpeedMultiplier : 1.0f);
					CameraTransform.MoveLocal(Direction * static_cast<FReal>(Speed * Input.DeltaSeconds));
				}
			}
		}
		else if (bOrbitNavigation)
		{
			CameraTransform.Orbit(Input.MouseDelta.x * kOrbitSensitivity, -Input.MouseDelta.y * kOrbitSensitivity);
		}
		else if (bPanNavigation)
		{
			const float Scale = static_cast<float>(std::max(1.0, CameraTransform.GetOrbitDistance())) * kPanSensitivity;
			CameraTransform.Pan(-Input.MouseDelta.x * Scale, Input.MouseDelta.y * Scale);
		}
		else if (Input.bHovered && Input.MouseWheel != 0.0f)
		{
			CameraTransform.Dolly(Input.MouseWheel * static_cast<float>(std::max(0.25, CameraTransform.GetOrbitDistance() * 0.15)));
		}

		if (Input.bHovered && !Input.bWantTextInput && Input.bFocusSelection && SelectedActor != nullptr)
		{
			if (const DSceneComponent* RootComponent = SelectedActor->GetRootComponent())
			{
				CameraTransform.Focus(RootComponent->GetWorldLocation(), kFocusDistance);
			}
		}
	}

	auto FLevelEditorViewportClient::ResetNavigation() -> void
	{
		bFlyNavigation = false;
		bOrbitNavigation = false;
		bPanNavigation = false;
	}

	auto FLevelEditorViewportClient::InitializeForLevel(DLevel* Level) -> void
	{
		CurrentLevel = Level;
		ResetNavigation();
		if (Level != nullptr)
		{
			if (const ACameraActor* CameraActor = Level->GetPrimaryCameraActor())
			{
				if (const DCameraComponent* Camera = CameraActor->GetCameraComponent())
				{
					CameraTransform.SetFromTransform(Camera->GetWorldLocation(), Camera->GetWorldRotation());
				}
			}
		}
	}
}
