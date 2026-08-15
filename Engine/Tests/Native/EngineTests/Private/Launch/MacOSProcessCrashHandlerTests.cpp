#include "Runtime/Launch/Private/MacOS/MacOSProcessCrashHandler.h"

#include "NativeTestSupport.h"

#include <csignal>
#include <gtest/gtest.h>

namespace
{
	auto CurrentHandler(int Signal) -> void (*)(int)
	{
		struct sigaction Action{};
		EXPECT_EQ(sigaction(Signal, nullptr, &Action), 0);
		return Action.sa_handler;
	}
}

TEST(FMacOSProcessCrashHandlerTests, RepeatedInstallAndUninstallRestorePriorHandlers)
{
	const auto PreviousSegmentation = CurrentHandler(SIGSEGV);
	const auto PreviousAbort = CurrentHandler(SIGABRT);

	ASSERT_TRUE(Durin::InstallMacOSProcessCrashHandler());
	EXPECT_TRUE(Durin::InstallMacOSProcessCrashHandler());
	EXPECT_NE(CurrentHandler(SIGSEGV), PreviousSegmentation);
	EXPECT_NE(CurrentHandler(SIGABRT), PreviousAbort);

	Durin::UninstallMacOSProcessCrashHandler();
	EXPECT_EQ(CurrentHandler(SIGSEGV), PreviousSegmentation);
	EXPECT_EQ(CurrentHandler(SIGABRT), PreviousAbort);
	Durin::UninstallMacOSProcessCrashHandler();

	ASSERT_TRUE(Durin::InstallMacOSProcessCrashHandler());
	Durin::UninstallMacOSProcessCrashHandler();
	EXPECT_EQ(CurrentHandler(SIGSEGV), PreviousSegmentation);
	EXPECT_EQ(CurrentHandler(SIGABRT), PreviousAbort);
}

TEST(FMacOSProcessCrashHandlerTests, RootPublicationRequiresInstallAndAbsoluteSavedPath)
{
	Durin::UninstallMacOSProcessCrashHandler();
	EXPECT_FALSE(Durin::PublishMacOSProcessCrashRoot("/tmp/durin-not-installed"));
	ASSERT_TRUE(Durin::InstallMacOSProcessCrashHandler());
	EXPECT_FALSE(Durin::PublishMacOSProcessCrashRoot("relative/path"));
	EXPECT_TRUE(Durin::PublishMacOSProcessCrashRoot(
		Durin::Testing::GetTestWorkDirectory().string(), true));
	EXPECT_TRUE(Durin::PublishMacOSProcessCrashRoot("relative/path", false));
	Durin::UninstallMacOSProcessCrashHandler();
}

TEST(FMacOSProcessCrashHandlerTests, UnsupportedFixtureReturnsWithoutFaulting)
{
	EXPECT_FALSE(Durin::RunMacOSProcessCrashFixture(""));
	EXPECT_FALSE(Durin::RunMacOSProcessCrashFixture("unknown"));
}
