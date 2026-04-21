#pragma once

#include "HAL/Platform.h"

#if defined(COREDOBJECT_EXPORTS)
    #define COREDOBJECT_API DLLEXPORT
#else
    #define COREDOBJECT_API DLLIMPORT
#endif

