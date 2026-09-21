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
