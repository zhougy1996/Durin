#pragma once

#include "HAL/Platform.h"

#include "Definitions.Core.h"
#include "Definitions.RHI.h"
#include "Definitions.Engine.h"
#include "Definitions.ApplicationCore.h"
#include "Definitions.MonaCore.h"
#include "Definitions.Mona.h"
#include "Definitions.DogeEd.h"

#ifdef LEVELEDITOR_EXPORTS
	#define LEVELEDITOR_API DLLEXPORT
#else
	#define LEVELEDITOR_API DLLIMPORT
#endif