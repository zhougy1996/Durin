#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"

#ifdef MONA_EXPORTS
	#define KLEE_API DLLEXPORT
#else
	#define KLEE_API DLLIMPORT
#endif
