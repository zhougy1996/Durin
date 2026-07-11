#include "LaunchAPI.h"
#include "CoreGlobals.h"
#include "LaunchEngineLoop.h"

using namespace Durin;

int LAUNCH_API main(int argc, char** argv)
{
	std::vector<std::string_view> Arguments;
	for (int Index = 1; Index < argc; ++Index) Arguments.emplace_back(argv[Index]);
	GEngineLoop.PreInit(Arguments);
	GEngineLoop.Init();

	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
	}
	GEngineLoop.Exit();
	return 0;
}
