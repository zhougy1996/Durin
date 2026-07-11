#pragma once

#include "Client/ViewportClient.h"
#include "Viewport/ViewportCameraTransform.h"

namespace Durin
{
	class AActor;
	class DLevel;

	struct FLevelEditorViewportInput
	{
		float DeltaSeconds = 0.0f;
		FVector2f MouseDelta{0.0f};
		float MouseWheel = 0.0f;
		bool bHovered = false;
		bool bFocused = false;
		bool bWantTextInput = false;
		bool bAlt = false;
		bool bShift = false;
		bool bLeftMouseDown = false;
		bool bMiddleMouseDown = false;
		bool bRightMouseDown = false;
		bool bLeftMousePressed = false;
		bool bMiddleMousePressed = false;
		bool bRightMousePressed = false;
		bool bMoveForward = false;
		bool bMoveBackward = false;
		bool bMoveLeft = false;
		bool bMoveRight = false;
		bool bMoveDown = false;
		bool bMoveUp = false;
		bool bFocusSelection = false;
	};

	class FLevelEditorViewportClient final : public FViewportClient
	{
	public:
		auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override;
		auto Update(DLevel* Level, AActor* SelectedActor, const FLevelEditorViewportInput& Input) -> void;
		auto ResetNavigation() -> void;
		auto GetViewMatrix() const -> FMatrix { return CameraTransform.GetViewMatrix(); }
		auto GetCameraTransform() const -> const FViewportCameraTransform& { return CameraTransform; }

	private:
		auto InitializeForLevel(DLevel* Level) -> void;

		FViewportCameraTransform CameraTransform;
		DLevel* CurrentLevel = nullptr;
		float FieldOfViewDegrees = 60.0f;
		float NearClip = 0.1f;
		float FarClip = 10000.0f;
		float MovementSpeed = 5.0f;
		bool bFlyNavigation = false;
		bool bOrbitNavigation = false;
		bool bPanNavigation = false;
	};
}
