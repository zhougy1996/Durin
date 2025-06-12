#pragma once

#include "HAL/Platform.h"

#ifdef ENGINE_EXPORTS
	#define ENGINE_API DLLEXPORT
#else
	#define ENGINE_API DLLIMPORT
#endif // ENGINE_EXPORTS
