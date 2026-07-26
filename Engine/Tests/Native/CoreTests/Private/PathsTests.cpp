#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"

#include <gtest/gtest.h>

TEST(FPathsTests, RootAndEngineMountAreWorkspaceRelative)
{
	std::filesystem::path EngineDir = Durin::FPaths::EngineDir();
	if (EngineDir.filename().empty()) EngineDir = EngineDir.parent_path();
	const std::filesystem::path ExpectedRoot = EngineDir.parent_path().lexically_normal();

	EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(Durin::FPaths::RootDir()), ExpectedRoot));

	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	Durin::PathUtilities::InitDefaultMountPoints();
	const std::filesystem::path ResolvedEngine = Durin::FPaths::Resolve("/Engine/StaticMeshes/Test");
	EXPECT_EQ(ResolvedEngine.lexically_normal(), (EngineDir / "Content/StaticMeshes/Test").lexically_normal());
}

TEST(FPathsTests, ThirdPartyRuntimeBinariesAreSharedByBuildConfiguration)
{
	std::filesystem::path EngineDir = Durin::FPaths::EngineDir();
	if (EngineDir.filename().empty()) EngineDir = EngineDir.parent_path();
	std::filesystem::path ThirdPartyDir =
		std::filesystem::path(Durin::FPaths::EngineThirdPartyRuntimeBinariesDir()).lexically_normal();
	if (ThirdPartyDir.filename().empty()) ThirdPartyDir = ThirdPartyDir.parent_path();

	EXPECT_EQ(ThirdPartyDir.filename(), DURIN_BUILD_CONFIGURATION);
	EXPECT_EQ(ThirdPartyDir.parent_path().filename(), "ThirdParty");
	EXPECT_EQ(ThirdPartyDir.parent_path().parent_path().parent_path(), (EngineDir / "Binaries").lexically_normal());
}

TEST(FPathsTests, ExplicitProjectFileControlsProjectDirectoryAndMount)
{
	const std::filesystem::path ProjectDir = std::filesystem::path(DURIN_TEST_WORK_DIR) / "ExternalProject";
	std::filesystem::create_directories(ProjectDir / "Content");
	const std::filesystem::path ProjectFile = ProjectDir / "ExternalGame.dproject";
	{
		std::ofstream Stream(ProjectFile);
		Stream << R"({"ProjectName":"ExternalGame"})";
	}

	std::string Error;
	ASSERT_TRUE(Durin::FPaths::SetProjectFile(ProjectFile.generic_string(), &Error)) << Error;
	EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(Durin::FPaths::ProjectDir()), ProjectDir));

	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	Durin::PathUtilities::InitDefaultMountPoints();
	const std::filesystem::path Resolved = Durin::FPaths::Resolve("/Game/Levels/Test");
	EXPECT_EQ(Resolved.lexically_normal(), (ProjectDir / "Content/Levels/Test").lexically_normal());
}
