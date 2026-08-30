#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
#ifdef _WIN32
	const std::filesystem::path Root = "C:/Workspace/Content";
#else
	const std::filesystem::path Root = "/workspace/Content";
#endif
}

TEST(FLexicalPathTests, ClassifiesEqualityDescendantsAndEscapingPaths)
{
	std::filesystem::path Relative;
	EXPECT_TRUE(Durin::FPaths::TryMakeLexicalRelativePath(Root, Root, Relative));
	EXPECT_TRUE(Relative.empty());
	EXPECT_FALSE(Durin::FPaths::IsLexicalDescendantPath(Root, Root, true));
	EXPECT_TRUE(Durin::FPaths::IsLexicalDescendantPath(Root / "Materials", Root, false));
	EXPECT_TRUE(Durin::FPaths::IsLexicalDescendantPath(Root / "Materials/Instances", Root, true));
	EXPECT_FALSE(Durin::FPaths::IsLexicalDescendantPath(Root / "Materials/Instances", Root, false));
	EXPECT_FALSE(Durin::FPaths::IsLexicalDescendantPath(
		std::filesystem::path(Root.generic_string() + "Extra") / "Asset", Root, true));
	EXPECT_FALSE(Durin::FPaths::IsLexicalDescendantPath(Root / "Materials/../Textures", Root, true));
#ifdef _WIN32
	const std::filesystem::path OtherRoot = "D:/Workspace/Content/Asset";
#else
	const std::filesystem::path OtherRoot = "/other/Content/Asset";
#endif
	EXPECT_FALSE(Durin::FPaths::IsLexicalDescendantPath(OtherRoot, Root, true));
}

TEST(FLexicalPathTests, NormalizesTrailingSeparatorsAndPlatformComponentCase)
{
	std::filesystem::path Relative;
	const std::filesystem::path RootWithSeparator(Root.generic_string() + "/");
	EXPECT_TRUE(Durin::FPaths::TryMakeLexicalRelativePath(Root / "Materials/", RootWithSeparator, Relative));
	EXPECT_EQ(Relative.generic_string(), "Materials");

#ifdef _WIN32
	ASSERT_TRUE(Durin::FPaths::TryMakeLexicalRelativePath(
		"c:/WORKSPACE/content/Materials", Root, Relative));
	EXPECT_EQ(Relative.generic_string(), "Materials");
#endif
}

TEST(FLexicalPathTests, ResolvesContainedPathsAndNonexistentTails)
{
	const std::filesystem::path PhysicalRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "ResolvedContainment").lexically_normal();
	std::filesystem::create_directories(PhysicalRoot);
	std::filesystem::path Resolved;
	std::error_code Error;
	EXPECT_TRUE(Durin::FPaths::TryResolveContainedPath(
		PhysicalRoot / "Missing/Asset.bin", PhysicalRoot, Resolved, Error));
	EXPECT_FALSE(Error);
	EXPECT_EQ(Resolved, PhysicalRoot / "Missing/Asset.bin");
	EXPECT_FALSE(Durin::FPaths::TryResolveContainedPath(
		PhysicalRoot.parent_path() / "Outside.bin", PhysicalRoot, Resolved, Error));
	EXPECT_FALSE(Error);
	EXPECT_TRUE(Resolved.empty());
	EXPECT_FALSE(Durin::FPaths::TryResolveContainedPath(
		"Relative/Asset.bin", PhysicalRoot, Resolved, Error));
	EXPECT_EQ(Error, std::make_error_code(std::errc::invalid_argument));
}

TEST(FLexicalPathTests, RejectsSymbolicLinkEscapeWhenSupported)
{
	const std::filesystem::path WorkRoot = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "ResolvedSymlinkContainment").lexically_normal();
	const std::filesystem::path PhysicalRoot = WorkRoot / "Root";
	const std::filesystem::path Outside = WorkRoot / "Outside";
	std::filesystem::create_directories(PhysicalRoot);
	std::filesystem::create_directories(Outside);
	std::error_code Error;
	std::filesystem::create_directory_symlink(Outside, PhysicalRoot / "Escape", Error);
	if (Error) GTEST_SKIP() << "Directory symbolic links are unavailable: " << Error.message();

	std::filesystem::path Resolved;
	EXPECT_FALSE(Durin::FPaths::TryResolveContainedPath(
		PhysicalRoot / "Escape/Asset.bin", PhysicalRoot, Resolved, Error));
	EXPECT_FALSE(Error);
	EXPECT_TRUE(Resolved.empty());
}
