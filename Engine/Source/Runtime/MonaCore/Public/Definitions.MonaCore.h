#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.RenderCore.h"
#include "Definitions.ApplicationCore.h"

#ifdef MONA_CORE_EXPORTS
	#define MONA_CORE_API DLLEXPORT
#else
	#define MONA_CORE_API DLLIMPORT
#endif
