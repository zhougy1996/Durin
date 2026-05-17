#pragma once

#include "HAL/Platform.h"

#if defined(DURINED_EXPORTS)
    #define DURINED_API DLLEXPORT
#else
    #define DURINED_API DLLIMPORT
#endif

