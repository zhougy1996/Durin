#include "NativeTestSupport.h"
#include "LaunchContracts/RuntimeStorage.h"

#include <gtest/gtest.h>

namespace
{
	auto MakePaths(const std::filesystem::path& Root)
		-> Durin::FRuntimeStoragePaths
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

TEST(FRuntimeStorageTests, NoOpPreparationSelectsLegacyPathWithoutWarnings)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageNoOp");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	const Durin::FRuntimeStoragePreparationResult Result =
		Durin::PrepareRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, Paths.LaunchDirectory / "DurinEditor.yaml");
	EXPECT_TRUE(std::filesystem::is_directory(Paths.ConfigDirectory));
	EXPECT_TRUE(std::filesystem::is_directory(Paths.LogDirectory));
}

TEST(FRuntimeStorageTests, RenamesLegacyConfigurationAndIsIdempotent)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageRename");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	const std::filesystem::path Legacy = Paths.LaunchDirectory / "DurinEditor.yaml";
	const std::filesystem::path Saved = Paths.ConfigDirectory / "DurinEditor.yaml";
	WriteText(Legacy, "legacy");
	const Durin::FRuntimeStoragePreparationResult First =
		Durin::PrepareRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(First.Warnings.empty());
	EXPECT_EQ(First.AppConfigPath, Saved);
	EXPECT_FALSE(std::filesystem::exists(Legacy));
	EXPECT_EQ(ReadText(Saved), "legacy");
	const Durin::FRuntimeStoragePreparationResult Second =
		Durin::PrepareRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Second.Warnings.empty());
	EXPECT_EQ(Second.AppConfigPath, Saved);
}

TEST(FRuntimeStorageTests, CopiesAndRemovesWhenLegacyRenameFails)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageFallback");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	const std::filesystem::path Legacy = Paths.LaunchDirectory / "DurinEditor.yaml";
	const std::filesystem::path Saved = Paths.ConfigDirectory / "DurinEditor.yaml";
	WriteText(Legacy, "fallback");
	const Durin::FRuntimeStoragePreparationResult Result =
		Durin::PrepareRuntimeStorage(
			Paths, "DurinEditor.yaml", {.bForceLegacyFileRenameFailure = true});
	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, Saved);
	EXPECT_FALSE(std::filesystem::exists(Legacy));
	EXPECT_EQ(ReadText(Saved), "fallback");
}

TEST(FRuntimeStorageTests, ExistingDestinationWinsWithoutMutation)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageExisting");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	const std::filesystem::path Legacy = Paths.LaunchDirectory / "DurinEditor.yaml";
	const std::filesystem::path Saved = Paths.ConfigDirectory / "DurinEditor.yaml";
	WriteText(Legacy, "legacy");
	WriteText(Saved, "saved");
	const Durin::FRuntimeStoragePreparationResult Result =
		Durin::PrepareRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, Saved);
	EXPECT_EQ(ReadText(Legacy), "legacy");
	EXPECT_EQ(ReadText(Saved), "saved");
}

TEST(FRuntimeStorageTests, FilesystemFailureIsPassLocal)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageFailure");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	WriteText(Paths.SavedDirectory, "not a directory");
	const Durin::FRuntimeStoragePreparationResult Failed =
		Durin::PrepareRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_FALSE(Failed.Warnings.empty());
	EXPECT_EQ(Failed.AppConfigPath, Paths.LaunchDirectory / "DurinEditor.yaml");
	ASSERT_TRUE(std::filesystem::remove(Paths.SavedDirectory));
	const Durin::FRuntimeStoragePreparationResult Recovered =
		Durin::PrepareRuntimeStorage(Paths, "DurinEditor.yaml");
	EXPECT_TRUE(Recovered.Warnings.empty());
}
