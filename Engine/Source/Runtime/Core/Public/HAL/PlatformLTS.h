#pragma once

#ifdef _WIN32
	#include "Windows/WindowsPlatformLTS.h"
#elif defined(__APPLE__)
	#include "MacOS/MacOSPlatformLTS.h"
#endif
