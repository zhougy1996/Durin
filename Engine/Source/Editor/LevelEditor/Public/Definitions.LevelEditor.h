#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.Engine.h"
#include "Definitions.RenderCore.h"
#include "Definitions.ApplicationCore.h"
#include "Definitions.MonaCore.h"
#include "Definitions.Mona.h"
#include "Definitions.DogeEd.h"

#ifdef LEVEL_EDITOR_EXPORTS
	#define LEVEL_EDITOR_API DLLEXPORT
#else
	#define LEVEL_EDITOR_API DLLIMPORT
#endif