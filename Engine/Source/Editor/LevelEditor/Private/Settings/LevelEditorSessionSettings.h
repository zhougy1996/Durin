#pragma once

#include "Settings/LevelViewportSessionSettings.h"

namespace Durin
{
	class DLevel;
}

namespace Durin::Editor::Level
{
	class FEditorAssetMoveCoordinator;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	// Persists project-scoped level-editor layout and viewport session state.
	class FLevelEditorSessionSettings
	{
	public:
		static constexpr float MinimumContentBrowserIconSize = 56.0f;
		static constexpr float DefaultContentBrowserIconSize = 88.0f;
		static constexpr float MaximumContentBrowserIconSize = 160.0f;
		static constexpr float MinimumContentBrowserTreeRatio = 0.15f;
		static constexpr float DefaultContentBrowserTreeRatio = 0.24f;
		static constexpr float MaximumContentBrowserTreeRatio = 0.55f;
		static constexpr float MinimumDetailsPaneRatio = 0.10f;
		static constexpr float DefaultDetailsPaneRatio = 0.35f;
		static constexpr float MaximumDetailsPaneRatio = 0.90f;

		auto Load() -> bool;
		auto Save(const FSceneViewportPanel* SceneViewportPanel) const -> bool;
		auto PruneInvalidViewportStates() -> void;
		auto ApplyTo(FSceneViewportPanel& SceneViewportPanel) const -> void;
		auto CaptureViewportState(const FLevelEditorContext& Context, const FSceneViewportPanel& SceneViewportPanel) -> void;
		auto RestoreViewportState(DLevel* Level, FSceneViewportPanel& SceneViewportPanel) const -> void;
		auto MoveViewportState(std::string_view OldPath, std::string_view NewPath) -> void;

		auto GetContentBrowserViewMode() const -> uint8 { return ContentBrowserViewMode; }
		auto GetContentBrowserIconSize() const -> float { return ContentBrowserIconSize; }
		auto IsContentBrowserIconSizeLocked() const -> bool { return bContentBrowserIconSizeLocked; }
		auto GetContentBrowserTreeWidth() const -> float { return ContentBrowserTreeWidth; }
		auto GetContentBrowserShowHiddenFiles() const -> bool { return bContentBrowserShowHiddenFiles; }
		auto GetContentBrowserLastDirectory() const -> const std::string& { return ContentBrowserLastDirectory; }
		auto SetContentBrowserState(uint8 ViewMode, float IconSize, bool bIconSizeLocked, float TreeWidth, bool bShowHiddenFiles, std::string LastDirectory) -> void;
		auto GetDetailsPaneRatio() const -> float { return DetailsPaneRatio; }
		auto SetDetailsPaneRatio(float Ratio) -> void;

	private:
		friend class FEditorAssetMoveCoordinator;

		FLevelViewportStateMap ViewportStates;
		bool bGizmoSnapEnabled = false;
		float GizmoTranslationSnap = 0.5f;
		float GizmoRotationSnap = 15.0f;
		float GizmoScaleSnap = 0.1f;
		uint8 GizmoMode = 0;
		uint8 GizmoSpace = 0;
		bool bShowWorldGrid = true;
		float CameraMovementSpeed = 5.0f;
		uint8 ContentBrowserViewMode = 0;
		float ContentBrowserIconSize = DefaultContentBrowserIconSize;
		bool bContentBrowserIconSizeLocked = false;
		float ContentBrowserTreeWidth = DefaultContentBrowserTreeRatio;
		bool bContentBrowserShowHiddenFiles = false;
		std::string ContentBrowserLastDirectory;
		float DetailsPaneRatio = DefaultDetailsPaneRatio;
	};
} // namespace Durin::Editor::Level
