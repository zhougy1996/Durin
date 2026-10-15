#pragma once

#include "HAL/Platform.h"

#if defined(SHADERFORMAT_EXPORTS)
	#define SHADERFORMAT_API DLLEXPORT
#else
	#define SHADERFORMAT_API DLLIMPORT
#endif
