#pragma once

#include "HAL/Platform.h"

#if defined(DOGELAUNCHER_EXPORTS)
    #define DOGELAUNCHER_API DLLEXPORT
#else
    #define DOGELAUNCHER_API DLLIMPORT
#endif

