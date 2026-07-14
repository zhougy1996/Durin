#pragma once

#include "LevelEditorAPI.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class ILevelEditorPanel;
	class FEditorSessionSettings;
	class FLevelDocumentController;
	class FSceneViewportPanel;
	class FStaticMeshImportDialog;
	struct FLevelEditorContext;

	class MLevelEditor final : public MWidget
	{
	public:
		explicit MLevelEditor(FEditorSessionSettings& InSessionSettings);
		LEVELEDITOR_API ~MLevelEditor() override;
		LEVELEDITOR_API auto Construct() -> void override;
		LEVELEDITOR_API auto Draw() -> void override;

	private:
		auto DrawMainMenu() -> void;
		auto DrawProjectSettings() -> void;
		auto ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void;
		auto LoadProjectSettings() -> bool;
		auto SaveProjectSettings() -> bool;
		auto SetError(std::string Message) -> void;
		auto BuildDefaultLayout(uint32 DockSpaceId) -> void;

		std::unique_ptr<FLevelEditorContext> Context;
		FEditorSessionSettings& SessionSettings;
		std::unique_ptr<FLevelDocumentController> DocumentController;
		std::unique_ptr<FStaticMeshImportDialog> StaticMeshImportDialog;
		std::vector<std::unique_ptr<ILevelEditorPanel>> Panels;
		FSceneViewportPanel* SceneViewportPanel = nullptr;
		bool bResetLayoutRequested = false;
		bool bProjectSettingsOpen = false;
		std::string DefaultLevel;
		std::string EditorError;
	};
} // namespace Durin
