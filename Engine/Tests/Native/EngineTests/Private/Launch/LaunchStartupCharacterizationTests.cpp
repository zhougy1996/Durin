#include "HAL/PlatformProcess.h"

#include <gtest/gtest.h>

namespace
{
	auto RunLaunchChild(std::initializer_list<std::string> Arguments) -> int32
	{
		const auto Result = Durin::FPlatformProcess::ExecuteProcess(
			DURIN_LAUNCH_EXECUTABLE, Durin::FProcessLaunchOptions{.Arguments = Arguments});
		if (!Result) ADD_FAILURE() << Result.error().ToString();
		return Result.value_or(-1);
	}
}

// Full startup characterizations are selected explicitly, outside routine integration coverage.
TEST(FLaunchStartupCharacterizationTests, BoundedTickExitUsesNormalApplicationShutdown)
{
	EXPECT_EQ(RunLaunchChild(
		{"--project-browser", "--exit-after-ticks=2"}), 0u);
}

TEST(FLaunchStartupCharacterizationTests, MissingStartupCommandHandlerBecomesTerminal)
{
	EXPECT_EQ(RunLaunchChild({std::format("--project={}", DURIN_LAUNCH_TEST_PROJECT),
		"--startup-command=missing-test-handler"}), 2u);
}
