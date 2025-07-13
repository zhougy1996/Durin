#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.ApplicationCore.h"

#ifdef MONACORE_EXPORTS
	#define MONACORE_API DLLEXPORT
#else
	#define MONACORE_API DLLIMPORT
#endif
