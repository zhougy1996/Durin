#include "HAL/PlatformProcess.h"
#include "NativeTestSupport.h"

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

TEST(FLaunchStartupCharacterizationTests, MissingExplicitProjectDoesNotSucceedAsBrowser)
{
	const auto Missing = Durin::Testing::GetTestWorkDirectory() / "MissingProject" / "Missing.dproject";
	EXPECT_EQ(RunLaunchChild({"--project=" + Missing.generic_string(), "--exit-after-ticks=1"}), 1u);
}

TEST(FLaunchStartupCharacterizationTests, InvalidConfiguredLevelFailsInsteadOfOpeningEmptyDocument)
{
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "InvalidDefaultLevel";
	std::filesystem::create_directories(Root / "Configs");
	const auto Project = Root / "InvalidDefaultLevel.dproject";
	{ std::ofstream File(Project); File << R"({"ProjectName":"InvalidDefaultLevel","BaseModules":[]})"; }
	{ std::ofstream File(Root / "Configs" / "Project.yaml"); File << "Game:\n  DefaultLevel: /Game/Levels/Missing\n"; }
	EXPECT_EQ(RunLaunchChild({"--project=" + Project.generic_string(), "--exit-after-ticks=1"}), 1u);
}

TEST(FLaunchStartupCharacterizationTests, AutomatedLaunchPreservesInteractiveHistory)
{
	const auto History = std::filesystem::path(DURIN_LAUNCH_EXECUTABLE).parent_path()
		/ "Saved" / "Configs" / "ProjectHistory.yaml";
	auto Read = [&] {
		std::ifstream File(History);
		return std::string(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
	};
	const auto Before = Read();
	EXPECT_EQ(RunLaunchChild({std::format("--project={}", DURIN_LAUNCH_TEST_PROJECT), "--exit-after-ticks=2"}), 0u);
	if (!Before.empty()) EXPECT_EQ(Read(), Before);
}

TEST(FLaunchStartupCharacterizationTests, RequiredProjectModuleFailureBecomesTerminal)
{
	const auto Root = Durin::Testing::GetTestWorkDirectory() / "MissingNativeModule";
	std::filesystem::create_directories(Root);
	const auto Project = Root / "MissingNativeModule.dproject";
	{ std::ofstream File(Project); File << R"({"ProjectName":"MissingNativeModule","BaseModules":["StartupModuleThatDoesNotExist"]})"; }
	EXPECT_EQ(RunLaunchChild({"--project=" + Project.generic_string(), "--exit-after-ticks=1"}), 1u);
}
