#include "PCH.Core.h"

#include "CoreGlobals.h"

bool GIsRequestingExit = false;

FPath GWorkDirectory;
FPath GShaderPath;

TArray<const char*> GMonaRequiredVulkanInstanceExtensions;