#pragma once

#include "HAL/Platform.h"

#if defined(ASSETREGISTRY_EXPORTS)
	#define ASSETREGISTRY_API DLLEXPORT
#else
	#define ASSETREGISTRY_API DLLIMPORT
#endif
