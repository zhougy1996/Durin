#pragma once

#include "LevelEditorAPI.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class ILevelEditorPanel;
	struct FLevelEditorContext;

	class MLevelEditor final : public MWidget
	{
	public:
		LEVELEDITOR_API MLevelEditor();
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		enum class EPendingFileAction { None, NewLevel, OpenLevel, StartupLevel };
		enum class EQueuedFilePopup { None, UnsavedLevel, NewLevel, OpenLevel, StartupLevel, ImportStaticMesh };
		auto DrawMainMenu() -> void;
		auto DrawFileDialogs() -> void;
		auto ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void;
		auto InitializeStartupLevel() -> void;
		auto LoadSessionSettings() -> bool;
		auto SaveSessionSettings() const -> bool;
		auto RecordRecentLevel(std::string_view Path) -> void;
		auto GetStartupMountRoot() const -> std::string;
		auto DrawLevelAssetList(bool bStartupPicker) -> bool;
		auto RequestFileAction(EPendingFileAction Action) -> void;
		auto ExecutePendingFileAction() -> void;
		auto CreateLevel(std::string_view Path) -> void;
		auto OpenLevel(std::string_view Path) -> void;
		auto BrowseStaticMeshSource() -> void;
		auto BrowseStaticMeshDestination() -> void;
		auto ImportStaticMesh() -> void;
		auto SaveCurrentLevel() -> bool;
		auto ActivateLevel(class DLevel* Level) -> bool;
		auto SetError(std::string Message) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		bool bResetLayoutRequested = false;
		bool bAlwaysAskForStartupLevel = false;
		int32 WindowWidth = 1280;
		int32 WindowHeight = 800;
		float UIScale = 1.0f;
		EPendingFileAction PendingFileAction = EPendingFileAction::None;
		EQueuedFilePopup QueuedFilePopup = EQueuedFilePopup::None;
		std::array<char, 512> LevelPathBuffer{};
		std::array<char, 256> OpenFilterBuffer{};
		std::array<char, 512> ImportSourcePathBuffer{};
		std::array<char, 256> ImportAssetPathBuffer{};
		std::string LastSuggestedImportAssetPath;
		std::unordered_map<std::string, std::string> RecentLevelByMount;
		std::string EditorError;
	};
} // namespace Durin
