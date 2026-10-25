#pragma once

#include "CoreMinimal.h"
#include "LaunchArguments.h"
#include "EngineFramePhases.h"
#include "Engine/HostStartup.h"

namespace Durin
{
	class MWindow;

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

		// Returns false when mandatory process services cannot be initialized safely.
		auto PreInit(const FEngineStartupParams& Params) -> bool;
		// Returns false on failure; the process runner owns partial-startup teardown.
		auto Init() -> bool;
		auto Tick() -> void;
		auto Exit() -> void;
		auto HasLoggerStarted() const -> bool { return bLoggerStarted; }
		auto WasInitializationCancelled() const -> bool
		{
			return bInitializationCancelled;
		}
		auto GetState() const -> EEngineLoopState { return State; }
		auto GetStartupState() const -> EHostStartupState;
		auto GetStartupError() const -> std::string;
		auto GetCompletedRunningTicks() const -> uint64 { return CompletedRunningTicks; }

	private:
		auto TickPostEventFrame(bool bAllowMinimizedWait, bool bAdvanceHost = true) -> void;
		auto TickModalContinuation() -> void;

		// Previous tick timestamp in the platform clock's seconds domain.
		double LastTickTime = 0.0;
		double NextFrameTime = 0.0;
		uint64 CompletedRunningTicks = 0;
		EEngineLoopState State = EEngineLoopState::Uninitialized;
		EInteractiveFrameState FrameState = EInteractiveFrameState::Idle;
		std::shared_ptr<MWindow> StartupWindow;
		bool bLoggerStarted = false;
		bool bGameThreadDeferredExecutorStarted = false;
		bool bInitializationCancelled = false;
		std::string ProjectAdmissionError;
	};
}
