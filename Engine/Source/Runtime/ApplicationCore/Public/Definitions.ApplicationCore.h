#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"

#ifdef APPLICATIONCORE_EXPORTS
	#define APPLICATIONCORE_API DLLEXPORT
#else
	#define APPLICATIONCORE_API DLLIMPORT
#endif
