#include "HAL/PlatformProcess.h"
#include "Misc/Project.h"
#include "NativeTestSupport.h"

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

TEST(FLaunchProcessBoundaryTests, RejectsParseAndDuplicateFailuresWithCommandLineStatus)
{
	EXPECT_EQ(RunLaunchChild({"--unknown-launch-option"}), 2u);
	EXPECT_EQ(RunLaunchChild({"--project=One", "--project=Two"}), 2u);
	EXPECT_EQ(RunLaunchChild({"--exit-after-ticks=0"}), 2u);
}

TEST(FLaunchProcessBoundaryTests, ReportsWaitFailureAsRuntimeFailure)
{
#if defined(_WIN32)
	EXPECT_EQ(RunLaunchChild({"--wait-for-process=4"}), 1u);
#elif defined(__APPLE__)
	EXPECT_EQ(RunLaunchChild({"--wait-for-process=4294967295"}), 1u);
#endif
}

#if DURIN_WITH_EDITOR
TEST(FLaunchProcessBoundaryTests, OccupiedProjectExitsWithoutFallingBackToInteractiveBrowser)
{
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "OccupiedProject";
	std::filesystem::create_directories(Root);
	const auto Project = Root / "OccupiedProject.dproject";
	{ std::ofstream File(Project); File << R"({"ProjectName":"OccupiedProject","BaseModules":[]})"; }
	ASSERT_TRUE(Durin::InitializeCurrentProject({.RequestedProjectFile = Project.generic_string(),
		.bRecordInteractiveHistory = false}));
	ASSERT_TRUE(Durin::AcquireProjectEditOwnership());
	struct FOwnershipGuard
	{
		~FOwnershipGuard() { Durin::ReleaseProjectEditOwnership(); }
	} OwnershipGuard;
	// No automated exit: falling back to the interactive browser would leave the child running.
	EXPECT_EQ(RunLaunchChild({"--project=" + Project.generic_string()}), 1);
}
#endif
