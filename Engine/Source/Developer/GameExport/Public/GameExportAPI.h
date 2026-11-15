#pragma once
#include "HAL/Platform.h"

#if defined(GAMEEXPORT_EXPORTS)
#define GAMEEXPORT_API DLLEXPORT
#else
#define GAMEEXPORT_API DLLIMPORT
#endif
