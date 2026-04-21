#include "Launch/API.h"
#include "CoreGlobals.h"
#include "LaunchEngineLoop.h"

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