#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"

#ifdef RHI_EXPORTS
	#define RHI_API DLLEXPORT
#else
	#define RHI_API DLLIMPORT
#endif