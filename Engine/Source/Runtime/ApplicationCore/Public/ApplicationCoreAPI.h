#pragma once

#include "HAL/Platform.h"

#if defined(APPLICATIONCORE_EXPORTS)
    #define APPLICATIONCORE_API DLLEXPORT
#else
    #define APPLICATIONCORE_API DLLIMPORT
#endif

