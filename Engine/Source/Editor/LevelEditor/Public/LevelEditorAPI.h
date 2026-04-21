#pragma once

#include "HAL/Platform.h"

#if defined(LEVELEDITOR_EXPORTS)
    #define LEVELEDITOR_API DLLEXPORT
#else
    #define LEVELEDITOR_API DLLIMPORT
#endif

