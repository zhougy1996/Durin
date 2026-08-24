#pragma once

#include "HAL/Platform.h"

#if defined(ASSETFORGEBUILTINS_EXPORTS)
	#define ASSETFORGEBUILTINS_API DLLEXPORT
#else
	#define ASSETFORGEBUILTINS_API DLLIMPORT
#endif
