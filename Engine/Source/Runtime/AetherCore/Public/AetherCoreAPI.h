#pragma once

#include "HAL/Platform.h"

#if defined(AETHERCORE_EXPORTS)
    #define AETHERCORE_API DLLEXPORT
#else
    #define AETHERCORE_API DLLIMPORT
#endif
