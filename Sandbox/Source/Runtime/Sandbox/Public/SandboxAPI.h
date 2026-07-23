#pragma once

#include "HAL/Platform.h"

#if defined(SANDBOX_EXPORTS)
    #define SANDBOX_API DLLEXPORT
#else
    #define SANDBOX_API DLLIMPORT
#endif
