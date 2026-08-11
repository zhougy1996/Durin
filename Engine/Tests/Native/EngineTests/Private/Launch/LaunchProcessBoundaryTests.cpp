#include "Misc/StringConvert.h"

#include <gtest/gtest.h>

namespace
{
	constexpr DWORD LaunchChildTimeoutMilliseconds = 30000;

	auto RunLaunchChild(std::string_view Arguments) -> DWORD
	{
		const std::string Command = std::format(
			"\"{}\" {}", DURIN_LAUNCH_EXECUTABLE, Arguments);
		std::wstring WideCommand = Durin::StringUtils::Utf8ToWide(Command);
		STARTUPINFOW StartupInfo{};
		StartupInfo.cb = sizeof(StartupInfo);
		PROCESS_INFORMATION ProcessInfo{};
		EXPECT_TRUE(CreateProcessW(nullptr, WideCommand.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &StartupInfo, &ProcessInfo));
		if (ProcessInfo.hProcess == nullptr) return std::numeric_limits<DWORD>::max();
		CloseHandle(ProcessInfo.hThread);
		const DWORD Wait = WaitForSingleObject(ProcessInfo.hProcess, LaunchChildTimeoutMilliseconds);
		EXPECT_EQ(Wait, WAIT_OBJECT_0);
		if (Wait != WAIT_OBJECT_0)
		{
			TerminateProcess(ProcessInfo.hProcess, 0xEEu);
			WaitForSingleObject(ProcessInfo.hProcess, 5000);
		}
		DWORD ExitCode = std::numeric_limits<DWORD>::max();
		GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
		CloseHandle(ProcessInfo.hProcess);
		return ExitCode;
	}
}

TEST(FLaunchProcessBoundaryTests, RejectsParseAndDuplicateFailuresWithCommandLineStatus)
{
	EXPECT_EQ(RunLaunchChild("--unknown-launch-option"), 2u);
	EXPECT_EQ(RunLaunchChild("--project=One --project=Two"), 2u);
	EXPECT_EQ(RunLaunchChild("--exit-after-ticks=0"), 2u);
}

TEST(FLaunchProcessBoundaryTests, ReportsWaitFailureAsRuntimeFailure)
{
	EXPECT_EQ(RunLaunchChild("--wait-for-process=4"), 1u);
}

TEST(FLaunchProcessBoundaryTests, BoundedTickExitUsesNormalApplicationShutdown)
{
	EXPECT_EQ(RunLaunchChild(
		"--project-browser --hidden-window --exit-after-ticks=2"), 0u);
}

TEST(FLaunchProcessBoundaryTests, MissingStartupCommandHandlerBecomesTerminal)
{
	const std::string Arguments = std::format(
		"--project=\"{}\" --hidden-window --startup-command=missing-test-handler",
		DURIN_LAUNCH_TEST_PROJECT);
	EXPECT_EQ(RunLaunchChild(Arguments), 2u);
}
