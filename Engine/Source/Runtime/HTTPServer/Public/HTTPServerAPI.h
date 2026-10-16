#pragma once

#include "HAL/Platform.h"

#if defined(HTTPSERVER_EXPORTS)
    #define HTTPSERVER_API DLLEXPORT
#else
    #define HTTPSERVER_API DLLIMPORT
#endif
