#pragma once

#include "HAL/Platform.h"

#if defined(PHYSICS_EXPORTS)
    #define PHYSICS_API DLLEXPORT
#else
    #define PHYSICS_API DLLIMPORT
#endif
