#include "LaunchAPI.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "LaunchEngineLoop.h"
#include "Misc/Project.h"

using namespace Durin;

int LAUNCH_API main(int argc, char** argv)
{
	constexpr std::string_view WaitForProcessPrefix = "--wait-for-process=";
	constexpr std::string_view ExitAfterTicksPrefix = "--exit-after-ticks=";
	constexpr std::string_view ProjectPrefix = "--project=";
	std::optional<uint32> WaitForProcessId;
	std::optional<uint64> ExitAfterTicks;
	std::optional<std::string_view> ProjectEqualsArgument;
	std::optional<std::string_view> ProjectSeparateArgument;
	FEngineStartupParams StartupParams;
	for (int Index = 1; Index < argc; ++Index)
	{
		const std::string_view Argument = argv[Index];
		if (Argument.starts_with(WaitForProcessPrefix))
		{
			uint32 ProcessId = 0;
			const std::string_view ProcessIdText = Argument.substr(WaitForProcessPrefix.size());
			const auto [End, Error] = std::from_chars(ProcessIdText.data(), ProcessIdText.data() + ProcessIdText.size(), ProcessId);
			if (Error != std::errc{} || End != ProcessIdText.data() + ProcessIdText.size()) return 1;
			WaitForProcessId = ProcessId;
		}
		else if (Argument.starts_with(ExitAfterTicksPrefix))
		{
			uint64 TickCount = 0;
			const std::string_view TickCountText = Argument.substr(ExitAfterTicksPrefix.size());
			const auto [End, Error] = std::from_chars(TickCountText.data(), TickCountText.data() + TickCountText.size(), TickCount);
			if (Error != std::errc{} || End != TickCountText.data() + TickCountText.size() || TickCount == 0) return 1;
			ExitAfterTicks = TickCount;
		}
		else if (Argument == "--hidden-window")
		{
			StartupParams.bSuppressWindowDisplay = true;
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
	}
	if (WaitForProcessId && !FPlatformProcess::WaitForProcessExit(*WaitForProcessId)) return 1;

	if (ProjectEqualsArgument && !ProjectEqualsArgument->empty())
		StartupParams.Project.RequestedProjectFile = *ProjectEqualsArgument;
	else if (ProjectSeparateArgument)
		StartupParams.Project.RequestedProjectFile = *ProjectSeparateArgument;
	GEngineLoop.PreInit(StartupParams);
	GEngineLoop.Init();

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
	return 0;
}
