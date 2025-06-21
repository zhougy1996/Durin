#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.ApplicationCore.h"
#include "Definitions.MonaCore.h"
#include "Definitions.Mona.h"

#ifdef ENGINE_EXPORTS
	#define ENGINE_API DLLEXPORT
#else
	#define ENGINE_API DLLIMPORT
#endif
