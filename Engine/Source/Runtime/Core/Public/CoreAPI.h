#pragma once

#include "HAL/Platform.h"

#if defined(CORE_EXPORTS)
    #define CORE_API DLLEXPORT
#else
    #define CORE_API DLLIMPORT
#endif
