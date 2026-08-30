#pragma once

#include "CoreAPI.h"

namespace Durin
{
	class FRunnableThread;
	class FConfigCacheJson;

	// Don't modify this global variable directly, use the provided functions instead.
	// RequestEngineExit() and IsEngineExitRequested() are the functions to use.
	extern CORE_API bool GIsRequestingExit;

	// Launch sets this for unattended runtime validation so every native window,
	// including secondary UI viewports, remains hidden for the process lifetime.
	extern CORE_API bool GIsWindowDisplaySuppressed;

	FORCEINLINE auto RequestEngineExit() -> void
	{
		GIsRequestingExit = true;
	}

	FORCEINLINE auto IsEngineExitRequested() -> bool
	{
		return GIsRequestingExit;
	}

	extern CORE_API double GStartTime;

	extern CORE_API uint32 GGameThreadId;
	extern CORE_API bool GIsGameThreadIdInitialized;
	extern CORE_API FRunnableThread* GRenderingThread;
	extern CORE_API FRunnableThread* GRHIThread;

	extern CORE_API uint64 GFrameCounter;
	extern CORE_API uint64 GFrameCounterRenderThread;
	extern CORE_API uint64 GRenderFrameCounter;
	extern CORE_API uint64 GRenderFrameCounterRenderThread;
}
