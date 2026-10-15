#pragma once

#include "HAL/Platform.h"

#if defined(TARGETPLATFORM_EXPORTS)
#define TARGETPLATFORM_API DLLEXPORT
#else
#define TARGETPLATFORM_API DLLIMPORT
#endif
