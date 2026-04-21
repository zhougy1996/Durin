#pragma once

#include "HAL/Platform.h"

#if defined(MONACORE_EXPORTS)
    #define MONACORE_API DLLEXPORT
#else
    #define MONACORE_API DLLIMPORT
#endif

