#include "DogeEdGlobals.h"

#include "Interfaces/IMainFrameModule.h"

namespace Durin
{
	DOGEED_API auto EditorInit() -> void
	{
		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");

		MainFrameModule.CreateDefaultMainFrame();
		DURIN_DEBUG("Editor initialized successfully");
	}
}