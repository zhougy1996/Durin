#include <gtest/gtest.h>

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"

#if PLATFORM_WINDOWS
	#include <process.h>
#endif

TEST(FProjectTests, PlatformProcessReportsCurrentProcessId)
{
#if PLATFORM_WINDOWS
	EXPECT_EQ(Durin::FPlatformProcess::CurrentProcessId(), static_cast<Durin::uint32>(::_getpid()));
#endif
}

TEST(FProjectTests, LoadsExplicitProjectFile)
{
	const std::string ProjectFile = Durin::FPaths::RootDir() + "SandBox/SandBox.dproject";
	const std::array<std::string, 1> OwnedArguments{std::format("--project={}", ProjectFile)};
	const std::array<std::string_view, 1> Arguments{OwnedArguments[0]};
	std::string Error;
	ASSERT_TRUE(Durin::InitializeCurrentProject(Arguments, &Error)) << Error;
	ASSERT_TRUE(Durin::HasCurrentProject());
	EXPECT_EQ(Durin::GetCurrentProject()->Name, "SandBox");
	EXPECT_EQ(Durin::GetCurrentProject()->MountRoot, "/SandBox/");
	EXPECT_EQ(Durin::FPaths::ProjectDir(), Durin::GetCurrentProject()->ProjectDir);
}

TEST(FProjectTests, RejectsMissingProject)
{
	const std::array<std::string_view, 1> Arguments{"--project=Missing.dproject"};
	std::string Error;
	EXPECT_FALSE(Durin::InitializeCurrentProject(Arguments, &Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Durin::HasCurrentProject());
}

TEST(FProjectTests, ExplicitBrowserSkipsRecentProject)
{
	const std::array<std::string_view, 1> Arguments{"--project-browser"};
	std::string Error;
	EXPECT_TRUE(Durin::InitializeCurrentProject(Arguments, &Error));
	EXPECT_FALSE(Durin::HasCurrentProject());
}
