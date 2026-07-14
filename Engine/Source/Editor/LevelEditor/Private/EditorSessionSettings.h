#pragma once

#include "LevelViewportSessionSettings.h"

namespace Durin
{
	class DLevel;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	class FEditorSessionSettings
	{
	public:
		auto Load() -> bool;
		auto Save(const FSceneViewportPanel* SceneViewportPanel) const -> bool;
		auto PruneInvalidViewportStates() -> void;
		auto ApplyTo(FSceneViewportPanel& SceneViewportPanel) const -> void;
		auto CaptureViewportState(const FLevelEditorContext& Context, const FSceneViewportPanel& SceneViewportPanel) -> void;
		auto RestoreViewportState(DLevel* Level, FSceneViewportPanel& SceneViewportPanel) const -> void;
		auto MoveViewportState(std::string_view OldPath, std::string_view NewPath) -> void;

		auto GetWindowWidth() const -> int32 { return WindowWidth; }
		auto GetWindowHeight() const -> int32 { return WindowHeight; }
		auto GetUIScale() const -> float { return UIScale; }
		auto IsWindowMaximized() const -> bool { return bWindowMaximized; }
		auto SetDisplaySettings(int32 Width, int32 Height, float Scale) -> void;
		auto SetWindowMaximized(bool bMaximized) -> void { bWindowMaximized = bMaximized; }

	private:
		FLevelViewportStateMap ViewportStates;
		bool bWindowMaximized = true;
		int32 WindowWidth = 1280;
		int32 WindowHeight = 800;
		float UIScale = 1.0f;
		bool bGizmoSnapEnabled = false;
		float GizmoTranslationSnap = 0.5f;
		float GizmoRotationSnap = 15.0f;
		float GizmoScaleSnap = 0.1f;
		uint8 GizmoMode = 0;
		uint8 GizmoSpace = 0;
	};
} // namespace Durin
