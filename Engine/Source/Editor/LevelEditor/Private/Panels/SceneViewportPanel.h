#pragma once

#include "Panels/LevelEditorPanel.h"
#include "Viewport/TransformGizmo.h"
#include "Widgets/MWidget.h"

struct ImVec2;

namespace Durin
{
	enum class EEditorPlayStartLocation : uint8;
	enum class EEditorPlayDestination : uint8;
	class MViewport;
	class FSceneViewport;
	class FCameraPreviewViewportClient;
	class FLevelEditorViewportClient;
	class AActor;
	class DLevel;
	struct FLevelViewportCameraState;
	struct FViewportToolbarLayout;

	// Owns the level viewport, camera clients, toolbar, and play embedding state.
	class FSceneViewportPanel final : public ILevelEditorPanel
	{
	public:
		FSceneViewportPanel();
		~FSceneViewportPanel() override;

		auto GetWindowName() const -> const char* override { return "Scene Viewport"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

		auto IsViewportHovered() const -> bool { return bViewportHovered; }
		auto IsViewportFocused() const -> bool { return bViewportFocused; }
		auto CaptureCameraState(DLevel* Level, FLevelViewportCameraState& OutState) const -> bool;
		auto RestoreCameraState(DLevel* Level, const FLevelViewportCameraState* State) -> void;
		auto FinalizeViewportFrame(FLevelEditorContext& Context) -> void;
		auto SetPreferredPlayMode(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void;
		auto GetTransformGizmo() -> FTransformGizmo*;
		auto GetTransformGizmo() const -> const FTransformGizmo*;
		auto IsGridVisible() const -> bool;
		auto SetGridVisible(bool bVisible) -> void;
		auto FocusActor(const AActor* Actor) -> void;

	private:
		auto CalculateToolbarLayout(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> FViewportToolbarLayout;
		auto DrawToolbar(FLevelEditorContext& Context, const FViewportToolbarLayout& Layout) -> void;
		auto DrawOrientationOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void;
		auto DrawFPSOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void;
		auto DrawCameraPreview(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void;
		auto UpdateCameraPreview(FLevelEditorContext& Context) -> void;
		auto UpdateViewportSize() -> void;
		auto UpdateViewportInput(FLevelEditorContext& Context, const FViewportToolbarLayout& ToolbarLayout) -> void;

		std::unique_ptr<FLevelEditorViewportClient> ViewportClient;
		std::shared_ptr<MViewport> ViewportWidget;
		std::unique_ptr<FCameraPreviewViewportClient> CameraPreviewViewportClient;
		std::shared_ptr<MViewport> CameraPreviewViewportWidget;
		std::shared_ptr<FSceneViewport> CameraPreviewSceneViewport;
		bool bViewportHovered = false;
		bool bViewportFocused = false;
		EEditorPlayStartLocation PreferredPlayStartLocation;
		EEditorPlayDestination PreferredPlayDestination;
	};
} // namespace Durin
