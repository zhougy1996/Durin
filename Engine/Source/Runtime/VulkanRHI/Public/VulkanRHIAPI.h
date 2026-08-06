#pragma once

#include "HAL/Platform.h"

#if defined(VULKANRHI_EXPORTS)
	#define VULKANRHI_API DLLEXPORT
#else
	#define VULKANRHI_API DLLIMPORT
#endif
