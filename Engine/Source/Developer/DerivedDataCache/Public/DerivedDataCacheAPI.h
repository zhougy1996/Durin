#pragma once

#include "HAL/Platform.h"

#if defined(DERIVEDDATACACHE_EXPORTS)
	#define DERIVEDDATACACHE_API DLLEXPORT
#else
	#define DERIVEDDATACACHE_API DLLIMPORT
#endif
