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
		enum class EPendingFileAction { None, NewLevel, OpenLevel };
		enum class EQueuedFilePopup { None, UnsavedLevel, NewLevel, OpenLevel };
		auto DrawMainMenu() -> void;
		auto DrawFileDialogs() -> void;
		auto RequestFileAction(EPendingFileAction Action) -> void;
		auto ExecutePendingFileAction() -> void;
		auto CreateLevel(std::string_view Path) -> void;
		auto OpenLevel(std::string_view Path) -> void;
		auto SaveCurrentLevel() -> bool;
		auto ActivateLevel(class DLevel* Level) -> bool;
		auto SetError(std::string Message) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		bool bResetLayoutRequested = false;
		EPendingFileAction PendingFileAction = EPendingFileAction::None;
		EQueuedFilePopup QueuedFilePopup = EQueuedFilePopup::None;
		std::array<char, 512> LevelPathBuffer{};
		std::array<char, 256> OpenFilterBuffer{};
		std::string EditorError;
	};
} // namespace Durin
