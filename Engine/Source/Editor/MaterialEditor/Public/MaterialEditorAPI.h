#pragma once

#include "HAL/Platform.h"

#if defined(MATERIALEDITOR_EXPORTS)
    #define MATERIALEDITOR_API DLLEXPORT
#else
    #define MATERIALEDITOR_API DLLIMPORT
#endif

