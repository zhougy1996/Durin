#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.Engine.h"
#include "Definitions.ApplicationCore.h"
#include "Definitions.MonaCore.h"
#include "Definitions.Mona.h"
#include "Definitions.DogeEd.h"

#ifdef MAIN_FRAME_EXPORTS
	#define MAIN_FRAME_API DLLEXPORT
#else
	#define MAIN_FRAME_API DLLIMPORT
#endif