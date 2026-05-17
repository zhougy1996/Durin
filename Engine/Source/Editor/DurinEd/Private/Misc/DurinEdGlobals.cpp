#include "DurinEdGlobals.h"

#include "Interfaces/IMainFrameModule.h"

namespace Durin
{
	DURINED_API auto EditorInit() -> void
	{
		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");

		MainFrameModule.CreateDefaultMainFrame();
		DURIN_DEBUG("Editor initialized successfully");
	}
}