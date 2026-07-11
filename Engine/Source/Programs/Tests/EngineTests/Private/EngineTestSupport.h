#pragma once

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"

inline auto InitializeDObjectSystem() -> void
{
	static const bool bInitialized = []() {
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
		Durin::DObjectInit();
		return true;
	}();
	(void)bInitialized;
}
