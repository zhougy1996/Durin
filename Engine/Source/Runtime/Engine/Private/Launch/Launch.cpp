#include "PCH.Engine.h"

#include "CoreGlobals.h"
#include "EngineLoop/EngineLoop.h"

FEngineLoop GEngineLoop;

int ENGINE_API main()
{
	GEngineLoop.PreInit();
	GEngineLoop.Init();
	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
	}
	GEngineLoop.Exit();
	return 0;
}