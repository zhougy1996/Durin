#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"

#ifdef RENDERCORE_EXPORTS
	#define RENDERCORE_API DLLEXPORT
#else
	#define RENDERCORE_API DLLIMPORT
#endif
