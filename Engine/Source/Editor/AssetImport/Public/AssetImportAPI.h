#pragma once

#include "HAL/Platform.h"

#if defined(ASSETIMPORT_EXPORTS)
	#define ASSETIMPORT_API DLLEXPORT
#else
	#define ASSETIMPORT_API DLLIMPORT
#endif
