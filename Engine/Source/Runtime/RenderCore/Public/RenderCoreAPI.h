#pragma once

#include "HAL/Platform.h"

#if defined(RENDERCORE_EXPORTS)
    #define RENDERCORE_API DLLEXPORT
#else
    #define RENDERCORE_API DLLIMPORT
#endif

