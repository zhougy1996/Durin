#include "LaunchAPI.h"
#include "CoreGlobals.h"
#include "LaunchEngineLoop.h"

int LAUNCH_API main()
{
	using namespace Durin;

	GEngineLoop.PreInit();
	GEngineLoop.Init();
	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
	}
	GEngineLoop.Exit();
	return 0;
}