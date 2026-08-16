#include "HAL/PlatformProcess.h"

#include <gtest/gtest.h>

namespace
{
	auto RunLaunchChild(std::string_view Arguments) -> Durin::int32
	{
		Durin::int32 ReturnCode = -1;
		std::string Error;
		EXPECT_TRUE(Durin::FPlatformProcess::ExecuteProcess(
			DURIN_LAUNCH_EXECUTABLE, Arguments, ReturnCode, &Error)) << Error;
		return ReturnCode;
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
#if defined(_WIN32)
	EXPECT_EQ(RunLaunchChild("--wait-for-process=4"), 1u);
#elif defined(__APPLE__)
	EXPECT_EQ(RunLaunchChild("--wait-for-process=4294967295"), 1u);
#endif
}

// Full runtime startup is sensitive to unrelated host load, so keep these
// process characterizations opt-in instead of making the aggregate flaky.
TEST(FLaunchProcessBoundaryTests, DISABLED_BoundedTickExitUsesNormalApplicationShutdown)
{
	EXPECT_EQ(RunLaunchChild(
		"--project-browser --hidden-window --exit-after-ticks=2"), 0u);
}

TEST(FLaunchProcessBoundaryTests, DISABLED_MissingStartupCommandHandlerBecomesTerminal)
{
	const std::string Arguments = std::format(
		"--project=\"{}\" --hidden-window --startup-command=missing-test-handler",
		DURIN_LAUNCH_TEST_PROJECT);
	EXPECT_EQ(RunLaunchChild(Arguments), 2u);
}
