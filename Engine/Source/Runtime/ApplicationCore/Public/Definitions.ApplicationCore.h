#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"

#ifdef APPLICATION_CORE_EXPORTS
	#define APPLICATION_CORE_API DLLEXPORT
#else
	#define APPLICATION_CORE_API DLLIMPORT
#endif
