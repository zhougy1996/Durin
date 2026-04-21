#pragma once

#include "HAL/Platform.h"

#if defined(ENGINE_EXPORTS)
    #define ENGINE_API DLLEXPORT
#else
    #define ENGINE_API DLLIMPORT
#endif

