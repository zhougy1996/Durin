#pragma once

#include "HAL/Platform.h"

#if defined(ASSETIMPORTCORE_EXPORTS)
	#define ASSETIMPORTCORE_API DLLEXPORT
#else
	#define ASSETIMPORTCORE_API DLLIMPORT
#endif
