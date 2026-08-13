#pragma once

#include "HAL/Platform.h"

#if defined(ASSETBUILDCORE_EXPORTS)
	#define ASSETBUILDCORE_API DLLEXPORT
#else
	#define ASSETBUILDCORE_API DLLIMPORT
#endif
