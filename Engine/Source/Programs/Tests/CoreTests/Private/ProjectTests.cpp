#include <gtest/gtest.h>

#include "Misc/Paths.h"
#include "Misc/Project.h"

TEST(FProjectTests, LoadsRegisteredWorkspaceProject)
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

TEST(FProjectTests, RejectsUnregisteredProject)
{
	const std::array<std::string_view, 1> Arguments{"--project=Unregistered.dproject"};
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
	EXPECT_FALSE(Durin::GetRegisteredProjects().empty());
}
