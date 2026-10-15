#pragma once

#include "HAL/Platform.h"

#if defined(DURINMCP_EXPORTS)
	#define DURINMCP_API DLLEXPORT
#else
	#define DURINMCP_API DLLIMPORT
#endif
