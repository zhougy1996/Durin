#pragma once

#include "Definitions.Core.h"
#include "Definitions.ApplicationCore.h"
#include "Definitions.RHI.h"

#ifdef VULKANRHI_EXPORTS
	#define VULKANRHI_API DLLEXPORT
#else
	#define VULKANRHI_API DLLIMPORT
#endif