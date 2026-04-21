#pragma once

#include "HAL/Platform.h"

#if defined(MONA_EXPORTS)
    #define MONA_API DLLEXPORT
#else
    #define MONA_API DLLIMPORT
#endif

