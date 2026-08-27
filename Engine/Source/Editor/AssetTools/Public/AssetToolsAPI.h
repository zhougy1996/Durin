#pragma once

#include "HAL/Platform.h"

#if defined(ASSETTOOLS_EXPORTS)
    #define ASSETTOOLS_API DLLEXPORT
#else
    #define ASSETTOOLS_API DLLIMPORT
#endif
