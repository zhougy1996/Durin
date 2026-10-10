#pragma once

#include "HAL/Platform.h"

#if defined(TEXTURECOMPRESSOR_EXPORTS)
	#define TEXTURECOMPRESSOR_API DLLEXPORT
#else
	#define TEXTURECOMPRESSOR_API DLLIMPORT
#endif
