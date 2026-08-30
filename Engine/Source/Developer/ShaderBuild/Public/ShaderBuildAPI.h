#pragma once

#include "HAL/Platform.h"

#if defined(SHADERBUILD_EXPORTS)
	#define SHADERBUILD_API DLLEXPORT
#else
	#define SHADERBUILD_API DLLIMPORT
#endif
