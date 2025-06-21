#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.Engine.h"
#include "Definitions.ApplicationCore.h"
#include "Definitions.MonaCore.h"
#include "Definitions.Mona.h"

#ifdef DOGE_ED_EXPORTS
	#define DOGE_ED_API DLLEXPORT
#else
	#define DOGE_ED_API DLLIMPORT
#endif