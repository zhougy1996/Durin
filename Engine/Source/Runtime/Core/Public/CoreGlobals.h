#pragma once

#include "CoreAPI.h"
#include "Delegates/Delegate.h"

namespace Durin
{
	class FRunnableThread;
	class FConfigCacheJson;

	enum class EEngineExitPhase : uint8
	{
		Running,
		QuiescingProducers,
		DetachingRenderConsumers,
		DrainingObjects,
		UnloadingModules,
		ClosingRenderAdmission,
		RenderingStopped,
		Complete
	};

	using FOnEnginePreExit = TMulticastDelegate<void()>;

	// Owns the monotonic process-exit phase and the one-shot synchronous
	// pre-exit notification. Mutating operations are game-thread-only once the
	// game thread has been initialized.
	class FEngineExitCoordinator
	{
	public:
		CORE_API auto GetPhase() const -> EEngineExitPhase;
		CORE_API auto AddPreExitCallback(std::function<void()> Callback) -> FDelegateHandle;
		CORE_API auto RemovePreExitCallback(FDelegateHandle Handle) -> bool;
		CORE_API auto BeginExit() -> bool;
		CORE_API auto AdvanceTo(EEngineExitPhase NewPhase) -> bool;

	private:
		std::atomic<EEngineExitPhase> Phase = EEngineExitPhase::Running;
		FOnEnginePreExit OnEnginePreExit;
	};

	CORE_API auto GetEngineExitPhase() -> EEngineExitPhase;
	CORE_API auto GetEngineExitPhaseName(EEngineExitPhase Phase) -> const char*;
	CORE_API auto AddOnEnginePreExit(std::function<void()> Callback) -> FDelegateHandle;
	CORE_API auto RemoveOnEnginePreExit(FDelegateHandle Handle) -> bool;
	CORE_API auto BeginEngineExit() -> bool;
	CORE_API auto AdvanceEngineExitPhase(EEngineExitPhase NewPhase) -> bool;

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

	extern CORE_API std::vector<const char*> GMonaRequiredVulkanInstanceExtensions;

	extern CORE_API double GStartTime;

	extern CORE_API uint32 GGameThreadId;
	extern CORE_API bool GIsGameThreadIdInitialized;
	extern CORE_API FRunnableThread* GRenderingThread;

	extern CORE_API uint64 GFrameCounter;
	extern CORE_API uint64 GFrameCounterRenderThread;
	extern CORE_API uint64 GRenderFrameCounter;
	extern CORE_API uint64 GRenderFrameCounterRenderThread;
}
