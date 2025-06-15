#pragma once

// Don't modify this global variable directly, use the provided functions instead.
// RequestEngineExit() and IsEngineExitRequested() are the functions to use.
extern CORE_API bool GIsRequestingExit;

FORCEINLINE auto RequestEngineExit() -> void
{
	GIsRequestingExit = true;
}

FORCEINLINE auto IsEngineExitRequested() -> bool
{
	return GIsRequestingExit;
}