#pragma once

#include "HAL/Platform.h"

#if defined(ENGINEASSETBUILD_EXPORTS)
	#define ENGINEASSETBUILD_API DLLEXPORT
#else
	#define ENGINEASSETBUILD_API DLLIMPORT
#endif
