#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"

#ifdef MONA_EXPORTS
	#define MONA_API DLLEXPORT
#else
	#define MONA_API DLLIMPORT
#endif
