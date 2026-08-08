#pragma once

#include "HAL/Platform.h"

#if defined(STATICMESHEDITOR_EXPORTS)
    #define STATICMESHEDITOR_API DLLEXPORT
#else
    #define STATICMESHEDITOR_API DLLIMPORT
#endif
