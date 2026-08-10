#include "NativeTestSupport.h"
#include "Runtime/Launch/Private/LaunchRuntimeStorage.h"

#include <gtest/gtest.h>

namespace
{
	auto MakePaths(const std::filesystem::path& Root)
		-> Durin::FLaunchRuntimeStoragePaths
	{
		return {
			.LaunchDirectory = Root / "Launch",
			.SavedDirectory = Root / "Launch" / "Saved",
			.ConfigDirectory = Root / "Launch" / "Saved" / "Config",
			.LogDirectory = Root / "Launch" / "Saved" / "Logs"};
	}

	auto WriteText(const std::filesystem::path& Path, std::string_view Text) -> void
	{
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream << Text;
	}

	auto ReadText(const std::filesystem::path& Path) -> std::string
	{
		std::ifstream Stream(Path, std::ios::binary);
		return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	}
}

TEST(FLaunchRuntimeStorageTests, NoOpPreparationSelectsLegacyPathWithoutWarnings)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageNoOp");
	const Durin::FLaunchRuntimeStoragePaths Paths = MakePaths(Root);
	const Durin::FLaunchRuntimeStorageResult Result =
		Durin::PrepareLaunchRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, Paths.LaunchDirectory / "DurinEditor.yaml");
	EXPECT_TRUE(std::filesystem::is_directory(Paths.ConfigDirectory));
	EXPECT_TRUE(std::filesystem::is_directory(Paths.LogDirectory));
}

TEST(FLaunchRuntimeStorageTests, RenamesLegacyConfigurationAndIsIdempotent)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageRename");
	const Durin::FLaunchRuntimeStoragePaths Paths = MakePaths(Root);
	const std::filesystem::path Legacy = Paths.LaunchDirectory / "DurinEditor.yaml";
	const std::filesystem::path Saved = Paths.ConfigDirectory / "DurinEditor.yaml";
	WriteText(Legacy, "legacy");
	const Durin::FLaunchRuntimeStorageResult First =
		Durin::PrepareLaunchRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(First.Warnings.empty());
	EXPECT_EQ(First.AppConfigPath, Saved);
	EXPECT_FALSE(std::filesystem::exists(Legacy));
	EXPECT_EQ(ReadText(Saved), "legacy");
	const Durin::FLaunchRuntimeStorageResult Second =
		Durin::PrepareLaunchRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Second.Warnings.empty());
	EXPECT_EQ(Second.AppConfigPath, Saved);
}

TEST(FLaunchRuntimeStorageTests, CopiesAndRemovesWhenLegacyRenameFails)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageFallback");
	const Durin::FLaunchRuntimeStoragePaths Paths = MakePaths(Root);
	const std::filesystem::path Legacy = Paths.LaunchDirectory / "DurinEditor.yaml";
	const std::filesystem::path Saved = Paths.ConfigDirectory / "DurinEditor.yaml";
	WriteText(Legacy, "fallback");
	const Durin::FLaunchRuntimeStorageResult Result =
		Durin::PrepareLaunchRuntimeStorage(
			Paths, "DurinEditor.yaml", {.bForceLegacyFileRenameFailure = true});
	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, Saved);
	EXPECT_FALSE(std::filesystem::exists(Legacy));
	EXPECT_EQ(ReadText(Saved), "fallback");
}

TEST(FLaunchRuntimeStorageTests, ExistingDestinationWinsWithoutMutation)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageExisting");
	const Durin::FLaunchRuntimeStoragePaths Paths = MakePaths(Root);
	const std::filesystem::path Legacy = Paths.LaunchDirectory / "DurinEditor.yaml";
	const std::filesystem::path Saved = Paths.ConfigDirectory / "DurinEditor.yaml";
	WriteText(Legacy, "legacy");
	WriteText(Saved, "saved");
	const Durin::FLaunchRuntimeStorageResult Result =
		Durin::PrepareLaunchRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, Saved);
	EXPECT_EQ(ReadText(Legacy), "legacy");
	EXPECT_EQ(ReadText(Saved), "saved");
}

TEST(FLaunchRuntimeStorageTests, FilesystemFailureIsPassLocal)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageFailure");
	const Durin::FLaunchRuntimeStoragePaths Paths = MakePaths(Root);
	WriteText(Paths.SavedDirectory, "not a directory");
	const Durin::FLaunchRuntimeStorageResult Failed =
		Durin::PrepareLaunchRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_FALSE(Failed.Warnings.empty());
	EXPECT_EQ(Failed.AppConfigPath, Paths.LaunchDirectory / "DurinEditor.yaml");
	ASSERT_TRUE(std::filesystem::remove(Paths.SavedDirectory));
	const Durin::FLaunchRuntimeStorageResult Recovered =
		Durin::PrepareLaunchRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Recovered.Warnings.empty());
}
