#pragma once

#include "HAL/Platform.h"

#if defined(PHYSICSCORE_EXPORTS)
    #define PHYSICSCORE_API DLLEXPORT
#else
    #define PHYSICSCORE_API DLLIMPORT
#endif
