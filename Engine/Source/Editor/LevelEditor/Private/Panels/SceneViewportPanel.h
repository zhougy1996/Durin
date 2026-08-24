#pragma once

#include "Panels/LevelEditorPanel.h"
#include "Viewport/TransformGizmo.h"
#include "LevelEditorViewportEditing.h"
#include "Widgets/MWidget.h"
#include "Console/ConsoleCommand.h"

struct ImVec2;

namespace Durin::Editor
{
	enum class EPlayStartLocation : uint8;
	enum class EPlayDestination : uint8;
}

namespace Durin
{
	class MViewport;
	class FSceneViewport;
	class AActor;
	class DLevel;
	struct FSceneViewportStatisticsSnapshot;
	struct FSceneViewportRenderGraphSnapshot;
}

namespace Durin::Editor::Level
{
	class FCameraPreviewViewportClient;
	class FLevelEditorViewportClient;
	struct FLevelViewportCameraState;
	struct FViewportToolbarLayout;
	class FViewportToolbar;

	// Owns the level viewport, camera clients, toolbar, and play embedding state.
	class FSceneViewportPanel final : public ILevelEditorPanel
	{
	public:
		explicit FSceneViewportPanel(FModuleOwnedCallbackGate OwnerGate = {});
		~FSceneViewportPanel() override;

		auto GetWindowName() const -> const char* override { return "Scene Viewport"; }
		auto Draw(FLevelEditorContext& Context) -> void override;

		auto IsViewportHovered() const -> bool { return bViewportHovered; }
		auto IsViewportFocused() const -> bool { return bViewportFocused; }
		auto CaptureCameraState(DLevel* Level, FLevelViewportCameraState& OutState) const -> bool;
		auto RestoreCameraState(DLevel* Level, const FLevelViewportCameraState* State) -> void;
		auto FinalizeViewportFrame(FLevelEditorContext& Context) -> void;
		auto SetPreferredPlayMode(::Durin::Editor::EPlayStartLocation StartLocation, ::Durin::Editor::EPlayDestination Destination) -> void;
		auto GetTransformGizmo() -> FTransformGizmo*;
		auto GetTransformGizmo() const -> const FTransformGizmo*;
		auto GetEditModeManager() -> FLevelViewportEditModeManager& { return EditModeManager; }
		auto IsGridVisible() const -> bool;
		auto SetGridVisible(bool bVisible) -> void;
		auto GetCameraMovementSpeed() const -> float;
		auto SetCameraMovementSpeed(float Speed) -> void;
		auto IsStatisticsVisible() const -> bool { return bShowStatistics; }
		auto SetStatisticsVisible(bool bVisible) -> void { bShowStatistics = bVisible; }
		auto SetOpenRenderingDiagnostics(std::function<void()> Callback) -> void
		{
			OpenRenderingDiagnostics = std::move(Callback);
		}
		auto GetRenderStatisticsSnapshot() const
			-> FSceneViewportStatisticsSnapshot;
		auto GetRenderGraphSnapshot() const
			-> FSceneViewportRenderGraphSnapshot;
		auto RequestRenderGraphCapture() -> void;
		auto FocusActor(const AActor* Actor) -> void;

	private:
		auto DrawCameraPreview(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void;
		auto UpdateCameraPreview(FLevelEditorContext& Context) -> void;
		auto UpdateViewportSize() -> void;
		auto UpdateViewportInput(FLevelEditorContext& Context, const FViewportToolbarLayout& ToolbarLayout) -> void;
		auto RegisterViewportConsoleCommands() -> void;
		FModuleOwnedCallbackGate OwnerGate;

		std::unique_ptr<FLevelEditorViewportClient> ViewportClient;
		std::shared_ptr<MViewport> ViewportWidget;
		std::shared_ptr<FSceneViewport> SceneViewport;
		std::unique_ptr<FCameraPreviewViewportClient> CameraPreviewViewportClient;
		std::shared_ptr<MViewport> CameraPreviewViewportWidget;
		std::shared_ptr<FSceneViewport> CameraPreviewSceneViewport;
		std::unique_ptr<FViewportToolbar> ViewportToolbar;
		FLevelViewportEditModeManager EditModeManager;
		bool bViewportHovered = false;
		bool bViewportFocused = false;
		bool bShowStatistics = false;
		std::function<void()> OpenRenderingDiagnostics;
		::Durin::Editor::EPlayStartLocation PreferredPlayStartLocation;
		::Durin::Editor::EPlayDestination PreferredPlayDestination;
		std::vector<FConsoleCommandHandle> ViewportConsoleCommandHandles;
	};
} // namespace Durin::Editor::Level
