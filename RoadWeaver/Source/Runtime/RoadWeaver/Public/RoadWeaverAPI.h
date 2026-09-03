#pragma once

#include "HAL/Platform.h"

#if defined(ROADWEAVER_EXPORTS)
	#define ROADWEAVER_API DLLEXPORT
#else
	#define ROADWEAVER_API DLLIMPORT
#endif
