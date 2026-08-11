#pragma once

#include "CoreMinimal.h"
#include "Misc/Project.h"

namespace Durin
{
	// Carries semantic process startup choices into the engine loop.
	struct FEngineStartupParams
	{
		bool bSuppressWindowDisplay = false;
		bool bRunTaskSchedulerLifecycleSmoke = false;
		bool bRunEngineAssetServiceLifecycleSmoke = false;
		bool bRunEditorPIELifecycleSmoke = false;
		bool bRunNativeGameplayLifecycleSmoke = false;
		std::string_view NativeCrashFixture;
		std::string_view NativeCrashPhase;
		bool bFillNativeCrashLogGap = false;
		FProjectInitializationParams Project;
	};

	// Drives process-wide engine startup, ticking, and ordered shutdown.
	class FEngineLoop
	{
	public:
		// Returns false when mandatory process services cannot be initialized safely.
		auto PreInit(const FEngineStartupParams& Params) -> bool;
		// Returns false after unwinding when mandatory runtime initialization fails.
		auto Init() -> bool;
		auto Tick() -> void;
		auto Exit() -> void;

	private:
		// Previous tick timestamp in the platform clock's seconds domain.
		double LastTickTime = 0.0;
		bool bRunTaskSchedulerLifecycleSmoke = false;
		bool bRunEngineAssetServiceLifecycleSmoke = false;
		bool bRunEditorPIELifecycleSmoke = false;
		bool bEditorPIELifecycleSmokeCompleted = false;
		bool bRunNativeGameplayLifecycleSmoke = false;
		bool bNativeGameplayLifecycleSmokeCompleted = false;
		std::string_view NativeCrashFixture;
		std::string_view NativeCrashPhase;
		bool bFillNativeCrashLogGap = false;
	};

	extern FEngineLoop GEngineLoop;
}
