#pragma once

#include "HAL/Platform.h"

#if defined(RENDERER_EXPORTS)
    #define RENDERER_API DLLEXPORT
#else
    #define RENDERER_API DLLIMPORT
#endif

