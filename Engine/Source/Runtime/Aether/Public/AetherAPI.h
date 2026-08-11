#pragma once

#include "HAL/Platform.h"

#if defined(AETHER_EXPORTS)
    #define AETHER_API DLLEXPORT
#else
    #define AETHER_API DLLIMPORT
#endif
