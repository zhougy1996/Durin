#include "Editor/EditorEngine.h"

#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	auto DEditorEngine::Init() -> void
	{
		DEngine::Init();

		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		MainFrameModule.CreateDefaultMainFrame();
		DURIN_DEBUG("Editor initialized successfully");
	}
}
