#pragma once

#include "LevelEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "LevelEditorCustomizations.h"

namespace Durin
{
	class FEditorSessionSettings;
	class FEditorWorkspaceManager;

	class FLevelEditorModule final : public IModuleInterface
	{
	public:
		LEVELEDITOR_API ~FLevelEditorModule() override;
		LEVELEDITOR_API auto StartupModule() -> void override;
		LEVELEDITOR_API auto ShutdownModule() -> void override;
		LEVELEDITOR_API auto RegisterLevelEditorWorkspace(FEditorWorkspaceManager& WorkspaceManager) -> bool;
		LEVELEDITOR_API auto GetWindowWidth() const -> int32;
		LEVELEDITOR_API auto GetWindowHeight() const -> int32;
		LEVELEDITOR_API auto GetUIScale() const -> float;
		LEVELEDITOR_API auto IsWindowMaximized() const -> bool;

	private:
		std::unique_ptr<FEditorSessionSettings> SessionSettings;
		std::vector<FLevelEditorCustomizationHandle> CustomizationHandles;
	};
}
