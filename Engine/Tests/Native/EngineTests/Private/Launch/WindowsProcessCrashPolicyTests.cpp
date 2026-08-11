#include "NativeTestSupport.h"
#include "Runtime/Launch/Private/Windows/WindowsProcessCrashPolicy.h"

#include <gtest/gtest.h>

namespace
{
	auto MakePolicyDirectory(std::string_view Name) -> std::filesystem::path
	{
		const std::filesystem::path Directory = Durin::Testing::GetTestWorkDirectory() / "CrashPolicy" / Name;
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Directory, Error);
		std::filesystem::create_directories(Directory);
		return Directory;
	}

	auto AddCrashDirectory(
		const std::filesystem::path& Root,
		std::string_view Name,
		bool bComplete,
		Durin::uint32 AgeDays) -> std::filesystem::path
	{
		const std::filesystem::path Directory = Root / Name;
		std::filesystem::create_directories(Directory);
		if (bComplete)
		{
			std::ofstream Marker(Directory / "Complete.marker", std::ios::binary);
			Marker << "CrashContextVersion=1\n";
		}
		std::filesystem::last_write_time(
			Directory,
			std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * AgeDays));
		return Directory;
	}
}

TEST(FWindowsProcessCrashPolicyTests, RequiresAbsoluteTraversalFreeSavedDirectory)
{
	EXPECT_FALSE(Durin::IsValidWindowsProcessCrashSavedDirectory({}));
	EXPECT_FALSE(Durin::IsValidWindowsProcessCrashSavedDirectory("relative/Saved"));
	EXPECT_FALSE(Durin::IsValidWindowsProcessCrashSavedDirectory("D:/Runtime/../Other"));
	EXPECT_TRUE(Durin::IsValidWindowsProcessCrashSavedDirectory("D:/Runtime/Saved"));
}

TEST(FWindowsProcessCrashPolicyTests, MapsSyntheticAccessViolationParameters)
{
	EXPECT_STREQ(Durin::WindowsAccessViolationOperationName(0), "Read");
	EXPECT_STREQ(Durin::WindowsAccessViolationOperationName(1), "Write");
	EXPECT_STREQ(Durin::WindowsAccessViolationOperationName(8), "Execute");
	EXPECT_STREQ(Durin::WindowsAccessViolationOperationName(7), "Unavailable");
}

TEST(FWindowsProcessCrashPolicyTests, AppliesAgeCountAndPartialRetentionWithoutFollowingLinks)
{
	const std::filesystem::path Root = MakePolicyDirectory("Retention");
	const std::filesystem::path Newest = AddCrashDirectory(Root, "complete-newest", true, 0);
	const std::filesystem::path Middle = AddCrashDirectory(Root, "complete-middle", true, 1);
	const std::filesystem::path CountExpired = AddCrashDirectory(Root, "complete-count-expired", true, 2);
	const std::filesystem::path AgeExpired = AddCrashDirectory(Root, "complete-age-expired", true, 40);
	const std::filesystem::path PartialFresh = AddCrashDirectory(Root, "partial-fresh", false, 1);
	const std::filesystem::path PartialExpired = AddCrashDirectory(Root, "partial-expired", false, 8);

	const Durin::FWindowsProcessCrashRetentionResult Result =
		Durin::ApplyWindowsProcessCrashRetention(Root, 2, 30, 7);
	EXPECT_EQ(Result.RetainedComplete, 2u);
	EXPECT_EQ(Result.RemovedComplete, 2u);
	EXPECT_EQ(Result.RemovedPartial, 1u);
	EXPECT_TRUE(std::filesystem::is_directory(Newest));
	EXPECT_TRUE(std::filesystem::is_directory(Middle));
	EXPECT_FALSE(std::filesystem::exists(CountExpired));
	EXPECT_FALSE(std::filesystem::exists(AgeExpired));
	EXPECT_TRUE(std::filesystem::is_directory(PartialFresh));
	EXPECT_FALSE(std::filesystem::exists(PartialExpired));

	const std::filesystem::path Outside = MakePolicyDirectory("OutsideLinkTarget");
	const std::filesystem::path Link = Root / "directory-link";
	std::error_code LinkError;
	std::filesystem::create_directory_symlink(Outside, Link, LinkError);
	if (!LinkError)
	{
		Durin::ApplyWindowsProcessCrashRetention(Root, 0, 0, 0);
		EXPECT_TRUE(std::filesystem::is_directory(Outside));
	}
}
