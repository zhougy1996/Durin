#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"

#ifdef ENGINE_EXPORTS
	#define ENGINE_API DLLEXPORT
#else
	#define ENGINE_API DLLIMPORT
#endif
