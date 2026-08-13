#pragma once

#include "HAL/Platform.h"

#if defined(TEXTUREBUILD_EXPORTS)
	#define TEXTUREBUILD_API DLLEXPORT
#else
	#define TEXTUREBUILD_API DLLIMPORT
#endif
