#include "HAL/PlatformProcess.h"

#include <gtest/gtest.h>

namespace
{
	auto RunLaunchChild(std::initializer_list<std::string> Arguments) -> int32
	{
		int32 ReturnCode = -1;
		std::string Error;
		const auto ExecuteProcessResult = Durin::FPlatformProcess::ExecuteProcess(DURIN_LAUNCH_EXECUTABLE, Durin::FProcessLaunchOptions{.Arguments = Arguments});
		Error = ExecuteProcessResult ? std::string{} : ExecuteProcessResult.error().ToString();
		if (ExecuteProcessResult) ReturnCode = *ExecuteProcessResult;
		EXPECT_TRUE(ExecuteProcessResult.has_value()) << Error;
		return ReturnCode;
	}
}

// Full startup characterizations are selected explicitly, outside routine integration coverage.
TEST(FLaunchStartupCharacterizationTests, BoundedTickExitUsesNormalApplicationShutdown)
{
	EXPECT_EQ(RunLaunchChild(
		{"--project-browser", "--hidden-window", "--exit-after-ticks=2"}), 0u);
}

TEST(FLaunchStartupCharacterizationTests, MissingStartupCommandHandlerBecomesTerminal)
{
	EXPECT_EQ(RunLaunchChild({std::format("--project={}", DURIN_LAUNCH_TEST_PROJECT),
		"--hidden-window", "--startup-command=missing-test-handler"}), 2u);
}
