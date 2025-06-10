#pragma once

#include "HAL/Platform.h"

// clang-format off
#ifdef _DEBUG
	#define DO_CHECK
#endif

#ifdef DO_CHECK
	#ifndef check
		#define check(expr) \
			if (!(expr)) PLATFORM_BREAK()
	#endif // !check
#else
	#define check(expr)
#endif // DO_CHECK

// clang-format on
