#include "ApplicationDiagnostics.h"

#include "Diagnostics/EditorPIELifecycleSmoke.h"
#include "Diagnostics/NativeGameplayLifecycleSmoke.h"
#include "Diagnostics/ProcessCrashContext.h"
#include "Diagnostics/TaskSchedulerLifecycleSmoke.h"
#include "EngineAssetServices.h"
#include "Windows/WindowsProcessCrashHandler.h"

#if DURIN_WITH_EDITOR
	#include "Editor/EditorEngine.h"
#endif

namespace Durin
{
	FApplicationDiagnostics::FApplicationDiagnostics(FLaunchDiagnosticsRequest InRequest)
		: Request(std::move(InRequest))
	{
	}

	auto FApplicationDiagnostics::FillCrashLogGap() const -> void
	{
		if (!Request.bFillNativeCrashLogGap) return;
		for (uint32 Index = 0; Index < 4096; ++Index)
			DURIN_TRACE("Native crash logger-tail qualification record {}.", Index);
	}

	auto FApplicationDiagnostics::AtPreInitialization() const -> void
	{
		if (Request.NativeCrashPhase == ENativeCrashPhase::PreInitialization)
			RunWindowsProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
	}

	auto FApplicationDiagnostics::AfterLoggerStarted() const -> void
	{
		if (Request.NativeCrashPhase != ENativeCrashPhase::LoggerRunning) return;
		FillCrashLogGap();
		RunWindowsProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
	}

	auto FApplicationDiagnostics::AfterEngineInitialized() -> void
	{
		if (Request.bRunEngineAssetServiceLifecycleSmoke)
			BeginEngineAssetServiceLifecycleSmoke();
		if (Request.NativeCrashPhase == ENativeCrashPhase::Running)
		{
			FillCrashLogGap();
			RunWindowsProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
		}
	}

	auto FApplicationDiagnostics::Tick() -> void
	{
#if DURIN_WITH_EDITOR
		if (Request.bRunEditorPIELifecycleSmoke
			&& !bEditorPIELifecycleSmokeCompleted && GEditor)
			bEditorPIELifecycleSmokeCompleted = TryRunEditorPIELifecycleSmoke();
#endif
		if (Request.bRunNativeGameplayLifecycleSmoke
			&& !bNativeGameplayLifecycleSmokeCompleted)
		{
			RunNativeGameplayLifecycleSmoke();
			bNativeGameplayLifecycleSmokeCompleted = true;
		}
	}

	auto FApplicationDiagnostics::BeginConsumerDetachment() -> void
	{
#if DURIN_WITH_EDITOR
		checkf(!Request.bRunEditorPIELifecycleSmoke
			|| bEditorPIELifecycleSmokeCompleted,
			"Editor PIE lifecycle smoke never observed an active source Level.");
#endif
		checkf(!Request.bRunNativeGameplayLifecycleSmoke
			|| bNativeGameplayLifecycleSmokeCompleted,
			"Native gameplay lifecycle smoke did not execute.");
		if (Request.bRunTaskSchedulerLifecycleSmoke)
			TaskSchedulerState = BeginTaskSchedulerLifecycleSmoke();
	}

	auto FApplicationDiagnostics::BeforeAssetServiceShutdown() const -> void
	{
		if (Request.bRunEngineAssetServiceLifecycleSmoke)
			ValidateEngineAssetServiceLifecycleSmoke();
	}

	auto FApplicationDiagnostics::AfterTaskSystemShutdown() const -> void
	{
		if (TaskSchedulerState)
			ValidateTaskSchedulerLifecycleSmoke(TaskSchedulerState);
	}

	auto FApplicationDiagnostics::AtObjectCollection() const -> void
	{
		if (Request.NativeCrashPhase == ENativeCrashPhase::ObjectCollection)
			RunWindowsProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
	}
}
