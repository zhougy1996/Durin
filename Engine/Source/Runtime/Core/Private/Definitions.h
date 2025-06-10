#pragma once

#define MODULE_NAME "Core"

#include "HAL/Platform.h"

#ifdef CORE_EXPORTS
	#define CORE_API DLLEXPORT
#else
	#define CORE_API DLLIMPORT
#endif // CORE_EXPORTS
