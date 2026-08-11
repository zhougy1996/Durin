#include "LaunchAPI.h"
#include "CoreGlobals.h"
#include "Diagnostics/ProcessCrashContext.h"
#include "HAL/PlatformProcess.h"
#include "LaunchEngineLoop.h"
#include "Misc/Build.h"
#include "Misc/Project.h"
#include "Misc/Version.h"
#include "Profiling/Profiling.h"
#include "Windows/WindowsProcessCrashHandler.h"

using namespace Durin;

int LAUNCH_API main(int argc, char** argv)
{
	InitializeProcessCrashContext(DURIN_RUNTIME_VARIANT, DURIN_BUILD_TYPE_STRING, GetEngineVersionString());
	if (!InstallWindowsProcessCrashHandler()) return 1;
	DURIN_PROFILE_CPU_ZONE_NAMED("Startup.Process");
	Durin::Profiling::RecordStartupMilestone(Durin::Profiling::EStartupMilestone::ProcessEntry);
	constexpr std::string_view WaitForProcessPrefix = "--wait-for-process=";
	constexpr std::string_view ExitAfterTicksPrefix = "--exit-after-ticks=";
	constexpr std::string_view ProjectPrefix = "--project=";
	constexpr std::string_view CrashFixturePrefix = "--native-crash-fixture=";
	constexpr std::string_view CrashSavedPrefix = "--native-crash-saved=";
	constexpr std::string_view CrashPhasePrefix = "--native-crash-at=";
	std::optional<uint32> WaitForProcessId;
	std::optional<uint64> ExitAfterTicks;
	std::optional<std::string_view> ProjectEqualsArgument;
	std::optional<std::string_view> ProjectSeparateArgument;
	std::optional<std::string_view> CrashFixture;
	std::optional<std::string_view> CrashSavedOverride;
	std::optional<std::string_view> CrashPhase;
	bool bDisableCrashDump = false;
	bool bForceCrashCollision = false;
	bool bFillCrashLogGap = false;
	bool bFaultCrashWriter = false;
	FEngineStartupParams StartupParams;
	for (int Index = 1; Index < argc; ++Index)
	{
		const std::string_view Argument = argv[Index];
		if (Argument.starts_with(WaitForProcessPrefix))
		{
			uint32 ProcessId = 0;
			const std::string_view ProcessIdText = Argument.substr(WaitForProcessPrefix.size());
			const auto [End, Error] = std::from_chars(ProcessIdText.data(), ProcessIdText.data() + ProcessIdText.size(), ProcessId);
			if (Error != std::errc{} || End != ProcessIdText.data() + ProcessIdText.size())
			{
				UninstallWindowsProcessCrashHandler();
				return 1;
			}
			WaitForProcessId = ProcessId;
		}
		else if (Argument.starts_with(ExitAfterTicksPrefix))
		{
			uint64 TickCount = 0;
			const std::string_view TickCountText = Argument.substr(ExitAfterTicksPrefix.size());
			const auto [End, Error] = std::from_chars(TickCountText.data(), TickCountText.data() + TickCountText.size(), TickCount);
			if (Error != std::errc{} || End != TickCountText.data() + TickCountText.size() || TickCount == 0)
			{
				UninstallWindowsProcessCrashHandler();
				return 1;
			}
			ExitAfterTicks = TickCount;
		}
		else if (Argument == "--hidden-window")
		{
			StartupParams.bSuppressWindowDisplay = true;
		}
		else if (Argument == "--task-scheduler-lifecycle-smoke")
		{
			StartupParams.bRunTaskSchedulerLifecycleSmoke = true;
		}
		else if (Argument == "--engine-asset-service-lifecycle-smoke")
		{
			StartupParams.bRunEngineAssetServiceLifecycleSmoke = true;
		}
		else if (Argument == "--editor-pie-lifecycle-smoke")
		{
			StartupParams.bRunEditorPIELifecycleSmoke = true;
		}
		else if (Argument == "--native-gameplay-lifecycle-smoke")
		{
			StartupParams.bRunNativeGameplayLifecycleSmoke = true;
		}
		else if (Argument == "--project-browser")
		{
			StartupParams.Project.bOpenProjectBrowser = true;
		}
		else if (Argument.starts_with(ProjectPrefix) && !ProjectEqualsArgument)
		{
			ProjectEqualsArgument = Argument.substr(ProjectPrefix.size());
		}
		else if (Argument == "--project" && Index + 1 < argc && !ProjectSeparateArgument)
		{
			ProjectSeparateArgument = argv[Index + 1];
		}
		else if (Argument.starts_with(CrashFixturePrefix))
		{
			CrashFixture = Argument.substr(CrashFixturePrefix.size());
		}
		else if (Argument.starts_with(CrashSavedPrefix))
		{
			CrashSavedOverride = Argument.substr(CrashSavedPrefix.size());
		}
		else if (Argument.starts_with(CrashPhasePrefix))
		{
			CrashPhase = Argument.substr(CrashPhasePrefix.size());
		}
		else if (Argument == "--native-crash-disable-dump")
		{
			bDisableCrashDump = true;
		}
		else if (Argument == "--native-crash-force-collision")
		{
			bForceCrashCollision = true;
		}
		else if (Argument == "--native-crash-log-gap")
		{
			bFillCrashLogGap = true;
		}
		else if (Argument == "--native-crash-fault-writer")
		{
			bFaultCrashWriter = true;
		}
	}
	ConfigureWindowsProcessCrashTestOptions(bDisableCrashDump, bForceCrashCollision, bFaultCrashWriter);

#if DURIN_BUILD_SHIPPING
	if (CrashFixture || CrashPhase || CrashSavedOverride || bDisableCrashDump
		|| bForceCrashCollision || bFillCrashLogGap || bFaultCrashWriter)
	{
		UninstallWindowsProcessCrashHandler();
		return 1;
	}
#endif
	if (CrashSavedOverride && !PublishWindowsProcessCrashRoot(*CrashSavedOverride, true))
	{
		UninstallWindowsProcessCrashHandler();
		return 1;
	}
	if (CrashFixture && (!CrashPhase || *CrashPhase == "process-entry")
		&& RunWindowsProcessCrashFixture(*CrashFixture)) return 1;
	if (WaitForProcessId && !FPlatformProcess::WaitForProcessExit(*WaitForProcessId))
	{
		UninstallWindowsProcessCrashHandler();
		return 1;
	}

	if (ProjectEqualsArgument && !ProjectEqualsArgument->empty())
		StartupParams.Project.RequestedProjectFile = *ProjectEqualsArgument;
	else if (ProjectSeparateArgument)
		StartupParams.Project.RequestedProjectFile = *ProjectSeparateArgument;
	StartupParams.NativeCrashFixture = CrashFixture.value_or(std::string_view{});
	StartupParams.NativeCrashPhase = CrashPhase.value_or(std::string_view{});
	StartupParams.bFillNativeCrashLogGap = bFillCrashLogGap;
	if (!GEngineLoop.PreInit(StartupParams))
	{
		LoggerShutdown();
		UninstallWindowsProcessCrashHandler();
		return 1;
	}
	if (!GEngineLoop.Init())
	{
		LoggerShutdown();
		UninstallWindowsProcessCrashHandler();
		return 1;
	}

	uint64 CompletedTicks = 0;
	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
		++CompletedTicks;
		if (!IsEngineExitRequested() && ExitAfterTicks && CompletedTicks >= *ExitAfterTicks)
		{
			DURIN_INFO("Requesting automated engine exit after {} ticks.", CompletedTicks);
			RequestEngineExit();
		}
	}
	GEngineLoop.Exit();
	std::string RelaunchError;
	if (!LaunchPendingEditorRelaunch(&RelaunchError)) DURIN_ERROR("Failed to relaunch editor: {}", RelaunchError);
	LoggerShutdown();
	SetProcessCrashPhase(EProcessCrashPhase::Exited);
	UninstallWindowsProcessCrashHandler();
	return 0;
}
