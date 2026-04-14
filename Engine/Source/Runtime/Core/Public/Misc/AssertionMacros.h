#pragma once

#include "Misc/Build.h"
#include "HAL/Platform.h"

#ifdef DO_CHECK
	#ifndef check
		#define check(expr) \
			if (!(expr)) \
			{ \
				PLATFORM_BREAK(); \
				__assume(0); \
			}
	#endif // !check

	#ifndef checkf
		#define checkf(expr, format, ...) \
			if (!(expr)) \
			{ \
				DOGE_ERROR(format, ##__VA_ARGS__); \
				PLATFORM_BREAK(); \
				__assume(0); \
			}
	#endif // !checkf
#else
	#define check(expr)
#endif // DO_CHECK
