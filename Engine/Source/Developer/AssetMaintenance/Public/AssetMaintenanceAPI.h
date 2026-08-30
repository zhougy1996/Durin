#pragma once

#include "HAL/Platform.h"

#if defined(ASSETMAINTENANCE_EXPORTS)
	#define ASSETMAINTENANCE_API DLLEXPORT
#else
	#define ASSETMAINTENANCE_API DLLIMPORT
#endif
