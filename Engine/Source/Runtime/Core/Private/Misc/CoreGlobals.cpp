#include "PCH.Core.h"

#include "CoreGlobals.h"

namespace Durin
{
	bool GIsRequestingExit = false;
	bool GIsWindowDisplaySuppressed = false;

	std::vector<const char*> GMonaRequiredVulkanInstanceExtensions;

	uint32 GGameThreadId = 0;
	bool GIsGameThreadIdInitialized = false;


	uint64 GFrameCounter = 0;
	uint64 GFrameCounterRenderThread = 0;
	uint64 GRenderFrameCounter = 0;
	uint64 GRenderFrameCounterRenderThread = 0;
}
