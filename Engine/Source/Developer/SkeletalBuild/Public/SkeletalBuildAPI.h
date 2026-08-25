#pragma once

#include "HAL/Platform.h"

#if defined(SKELETALBUILD_EXPORTS)
	#define SKELETALBUILD_API DLLEXPORT
#else
	#define SKELETALBUILD_API DLLIMPORT
#endif
