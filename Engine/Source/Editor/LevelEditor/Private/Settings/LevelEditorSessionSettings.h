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

		auto GetDetailsPaneRatio() const -> float { return DetailsPaneRatio; }
		auto IsDetailsPaneAutoSized() const -> bool { return bDetailsPaneAutoSized; }
		auto SetDetailsPaneRatio(float Ratio) -> void;
		auto SetDetailsPaneAutoSized(bool bAutoSized) -> void { bDetailsPaneAutoSized = bAutoSized; }

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
		bool bShowViewportStatistics = false;
		float CameraMovementSpeed = 5.0f;
		float DetailsPaneRatio = DefaultDetailsPaneRatio;
		bool bDetailsPaneAutoSized = true;
	};
} // namespace Durin::Editor::Level
