#pragma once

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Name.h"

namespace Durin::Testing
{
	inline auto InitializeDObjectSystemForTests() -> void
	{
		static const bool bInitialized = [] {
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
			if (!IsFNameInitialized()) FNameInit();
			DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}
}
