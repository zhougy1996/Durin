#include "DogeEdGlobals.h"

#include "Interfaces/IMainFrameModule.h"

namespace Doge
{
	DOGEED_API auto EditorInit() -> void
	{
		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");

		MainFrameModule.CreateDefaultMainFrame();
		DOGE_DEBUG("Editor initialized successfully");
	}
}