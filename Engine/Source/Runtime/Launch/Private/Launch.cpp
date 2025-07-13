#include "CoreGlobals.h"
#include "LaunchEngineLoop.h"

FEngineLoop GEngineLoop;

int LAUNCH_API main()
{
	GEngineLoop.PreInit();
	GEngineLoop.Init();
	GEngineLoop.Init();
	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
	}
	GEngineLoop.Exit();
	return 0;
}