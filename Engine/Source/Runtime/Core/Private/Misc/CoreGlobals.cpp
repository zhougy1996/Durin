#include "PCH.Core.h"

#include "CoreGlobals.h"

namespace Doge
{
	bool GIsRequestingExit = false;

	FPath GWorkDirectory;
	FPath GShaderPath;

	std::vector<const char*> GMonaRequiredVulkanInstanceExtensions;

	auto GGameThreadId = std::thread::id();
	bool GIsGameThreadIdInitialized = false;


	uint64 GFrameCounter = 0;
	uint64 GFrameCounterRenderThread = 0;
}