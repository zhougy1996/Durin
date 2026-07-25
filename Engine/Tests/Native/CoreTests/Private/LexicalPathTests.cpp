#include "Misc/LexicalPath.h"

#include <gtest/gtest.h>

namespace
{
#ifdef _WIN32
	const std::filesystem::path Root = "C:/Workspace/Content";
#else
	const std::filesystem::path Root = "/workspace/Content";
#endif
}

TEST(FLexicalPathTests, TreatsEqualityAsContainedButNotDescendant)
{
	std::filesystem::path Relative;
	EXPECT_TRUE(Durin::PathUtilities::TryMakeLexicalRelativePath(Root, Root, Relative));
	EXPECT_TRUE(Relative.empty());
	EXPECT_FALSE(Durin::PathUtilities::IsLexicalDescendantPath(Root, Root, true));
}

TEST(FLexicalPathTests, DistinguishesDirectAndRecursiveDescendants)
{
	EXPECT_TRUE(Durin::PathUtilities::IsLexicalDescendantPath(Root / "Materials", Root, false));
	EXPECT_TRUE(Durin::PathUtilities::IsLexicalDescendantPath(Root / "Materials/Instances", Root, true));
	EXPECT_FALSE(Durin::PathUtilities::IsLexicalDescendantPath(Root / "Materials/Instances", Root, false));
}

TEST(FLexicalPathTests, RejectsSharedPrefixesAndParentComponents)
{
	EXPECT_FALSE(Durin::PathUtilities::IsLexicalDescendantPath(
		std::filesystem::path(Root.generic_string() + "Extra") / "Asset", Root, true));
	EXPECT_FALSE(Durin::PathUtilities::IsLexicalDescendantPath(Root / "Materials/../Textures", Root, true));
}

TEST(FLexicalPathTests, IgnoresTrailingSeparators)
{
	std::filesystem::path Relative;
	const std::filesystem::path RootWithSeparator(Root.generic_string() + "/");
	EXPECT_TRUE(Durin::PathUtilities::TryMakeLexicalRelativePath(Root / "Materials/", RootWithSeparator, Relative));
	EXPECT_EQ(Relative.generic_string(), "Materials");
}

TEST(FLexicalPathTests, RejectsDifferentRoots)
{
#ifdef _WIN32
	const std::filesystem::path OtherRoot = "D:/Workspace/Content/Asset";
#else
	const std::filesystem::path OtherRoot = "/other/Content/Asset";
#endif
	EXPECT_FALSE(Durin::PathUtilities::IsLexicalDescendantPath(OtherRoot, Root, true));
}

#ifdef _WIN32
TEST(FLexicalPathTests, ComparesWindowsComponentsWithoutCaseSensitivity)
{
	std::filesystem::path Relative;
	ASSERT_TRUE(Durin::PathUtilities::TryMakeLexicalRelativePath(
		"c:/WORKSPACE/content/Materials", Root, Relative));
	EXPECT_EQ(Relative.generic_string(), "Materials");
}
#endif
