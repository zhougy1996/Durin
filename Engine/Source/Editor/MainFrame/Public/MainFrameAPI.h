#pragma once

#include "HAL/Platform.h"

#if defined(MAINFRAME_EXPORTS)
    #define MAINFRAME_API DLLEXPORT
#else
    #define MAINFRAME_API DLLIMPORT
#endif

