#pragma once

#ifdef _WIN32
	#include "Windows/WindowsPlatform.h"
#elif defined(__APPLE__)
	#include "MacOS/MacOSPlatform.h"
#endif


