#include "PCH.Core.h"

#include "CoreGlobals.h"

bool GIsRequestingExit = false;

FPath GWorkDirectory;
FPath GShaderPath;

std::vector<const char*> GMonaRequiredVulkanInstanceExtensions;