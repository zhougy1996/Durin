#pragma once

namespace Doge
{
	class FRunnableThread;

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

	extern CORE_API std::vector<const char*> GMonaRequiredVulkanInstanceExtensions;

	extern CORE_API FPath GWorkDirectory;
	extern CORE_API FPath GShaderPath;

	extern CORE_API uint32 GGameThreadId;
	extern CORE_API bool GIsGameThreadIdInitialized;
	extern CORE_API FRunnableThread* GRenderingThread;

	extern CORE_API uint64 GFrameCounter;
	extern CORE_API uint64 GFrameCounterRenderThread;
}