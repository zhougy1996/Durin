#pragma once

#include "HAL/Platform.h"

#if defined(MESHBUILDER_EXPORTS)
	#define MESHBUILDER_API DLLEXPORT
#else
	#define MESHBUILDER_API DLLIMPORT
#endif
