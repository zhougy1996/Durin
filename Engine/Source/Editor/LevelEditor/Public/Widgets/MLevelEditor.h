#pragma once

#include "LevelEditorAPI.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class ILevelEditorPanel;
	class FSceneViewportPanel;
	struct FLevelEditorContext;
	struct FLevelViewportSessionState;

	class MLevelEditor final : public MWidget
	{
	public:
		LEVELEDITOR_API MLevelEditor();
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		enum class EPendingFileAction { None, NewLevel, OpenLevel, OpenProject };
		enum class EQueuedFilePopup { None, UnsavedLevel, NewLevel, ImportStaticMesh };
		auto DrawMainMenu() -> void;
		auto DrawProjectSettings() -> void;
		auto DrawFileDialogs() -> void;
		auto ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void;
		auto OpenDefaultLevel() -> void;
		auto LoadSessionSettings() -> bool;
		auto SaveSessionSettings() const -> bool;
		auto CaptureCurrentViewportState() -> void;
		auto RestoreViewportState(class DLevel* Level) -> void;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto RequestFileAction(EPendingFileAction Action) -> void;
		auto RequestOpenLevel(std::string Path) -> bool;
		auto ExecutePendingFileAction() -> void;
		auto CreateLevel(std::string_view Path) -> void;
		auto OpenLevel(std::string_view Path) -> void;
		auto BrowseStaticMeshSource() -> void;
		auto BrowseStaticMeshDestination() -> void;
		auto ImportStaticMesh() -> void;
		auto SaveCurrentLevel() -> bool;
		auto RenameCurrentLevel(std::string_view NewName) -> bool;
		auto ActivateLevel(class DLevel* Level) -> bool;
		auto SetError(std::string Message) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		std::unique_ptr<FLevelViewportSessionState> ViewportSessionState;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		bool bResetLayoutRequested = false;
		bool bProjectSettingsOpen = false;
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
		EPendingFileAction PendingFileAction = EPendingFileAction::None;
		EQueuedFilePopup QueuedFilePopup = EQueuedFilePopup::None;
		std::array<char, 512> LevelPathBuffer{};
		std::array<char, 512> ImportSourcePathBuffer{};
		std::array<char, 256> ImportAssetPathBuffer{};
		std::string LastSuggestedImportAssetPath;
		std::string DefaultLevel;
		std::string PendingLevelPath;
		std::string EditorError;
	};
} // namespace Durin
