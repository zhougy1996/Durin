#pragma once

#include "HAL/Platform.h"

#if defined(STATICMESHBUILD_EXPORTS)
	#define STATICMESHBUILD_API DLLEXPORT
#else
	#define STATICMESHBUILD_API DLLIMPORT
#endif
