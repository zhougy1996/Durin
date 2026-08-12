#include "ApplicationProcess.h"

#include "CoreGlobals.h"
#include "Diagnostics/ProcessCrashContext.h"
#include "EngineLoop.h"
#include "HAL/PlatformProcess.h"
#include "LaunchContracts/LaunchArguments.h"
#include "Misc/Project.h"
#include "Misc/StartupCommand.h"
#include "Misc/Version.h"
#include "Profiling/Profiling.h"
#include "Windows/WindowsProcessCrashHandler.h"

namespace Durin
{
	namespace
	{
		// Restores the previously installed process handlers on every ordinary return.
		class FProcessCrashHandlerGuard
		{
		public:
			~FProcessCrashHandlerGuard() { UninstallWindowsProcessCrashHandler(); }
		};

		auto WriteArgumentError(const FLaunchArgumentError& Error) -> void
		{
			std::fprintf(stderr, "Durin: option '%s' %s.\n",
				Error.Option.c_str(), Error.Message.c_str());
		}

		auto MakeStartupParams(const FLaunchHostRequest& Host) -> FEngineStartupParams
		{
			FEngineStartupParams Params;
			Params.bSuppressWindowDisplay = Host.bSuppressWindowDisplay;
			Params.Project.bOpenProjectBrowser = Host.bOpenProjectBrowser;
			Params.Project.RequestedProjectFile = Host.ProjectFile.value_or("");
			return Params;
		}

		auto IsProcessEntryCrash(const FLaunchDiagnosticsRequest& Diagnostics) -> bool
		{
			return Diagnostics.NativeCrashFixture
				&& (!Diagnostics.NativeCrashPhase
					|| Diagnostics.NativeCrashPhase == ENativeCrashPhase::ProcessEntry);
		}
	}

	auto RunApplicationProcess(int ArgumentCount, char** Arguments) -> int
	{
		InitializeProcessCrashContext(
			DURIN_RUNTIME_VARIANT, DURIN_BUILD_TYPE_STRING, GetEngineVersionString());
		if (!InstallWindowsProcessCrashHandler()) return 1;
		FProcessCrashHandlerGuard CrashHandlerGuard;
		DURIN_PROFILE_CPU_ZONE_NAMED("Startup.Process");
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::ProcessEntry);

		std::vector<std::string_view> ArgumentViews;
		ArgumentViews.reserve(ArgumentCount > 1 ? static_cast<size_t>(ArgumentCount - 1) : 0);
		for (int Index = 1; Index < ArgumentCount; ++Index)
			ArgumentViews.emplace_back(Arguments[Index]);
		FLaunchArgumentResult Parsed = ParseLaunchArguments(ArgumentViews);
		if (!Parsed)
		{
			WriteArgumentError(*Parsed.Error);
			return Parsed.Error->ExitCode;
		}
		FLaunchRequest Request = std::move(*Parsed.Request);

		FLaunchDiagnosticsRequest& Diagnostics = Request.Diagnostics;
		ConfigureWindowsProcessCrashTestOptions(
			Diagnostics.bDisableNativeCrashDump,
			Diagnostics.bForceNativeCrashCollision,
			Diagnostics.bFaultNativeCrashWriter);
		if (Diagnostics.NativeCrashSavedRoot
			&& !PublishWindowsProcessCrashRoot(*Diagnostics.NativeCrashSavedRoot, true))
		{
			std::fprintf(stderr, "Durin: --native-crash-saved could not publish the requested crash root.\n");
			return 1;
		}
		if (IsProcessEntryCrash(Diagnostics)
			&& RunWindowsProcessCrashFixture(*Diagnostics.NativeCrashFixture))
			return 1;

		if (Request.ProcessCoordination.WaitForProcessId
			&& !FPlatformProcess::WaitForProcessExit(
				*Request.ProcessCoordination.WaitForProcessId))
		{
			std::fprintf(stderr, "Durin: --wait-for-process could not wait for the requested process.\n");
			return 1;
		}

		FLaunchStartupCommandRequest& StartupCommand = Request.Automation.StartupCommand;
		if (StartupCommand.Name)
		{
			std::string Error;
			if (!ConfigureStartupCommand(
				std::move(*StartupCommand.Name), std::move(StartupCommand.Arguments), &Error))
			{
				std::fprintf(stderr, "Durin: --startup-command could not be configured: %s\n", Error.c_str());
				return 2;
			}
			Request.Host.bSuppressWindowDisplay = true;
		}

		FEngineLoop EngineLoop(std::move(Request.Diagnostics));
		const FEngineStartupParams StartupParams = MakeStartupParams(Request.Host);
		if (!EngineLoop.PreInit(StartupParams) || !EngineLoop.Init())
		{
			const int InitResult = EngineLoop.WasInitializationCancelled() ? 0 : 1;
			EngineLoop.Exit();
			if (EngineLoop.HasLoggerStarted()) LoggerShutdown();
			return InitResult;
		}

		int ProcessResult = 0;
		uint64 CompletedTicks = 0;
		while (!IsEngineExitRequested())
		{
			EngineLoop.Tick();
			++CompletedTicks;
			if (HasPendingStartupCommand())
			{
				std::string Error;
				if (const std::optional<int> Result = DispatchStartupCommand(
						&Error, CompletedTicks >= 120))
				{
					ProcessResult = *Result;
					if (!Error.empty()) DURIN_ERROR("{}", Error);
					RequestEngineExit();
				}
			}
			if (!IsEngineExitRequested() && Request.Automation.ExitAfterTicks
				&& CompletedTicks >= *Request.Automation.ExitAfterTicks)
			{
				DURIN_INFO("Requesting automated engine exit after {} ticks.", CompletedTicks);
				RequestEngineExit();
			}
		}

		EngineLoop.Exit();
		std::string RelaunchError;
		if (!LaunchPendingEditorRelaunch(&RelaunchError))
			DURIN_ERROR("Failed to relaunch editor: {}", RelaunchError);
		if (EngineLoop.HasLoggerStarted()) LoggerShutdown();
		SetProcessCrashPhase(EProcessCrashPhase::Exited);
		return ProcessResult;
	}
}
