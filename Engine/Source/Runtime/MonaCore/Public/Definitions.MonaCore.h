#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"

#ifdef MONA_CORE_EXPORTS
	#define MONA_CORE_API DLLEXPORT
#else
	#define MONA_CORE_API DLLIMPORT
#endif
