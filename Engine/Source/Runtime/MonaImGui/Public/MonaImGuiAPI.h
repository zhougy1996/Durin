#pragma once

#include "HAL/Platform.h"

#if defined(MONAIMGUI_EXPORTS)
	#define MONAIMGUI_API DLLEXPORT
#else
	#define MONAIMGUI_API DLLIMPORT
#endif
