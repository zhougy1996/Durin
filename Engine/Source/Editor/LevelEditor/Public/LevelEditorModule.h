#pragma once

#include "LevelEditorAPI.h"
#include "Modules/ModuleManager.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class FLevelEditorModule final : public IModuleInterface
	{
	public:
		LEVELEDITOR_API auto CreateLevelEditorWidget() -> std::shared_ptr<MWidget>;

	};
}
