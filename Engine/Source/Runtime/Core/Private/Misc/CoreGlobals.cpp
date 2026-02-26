#include "PCH.Core.h"

#include "CoreGlobals.h"

namespace Doge
{
	bool GIsRequestingExit = false;

	FPath GWorkDirectory;
	FPath GShaderPath;

	std::vector<const char*> GMonaRequiredVulkanInstanceExtensions;
}