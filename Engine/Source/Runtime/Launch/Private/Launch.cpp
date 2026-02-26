#include "CoreGlobals.h"
#include "LaunchEngineLoop.h"

namespace Doge
{
	FEngineLoop GEngineLoop;
}

int LAUNCH_API main()
{
	using namespace Doge;

	GEngineLoop.PreInit();
	GEngineLoop.Init();
	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
	}
	GEngineLoop.Exit();
	return 0;
}