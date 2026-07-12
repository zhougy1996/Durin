#include "LaunchAPI.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "LaunchEngineLoop.h"
#include "Misc/Project.h"

using namespace Durin;

int LAUNCH_API main(int argc, char** argv)
{
	constexpr std::string_view WaitForProcessPrefix = "--wait-for-process=";
	for (int Index = 1; Index < argc; ++Index)
	{
		const std::string_view Argument = argv[Index];
		if (!Argument.starts_with(WaitForProcessPrefix)) continue;
		uint32 ProcessId = 0;
		const std::string_view ProcessIdText = Argument.substr(WaitForProcessPrefix.size());
		const auto [End, Error] = std::from_chars(ProcessIdText.data(), ProcessIdText.data() + ProcessIdText.size(), ProcessId);
		if (Error != std::errc{} || End != ProcessIdText.data() + ProcessIdText.size()) return 1;
		if (!FPlatformProcess::WaitForProcessExit(ProcessId)) return 1;
		break;
	}

	std::vector<std::string_view> Arguments;
	for (int Index = 1; Index < argc; ++Index) Arguments.emplace_back(argv[Index]);
	GEngineLoop.PreInit(Arguments);
	GEngineLoop.Init();

	while (!IsEngineExitRequested())
	{
		GEngineLoop.Tick();
	}
	GEngineLoop.Exit();
	std::string RelaunchError;
	if (!LaunchPendingEditorRelaunch(&RelaunchError)) DURIN_ERROR("Failed to relaunch editor: {}", RelaunchError);
	return 0;
}
