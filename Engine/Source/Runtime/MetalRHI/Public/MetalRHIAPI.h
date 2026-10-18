#pragma once

#include "HAL/Platform.h"

#if defined(METALRHI_EXPORTS)
	#define METALRHI_API DLLEXPORT
#else
	#define METALRHI_API DLLIMPORT
#endif
