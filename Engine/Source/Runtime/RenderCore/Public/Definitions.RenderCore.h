#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"

#ifdef RENDER_CORE_EXPORTS
	#define RENDER_CORE_API DLLEXPORT
#else
	#define RENDER_CORE_API DLLIMPORT
#endif
