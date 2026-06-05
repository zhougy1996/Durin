#include "LevelEditorModule.h"

#include "Widgets/MLevelEditor.h"

namespace Durin
{
	IMPLEMENT_MODULE(FLevelEditorModule, LevelEditor)

	LEVELEDITOR_API auto FLevelEditorModule::CreateLevelEditorWidget() -> std::shared_ptr<Mona::MWidget>
	{
		std::shared_ptr<MLevelEditor> LevelEditorWidget = std::make_shared<MLevelEditor>();
		LevelEditorWidget->Construct();
		return LevelEditorWidget;
	}
}
