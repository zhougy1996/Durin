#pragma once

#include "HAL/Platform.h"

#if defined(DOGEED_EXPORTS)
    #define DOGEED_API DLLEXPORT
#else
    #define DOGEED_API DLLIMPORT
#endif

