#include "DogeEdGlobals.h"

#include "Interfaces/IMainFrameModule.h"

namespace Doge
{
	DOGEED_API auto EditorInit() -> void
	{
		DOGE_INFO("Initializing the editor.");
		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");

		MainFrameModule.CreateDefaultMainFrame();
	}
}