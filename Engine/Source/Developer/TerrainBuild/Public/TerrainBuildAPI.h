#pragma once

#include "HAL/Platform.h"

#if defined(TERRAINBUILD_EXPORTS)
	#define TERRAINBUILD_API DLLEXPORT
#else
	#define TERRAINBUILD_API DLLIMPORT
#endif
