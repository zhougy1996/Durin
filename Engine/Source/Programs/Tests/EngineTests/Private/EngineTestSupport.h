#pragma once

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/Name.h"

inline auto InitializeDObjectSystem() -> void
{
	static const bool bInitialized = []() {
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
		if (!Durin::IsFNameInitialized()) Durin::FNameInit();
		Durin::DObjectInit();
		return true;
	}();
	(void)bInitialized;
}
