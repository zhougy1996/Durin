#pragma once

#include "HAL/Platform.h"

#if defined(GEOMETRYBUILD_EXPORTS)
	#define GEOMETRYBUILD_API DLLEXPORT
#else
	#define GEOMETRYBUILD_API DLLIMPORT
#endif
