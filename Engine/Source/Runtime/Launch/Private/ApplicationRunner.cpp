#include "ApplicationProcess.h"

#include "CoreGlobals.h"
#include "Diagnostics/ProcessCrashContext.h"
#include "EngineLoop.h"
#include "HAL/PlatformProcess.h"
#include "LaunchArguments.h"
#include "Misc/Project.h"
#include "Misc/StartupCommand.h"
#include "Misc/Version.h"
#include "Profiling/Profiling.h"
#include "ProcessCrashServices.h"

namespace Durin
{
	namespace
	{
		// Restores the previously installed process handlers on every ordinary return.
		class FProcessCrashHandlerGuard
		{
		public:
			~FProcessCrashHandlerGuard() { UninstallProcessCrashHandler(); }
		};

		auto WriteArgumentError(const FLaunchArgumentError& Error) -> void
		{
			std::fprintf(stderr, "Durin: option '%s' %s.\n",
				Error.Option.c_str(), Error.Message.c_str());
		}

	}

	auto RunApplicationProcess(int ArgumentCount, char** Arguments) -> int
	{
		InitializeProcessCrashContext(
			DURIN_RUNTIME_VARIANT, DURIN_BUILD_TYPE_STRING, GetEngineVersionString());
		if (!InstallProcessCrashHandler()) return 1;
		FProcessCrashHandlerGuard CrashHandlerGuard;
		Profiling::FProcessSession ProfilingSession;
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

		FLaunchCrashTestRequest& CrashTest = Request.CrashTest;
		ConfigureProcessCrashTestOptions(
			CrashTest.bDisableNativeCrashDump,
			CrashTest.bForceNativeCrashCollision,
			CrashTest.bFaultNativeCrashWriter);
		if (CrashTest.NativeCrashSavedRoot
			&& !PublishProcessCrashRoot(*CrashTest.NativeCrashSavedRoot, true))
		{
			std::fprintf(stderr, "Durin: --native-crash-saved could not publish the requested crash root.\n");
			return 1;
		}
		if (CrashTest.NativeCrashFixture)
		{
			if (RunProcessCrashFixture(*CrashTest.NativeCrashFixture)) return 1;
			std::fprintf(stderr, "Durin: native crash fixture was not supported by this platform.\n");
			return 2;
		}

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
			if (const auto Result = ConfigureStartupCommand(
					std::move(*StartupCommand.Name), std::move(StartupCommand.Arguments)); !Result)
			{
				std::fprintf(stderr, "Durin: --startup-command could not be configured: %s\n",
					Result.error().ToString().c_str());
				return 2;
			}
		}

		FEngineLoop EngineLoop;
		Request.Host.bRecordInteractiveHistory = !Request.Automation.ExitAfterTicks && !StartupCommand.Name;
		if (!EngineLoop.PreInit(Request.Host) || !EngineLoop.Init())
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
			const auto BeforeTick = EngineLoop.GetCompletedRunningTicks();
			EngineLoop.Tick();
			const auto StartupState = EngineLoop.GetStartupState();
			if (StartupState == EHostStartupState::Failed
				&& (!DURIN_WITH_EDITOR || Request.Automation.ExitAfterTicks || StartupCommand.Name))
			{
				DURIN_ERROR("Requested host startup failed: {}", EngineLoop.GetStartupError());
				ProcessResult = 1;
				RequestEngineExit();
			}
			if (IsEngineExitRequested() || StartupState != EHostStartupState::Ready
				|| EngineLoop.GetCompletedRunningTicks() == BeforeTick) continue;
			++CompletedTicks;
			if (HasPendingStartupCommand())
			{
				const auto Result = DispatchStartupCommand(true);
				if (!Result)
				{
					ProcessResult = 2;
					DURIN_ERROR("{}", Result.error().ToString());
					RequestEngineExit();
				}
				else if (Result->has_value())
				{
					ProcessResult = **Result;
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

		if (EngineLoop.GetStartupState() != EHostStartupState::Ready && ProcessResult == 0
			&& (Request.Automation.ExitAfterTicks || StartupCommand.Name)) ProcessResult = 1;
		EngineLoop.Exit();
		std::string RelaunchError;
		const auto LaunchPendingEditorRelaunchResult = LaunchPendingEditorRelaunch();
		RelaunchError = LaunchPendingEditorRelaunchResult ? std::string{} : LaunchPendingEditorRelaunchResult.error().ToString();
		if (!LaunchPendingEditorRelaunchResult.has_value())
			DURIN_ERROR("Failed to relaunch editor: {}", RelaunchError);
		if (EngineLoop.HasLoggerStarted()) LoggerShutdown();
		return ProcessResult;
	}
}
