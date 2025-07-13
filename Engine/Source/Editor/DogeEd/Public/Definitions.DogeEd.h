#pragma once

#include "HAL/Platform.h"

// Public dependencies
#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.Engine.h"
#include "Definitions.MonaCore.h"
#include "Definitions.Mona.h"

#ifdef DOGEED_EXPORTS
	#define DOGEED_API DLLEXPORT
#else
	#define DOGEED_API DLLIMPORT
#endif