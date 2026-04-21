#pragma once

#include "HAL/Platform.h"

#if defined(KLEE_EXPORTS)
    #define KLEE_API DLLEXPORT
#else
    #define KLEE_API DLLIMPORT
#endif

