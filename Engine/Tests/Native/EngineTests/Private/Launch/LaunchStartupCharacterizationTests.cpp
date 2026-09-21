#include "HAL/PlatformProcess.h"

#include <gtest/gtest.h>

namespace
{
	auto RunLaunchChild(std::string_view Arguments) -> int32
	{
		int32 ReturnCode = -1;
		std::string Error;
		EXPECT_TRUE(Durin::FPlatformProcess::ExecuteProcess(
			DURIN_LAUNCH_EXECUTABLE, Arguments, ReturnCode, &Error)) << Error;
		return ReturnCode;
	}
}

// Full startup characterizations are selected explicitly, outside routine integration coverage.
TEST(FLaunchStartupCharacterizationTests, BoundedTickExitUsesNormalApplicationShutdown)
{
	EXPECT_EQ(RunLaunchChild(
		"--project-browser --hidden-window --exit-after-ticks=2"), 0u);
}

TEST(FLaunchStartupCharacterizationTests, MissingStartupCommandHandlerBecomesTerminal)
{
	const std::string Arguments = std::format(
		"--project=\"{}\" --hidden-window --startup-command=missing-test-handler",
		DURIN_LAUNCH_TEST_PROJECT);
	EXPECT_EQ(RunLaunchChild(Arguments), 2u);
}
