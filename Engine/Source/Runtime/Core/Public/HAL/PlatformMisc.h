#pragma once

#ifdef _WIN32
	#include "Windows/WindowsPlatformMisc.h"
#elif defined(__APPLE__)
	#include "MacOS/MacOSPlatformMisc.h"
#endif
