#include "DogeEdGlobals.h"

#include "Interfaces/IMainFrameModule.h"

DOGE_ED_API auto EditorInit() -> void
{
	DOGE_INFO("Initializing the editor.");
	IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");

	MainFrameModule.CreateDefaultMainFrame();
}
