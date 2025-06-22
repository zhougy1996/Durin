#pragma once

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.Engine.h"
#include "Definitions.ApplicationCore.h"
#include "Definitions.MonaCore.h"
#include "Definitions.Mona.h"
#include "Definitions.DogeEd.h"

#ifdef LAUNCH_EXPORTS
	#define LAUNCH_API DLLEXPORT
#else
	#define LAUNCH_API DLLIMPORT
#endif