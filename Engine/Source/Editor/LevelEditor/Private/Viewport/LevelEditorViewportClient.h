#pragma once

#include "Client/ViewportClient.h"
#include "Viewport/TransformGizmo.h"
#include "Viewport/ViewportCameraTransform.h"
#include "LevelEditorCustomizations.h"
#include "LevelEditorViewportPicking.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DLevel;
}

namespace Durin::Editor::Level
{
	class IViewportPickingBackend;
	class FViewportPickingService;
	class FViewportPickingSceneIndex;

	// Captures one frame of normalized viewport navigation and gizmo input.
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
		bool bLeftMouseDoubleClicked = false;
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
		bool bDelete = false;
		bool bDuplicate = false;
		bool bAppend = false;
		FVector2f MousePosition{0.0f};
		FVector2f ViewportSize{0.0f};
	};

	// Builds the editor scene view and coordinates navigation, picking, and gizmos.
	class FLevelEditorViewportClient final : public FViewportClient
	{
	public:
		static constexpr float DefaultNearClip = 0.1f;
		static constexpr float DefaultFarClip = 500000.0f;
		static constexpr float DefaultTerrainFadeStart = 180000.0f;
		static constexpr float DefaultTerrainRenderDistance = 200000.0f;

		FLevelEditorViewportClient();
		~FLevelEditorViewportClient() override;
		auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override;
		// Builds projection state without traversing the level or appending editor overlays.
		auto BuildViewMatrices(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool;
		// Captures the final editor view for reuse by rendering in the current logic frame.
		auto PrepareSceneView(DLevel* Level, uint32 Width, uint32 Height) -> void;
		// Matches the renderer's minimum-size and ceiling policy for logical widget extents.
		static auto ResolveViewportExtent(const FVector2f& ViewportSize, uint32& OutWidth, uint32& OutHeight) -> bool;
		auto Update(DLevel* Level, AActor* SelectedActor, const FLevelEditorViewportInput& Input) -> void;
		auto ResetNavigation() -> void;
		auto InitializeForLevel(DLevel* Level, const FLevelViewportCameraState* SavedState = nullptr) -> void;
		auto GetViewMatrix() const -> FMatrix { return CameraTransform.GetViewMatrix(); }
		auto GetCameraTransform() const -> const FViewportCameraTransform& { return CameraTransform; }
		auto GetMovementSpeed() const -> float { return MovementSpeed; }
		auto SetMovementSpeed(float Speed) -> void;
		auto SetCameraLocation(const FVector3& WorldLocation) -> void;
		auto MoveCameraLocal(const FVector3& LocalDelta) -> void;
		auto IsFlyNavigating() const -> bool { return bFlyNavigation; }
		auto GetCurrentLevel() const -> DLevel* { return CurrentLevel; }
		auto BuildPickingRay(const FVector2f& ViewportPosition, const FVector2f& ViewportSize, FVector3& OutOrigin, FVector3& OutDirection) const -> bool;
		auto SubmitViewportPick(DLevel* Level, const FSceneView& View, const FVector2f& ViewportPosition,
			EViewportPickLayer Layers = EViewportPickLayer::SceneGeometry | EViewportPickLayer::EditorVisualization) -> FViewportPickSubmission;
		auto PollViewportPick(FViewportPickTicket Ticket) -> FViewportPickCompletion;
		auto CancelViewportPick(FViewportPickTicket Ticket) -> void;
		auto ReleaseViewportPick(FViewportPickTicket Ticket) -> void;
		auto SetPickingBackendForTesting(std::unique_ptr<IViewportPickingBackend> Backend) -> void;
		auto SetPickingSceneIndex(std::shared_ptr<FViewportPickingSceneIndex> SceneIndex) -> void;
		auto UpdateHoveredVisualization(DLevel* Level, const FVector2f& ViewportPosition, const FVector2f& ViewportSize) -> void;
		auto UpdateHoveredVisualizationWithView(DLevel* Level, const FSceneView& View, const FVector2f& ViewportPosition) -> void;
		auto ProjectWorldToViewport(const FVector3& WorldPosition, const FVector2f& ViewportSize, FVector2f& OutPosition) const -> bool;
		auto SetSelectedActors(const std::vector<TObjectPtr<AActor>>& Actors, AActor* PrimaryActor) -> void;
		auto SetSelectedComponent(DActorComponent* Component, const std::vector<FEditorSubElementSelection>& Elements) -> void;
		auto FocusActor(const AActor* Actor) -> void;
		auto FocusLocation(const FVector3& WorldLocation) -> void;
		auto GetTransformGizmo() -> FTransformGizmo& { return TransformGizmo; }
		auto GetTransformGizmo() const -> const FTransformGizmo& { return TransformGizmo; }
		auto IsGridVisible() const -> bool { return bShowGrid; }
		auto SetGridVisible(bool bVisible) -> void;
		auto GetNearClip() const -> float { return NearClip; }
		auto GetFarClip() const -> float { return FarClip; }
		auto GetTerrainFadeStart() const -> float { return TerrainFadeStart; }
		auto GetTerrainRenderDistance() const -> float { return TerrainRenderDistance; }
		auto SetClipDistances(float InNearClip, float InFarClip) -> void;
		auto SetTerrainDistance(float InFadeStart, float InRenderDistance) -> void;
		auto ResetViewDistances() -> void;

	private:
		// Retains the last rendered editor view and its richer hit-test data across logic frames.
		struct FPreparedSceneView
		{
			FSceneView View;
			FEditorVisualizationCollector Visualizations;
			TWeakObjectPtr<DLevel> Level;
			uint64 FrameNumber = 0;
			uint32 Width = 0;
			uint32 Height = 0;
			bool bReadyForRender = false;
		};

		auto BuildCompleteSceneView(DLevel* Level, uint32 Width, uint32 Height, FPreparedSceneView& OutFrame) const -> bool;
		auto PopulateEditorOverlays(DLevel* Level, const FSceneView& View, FEditorVisualizationCollector& Collector) const -> void;
		auto AppendSelectionBounds(FSceneView& View) const -> void;
		auto ResetFlyMotion() -> void;
		auto InvalidatePreparedSceneView(bool bDiscardInteractionData = false) -> void;

		FViewportCameraTransform CameraTransform;
		DLevel* CurrentLevel = nullptr;
		std::vector<TObjectPtr<AActor>> SelectedActors;
		TObjectPtr<AActor> PrimarySelectedActor;
		TWeakObjectPtr<DActorComponent> SelectedComponent;
		std::vector<FEditorSubElementSelection> SelectedSubElements;
		FEditorVisualizationHit HoveredVisualization;
		// Perspective values are stored in degrees and world-space distance units.
		float FieldOfViewDegrees = 60.0f;
		float NearClip = DefaultNearClip;
		float FarClip = DefaultFarClip;
		float TerrainFadeStart = DefaultTerrainFadeStart;
		float TerrainRenderDistance = DefaultTerrainRenderDistance;
		float MovementSpeed = 5.0f;
		FVector2f FlyLookVelocity{0.0f};
		FVector3 FlyMovementVelocity{0.0};
		bool bShowGrid = true;
		bool bFlyNavigation = false;
		bool bOrbitNavigation = false;
		bool bPanNavigation = false;
		mutable FTransformGizmo TransformGizmo;
		mutable FPreparedSceneView PreparedSceneView;
		std::unique_ptr<FViewportPickingService> PickingService;
	};
}
