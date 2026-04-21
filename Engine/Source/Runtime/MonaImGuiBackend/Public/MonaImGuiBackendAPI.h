#pragma once

#include "HAL/Platform.h"

#if defined(MONAIMGUIBACKEND_EXPORTS)
    #define MONAIMGUIBACKEND_API DLLEXPORT
#else
    #define MONAIMGUIBACKEND_API DLLIMPORT
#endif

