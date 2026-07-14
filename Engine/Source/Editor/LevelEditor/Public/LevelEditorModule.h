#pragma once

#include "LevelEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class FEditorSessionSettings;

	class FLevelEditorModule final : public IModuleInterface
	{
	public:
		LEVELEDITOR_API ~FLevelEditorModule() override;
		LEVELEDITOR_API auto StartupModule() -> void override;
		LEVELEDITOR_API auto ShutdownModule() -> void override;
		LEVELEDITOR_API auto CreateLevelEditorWidget() -> std::shared_ptr<MWidget>;
		LEVELEDITOR_API auto GetWindowWidth() const -> int32;
		LEVELEDITOR_API auto GetWindowHeight() const -> int32;
		LEVELEDITOR_API auto GetUIScale() const -> float;
		LEVELEDITOR_API auto IsWindowMaximized() const -> bool;

	private:
		std::unique_ptr<FEditorSessionSettings> SessionSettings;
	};
}
