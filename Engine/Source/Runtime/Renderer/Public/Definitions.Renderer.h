#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"

#ifdef RENDERER_EXPORTS
	#define RENDERER_API DLLEXPORT
#else
	#define RENDERER_API DLLIMPORT
#endif
