#pragma once

#include "CoreMinimal.h"
#include "Diagnostics/ApplicationDiagnostics.h"
#include "Misc/Project.h"
#include "EngineFramePhases.h"

namespace Durin
{
	class MWindow;

	// Carries semantic process startup choices into the engine loop.
	struct FEngineStartupParams
	{
		bool bSuppressWindowDisplay = false;
		FProjectInitializationParams Project;
	};

	// Identifies how far process-wide engine ownership has advanced.
	enum class EEngineLoopState : uint8
	{
		Uninitialized,
		PreInitializing,
		PreInitialized,
		Initializing,
		Running,
		ShuttingDown,
		Exited
	};

	// Drives process-wide engine startup, ticking, and ordered shutdown.
	class FEngineLoop
	{
	public:
		explicit FEngineLoop(FLaunchDiagnosticsRequest DiagnosticsRequest);

		// Returns false when mandatory process services cannot be initialized safely.
		auto PreInit(const FEngineStartupParams& Params) -> bool;
		// Returns false after unwinding when mandatory runtime initialization fails.
		auto Init() -> bool;
		auto Tick() -> void;
		auto Exit() -> void;
		auto HasLoggerStarted() const -> bool { return bLoggerStarted; }
		auto WasInitializationCancelled() const -> bool
		{
			return bInitializationCancelled;
		}
		auto GetState() const -> EEngineLoopState { return State; }

	private:
		auto TickPostEventFrame(bool bAllowMinimizedWait) -> void;
		auto TickModalContinuation() -> void;

		// Previous tick timestamp in the platform clock's seconds domain.
		double LastTickTime = 0.0;
		EEngineLoopState State = EEngineLoopState::Uninitialized;
		EInteractiveFrameState FrameState = EInteractiveFrameState::Idle;
		FApplicationDiagnostics Diagnostics;
		std::shared_ptr<MWindow> StartupWindow;
		bool bLoggerStarted = false;
		bool bTaskSchedulerStarted = false;
		bool bGameThreadDeferredExecutorStarted = false;
		bool bInitializationCancelled = false;
	};
}
