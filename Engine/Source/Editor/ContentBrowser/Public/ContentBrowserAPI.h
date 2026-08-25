#pragma once

#include "HAL/Platform.h"

#if defined(CONTENTBROWSER_EXPORTS)
	#define CONTENTBROWSER_API DLLEXPORT
#else
	#define CONTENTBROWSER_API DLLIMPORT
#endif
