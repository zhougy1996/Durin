#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"

#include <gtest/gtest.h>

TEST(FPathsTests, RootAndProjectMountsAreWorkspaceRelative)
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
	const std::filesystem::path ResolvedSandBox = Durin::FPaths::Resolve("/SandBox/StaticMeshes/Test");
	EXPECT_EQ(ResolvedSandBox.lexically_normal(), (ExpectedRoot / "SandBox/Content/StaticMeshes/Test").lexically_normal());
}
