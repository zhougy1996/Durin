#pragma once

#include "HAL/Platform.h"

#if defined(LAUNCH_EXPORTS)
    #define LAUNCH_API DLLEXPORT
#else
    #define LAUNCH_API DLLIMPORT
#endif

