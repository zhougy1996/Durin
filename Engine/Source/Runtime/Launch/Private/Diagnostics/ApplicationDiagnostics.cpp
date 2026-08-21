#include "ApplicationDiagnostics.h"

#include "Diagnostics/EditorPIELifecycleSmoke.h"
#include "Diagnostics/NativeGameplayLifecycleSmoke.h"
#include "Diagnostics/RendererContactRuntimeSmoke.h"
#include "Diagnostics/ProcessCrashContext.h"
#include "Diagnostics/TaskSchedulerLifecycleSmoke.h"
#include "ProcessCrashServices.h"

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
			RunProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
	}

	auto FApplicationDiagnostics::AfterLoggerStarted() const -> void
	{
		if (Request.NativeCrashPhase != ENativeCrashPhase::LoggerRunning) return;
		FillCrashLogGap();
		RunProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
	}

	auto FApplicationDiagnostics::AfterEngineInitialized() -> void
	{
		if (Request.NativeCrashPhase == ENativeCrashPhase::Running)
		{
			FillCrashLogGap();
			RunProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
		}
		if (Request.bRunRendererContactRuntimeSmoke)
			RendererContactRuntimeState = BeginRendererContactRuntimeSmoke();
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
		if (Request.bRunRendererContactRuntimeSmoke
			&& !bRendererContactRuntimeSmokeCompleted)
		{
			bRendererContactRuntimeSmokeCompleted =
				TickRendererContactRuntimeSmoke(RendererContactRuntimeState);
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
		checkf(!Request.bRunRendererContactRuntimeSmoke
			|| bRendererContactRuntimeSmokeCompleted,
			"Renderer contact runtime smoke did not complete its view matrix.");
		if (RendererContactRuntimeState)
			EndRendererContactRuntimeSmoke(RendererContactRuntimeState);
		if (Request.bRunTaskSchedulerLifecycleSmoke)
			TaskSchedulerState = BeginTaskSchedulerLifecycleSmoke();
	}

	auto FApplicationDiagnostics::BeforeAssetServiceShutdown() const -> void
	{
	}

	auto FApplicationDiagnostics::AfterTaskSystemShutdown() const -> void
	{
		if (TaskSchedulerState)
			ValidateTaskSchedulerLifecycleSmoke(TaskSchedulerState);
	}

	auto FApplicationDiagnostics::AtObjectCollection() const -> void
	{
		if (Request.NativeCrashPhase == ENativeCrashPhase::ObjectCollection)
			RunProcessCrashFixture(Request.NativeCrashFixture.value_or(""));
	}
}
