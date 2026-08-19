#pragma once

#include "HAL/Platform.h"

#if defined(ASSETFORGE_EXPORTS)
	#define ASSETFORGE_API DLLEXPORT
#else
	#define ASSETFORGE_API DLLIMPORT
#endif
