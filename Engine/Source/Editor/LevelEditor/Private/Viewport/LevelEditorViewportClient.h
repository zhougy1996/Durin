#pragma once

#include "Client/ViewportClient.h"
#include "Viewport/TransformGizmo.h"
#include "Viewport/ViewportCameraTransform.h"
#include "LevelEditorCustomizations.h"

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
		bool bCtrl = false;
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
		bool bRequestSelection = false;
		bool bCancel = false;
		bool bModeTranslate = false;
		bool bModeRotate = false;
		bool bModeScale = false;
		FVector2f MousePosition{0.0f};
		FVector2f ViewportSize{0.0f};
	};

	class FLevelEditorViewportClient final : public FViewportClient
	{
	public:
		auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override;
		auto Update(DLevel* Level, AActor* SelectedActor, const FLevelEditorViewportInput& Input) -> void;
		auto ResetNavigation() -> void;
		auto InitializeForLevel(DLevel* Level, const FLevelViewportCameraState* SavedState = nullptr) -> void;
		auto GetViewMatrix() const -> FMatrix { return CameraTransform.GetViewMatrix(); }
		auto GetCameraTransform() const -> const FViewportCameraTransform& { return CameraTransform; }
		auto GetCurrentLevel() const -> DLevel* { return CurrentLevel; }
		auto BuildPickingRay(const FVector2f& ViewportPosition, const FVector2f& ViewportSize, FVector3& OutOrigin, FVector3& OutDirection) const -> bool;
		auto PickActor(DLevel* Level, const FVector2f& ViewportPosition, const FVector2f& ViewportSize) const -> AActor*;
		auto UpdateHoveredVisualization(DLevel* Level, const FVector2f& ViewportPosition, const FVector2f& ViewportSize) -> void;
		auto ProjectWorldToViewport(const FVector3& WorldPosition, const FVector2f& ViewportSize, FVector2f& OutPosition) const -> bool;
		auto SetSelectedActors(const std::vector<TObjectPtr<AActor>>& Actors, AActor* PrimaryActor) -> void;
		auto GetTransformGizmo() -> FTransformGizmo& { return TransformGizmo; }
		auto GetTransformGizmo() const -> const FTransformGizmo& { return TransformGizmo; }

	private:
		auto AppendSelectionBounds(FSceneView& View) const -> void;
		auto CollectEditorVisualizations(DLevel* Level, const FSceneView& View, FEditorVisualizationCollector& Collector) const -> void;

		FViewportCameraTransform CameraTransform;
		DLevel* CurrentLevel = nullptr;
		std::vector<TObjectPtr<AActor>> SelectedActors;
		TObjectPtr<AActor> PrimarySelectedActor;
		TObjectPtr<AActor> HoveredVisualizationActor;
		float FieldOfViewDegrees = 60.0f;
		float NearClip = 0.1f;
		float FarClip = 10000.0f;
		float MovementSpeed = 5.0f;
		bool bFlyNavigation = false;
		bool bOrbitNavigation = false;
		bool bPanNavigation = false;
		mutable FTransformGizmo TransformGizmo;
	};
}
