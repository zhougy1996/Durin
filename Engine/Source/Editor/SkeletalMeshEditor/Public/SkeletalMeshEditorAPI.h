#pragma once

#include "HAL/Platform.h"

#if defined(SKELETALMESHEDITOR_EXPORTS)
    #define SKELETALMESHEDITOR_API DLLEXPORT
#else
    #define SKELETALMESHEDITOR_API DLLIMPORT
#endif
