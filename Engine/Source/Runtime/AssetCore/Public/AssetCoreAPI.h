#pragma once

#include "HAL/Platform.h"

#if defined(ASSETCORE_EXPORTS)
    #define ASSETCORE_API DLLEXPORT
#else
    #define ASSETCORE_API DLLIMPORT
#endif

