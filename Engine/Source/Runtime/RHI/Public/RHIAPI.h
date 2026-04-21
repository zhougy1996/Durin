#pragma once

#include "HAL/Platform.h"

#if defined(RHI_EXPORTS)
    #define RHI_API DLLEXPORT
#else
    #define RHI_API DLLIMPORT
#endif

