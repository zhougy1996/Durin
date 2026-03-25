#pragma once

#include "Misc/Build.h"
#include "HAL/Platform.h"

#ifdef DO_CHECK
	#ifndef check
		#define check(expr) \
			if (!(expr)) PLATFORM_BREAK()
	#endif // !check
#else
	#define check(expr)
#endif // DO_CHECK
