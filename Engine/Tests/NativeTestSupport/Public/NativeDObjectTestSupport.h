#pragma once

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Name.h"

namespace Durin::Testing
{
	inline auto InitializeDObjectSystemForTests() -> void
	{
		// GoogleTest death tests fork an already-initialized process on POSIX.
		// Refresh the native thread identity in the child before consulting the
		// inherited one-time DObject initialization state.
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		static const bool bInitialized = [] {
			if (!IsFNameInitialized()) FNameInit();
			DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}
}
