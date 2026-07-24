#pragma once

#include "HAL/Platform.h"

#if defined(TEXTUREEDITOR_EXPORTS)
    #define TEXTUREEDITOR_API DLLEXPORT
#else
    #define TEXTUREEDITOR_API DLLIMPORT
#endif
