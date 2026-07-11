#include "LaunchAPI.h"
#include "CoreGlobals.h"
#include "LaunchEngineLoop.h"

using namespace Durin;

int LAUNCH_API main(int ArgC, char** ArgV)
{
	GEngineLoop.PreInit(ArgC, ArgV);
	GEngineLoop.Init();

	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
	}
	GEngineLoop.Exit();
	return 0;
}
