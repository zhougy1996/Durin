#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.Engine.h"
#include "Definitions.RenderCore.h"

#ifdef RENDERER_EXPORTS
	#define RENDERER_API DLLEXPORT
#else
	#define RENDERER_API DLLIMPORT
#endif
