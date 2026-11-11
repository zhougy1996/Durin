#pragma once

#include "HAL/Platform.h"

#if defined(PROJECTS_EXPORTS)
    #define PROJECTS_API DLLEXPORT
#else
    #define PROJECTS_API DLLIMPORT
#endif
