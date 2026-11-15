#include "NativeTestSupport.h"
#include "Runtime/Launch/Private/RuntimeStorage.h"

#include <gtest/gtest.h>

namespace
{
	auto MakePaths(const std::filesystem::path& Root)
		-> Durin::FRuntimeStoragePaths
	{
		return {
			.SavedDirectory = Root / "Launch" / "Saved",
			.ConfigDirectory = Root / "Launch" / "Saved" / "Configs",
			.LogDirectory = Root / "Launch" / "Saved" / "Logs",
			.AppConfigTemplatePath = Root / "Launch" / "Templates"
				/ "TP_DurinGame.yaml"};
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
		return {std::istreambuf_iterator<char>(Stream),
			std::istreambuf_iterator<char>()};
	}
}

TEST(FRuntimeStorageTests, CreatesMissingConfigFromDeployedTemplate)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageCreateConfig");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	WriteText(Paths.AppConfigTemplatePath, "template");

	const Durin::FRuntimeStoragePreparationResult Result =
		Durin::PrepareRuntimeStorage(Paths, "DurinGame.yaml");

	const std::filesystem::path ConfigPath =
		Paths.ConfigDirectory / "DurinGame.yaml";
	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, ConfigPath);
	EXPECT_EQ(ReadText(ConfigPath), "template");
	EXPECT_TRUE(std::filesystem::is_directory(Paths.LogDirectory));
}

TEST(FRuntimeStorageTests, PreservesExistingUserConfig)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageExistingConfig");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	const std::filesystem::path ConfigPath =
		Paths.ConfigDirectory / "DurinGame.yaml";
	WriteText(Paths.AppConfigTemplatePath, "template");
	WriteText(ConfigPath, "user");

	const Durin::FRuntimeStoragePreparationResult Result =
		Durin::PrepareRuntimeStorage(Paths, "DurinGame.yaml");

	EXPECT_TRUE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath, ConfigPath);
	EXPECT_EQ(ReadText(ConfigPath), "user");
}

TEST(FRuntimeStorageTests, MissingTemplateReportsCanonicalConfigFailure)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageMissingTemplate");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);

	const Durin::FRuntimeStoragePreparationResult Result =
		Durin::PrepareRuntimeStorage(Paths, "DurinGame.yaml");

	EXPECT_FALSE(Result.Warnings.empty());
	EXPECT_EQ(Result.AppConfigPath,
		Paths.ConfigDirectory / "DurinGame.yaml");
	EXPECT_FALSE(std::filesystem::exists(Result.AppConfigPath));
}

TEST(FRuntimeStorageTests, FilesystemFailureDoesNotPoisonNextPreparation)
{
	const std::filesystem::path Root =
		Durin::Testing::CreateTestFixtureDirectory("LaunchStorageRecovery");
	const Durin::FRuntimeStoragePaths Paths = MakePaths(Root);
	WriteText(Paths.AppConfigTemplatePath, "template");
	WriteText(Paths.SavedDirectory, "not a directory");

	const Durin::FRuntimeStoragePreparationResult Failed =
		Durin::PrepareRuntimeStorage(Paths, "DurinGame.yaml");
	EXPECT_FALSE(Failed.Warnings.empty());
	EXPECT_EQ(Failed.AppConfigPath,
		Paths.ConfigDirectory / "DurinGame.yaml");

	ASSERT_TRUE(std::filesystem::remove(Paths.SavedDirectory));
	const Durin::FRuntimeStoragePreparationResult Recovered =
		Durin::PrepareRuntimeStorage(Paths, "DurinGame.yaml");
	EXPECT_TRUE(Recovered.Warnings.empty());
	EXPECT_EQ(ReadText(Recovered.AppConfigPath), "template");
}

TEST(FRuntimeUserStorageTests, NewOverridesDoNotSeedInheritedDefaultsAndKeepUserEdits)
{
    const auto Root=Durin::Testing::CreateTestFixtureDirectory("UserOverrides");
    auto Paths=MakePaths(Root); Paths.bUserOverridesOnly=true; Paths.CacheDirectory=Root/"Launch/Cache";
    Paths.SavedDirectory/=""; Paths.ConfigDirectory/=""; Paths.LogDirectory/="";
    WriteText(Paths.AppConfigTemplatePath,"InheritedProjectValue: true\n");
    auto Result=Durin::PrepareRuntimeStorage(Paths,"DurinGame.yaml"); ASSERT_TRUE(Result.Warnings.empty());
    EXPECT_EQ(ReadText(Result.AppConfigPath),"{}\n"); EXPECT_TRUE(std::filesystem::is_directory(Paths.CacheDirectory));
    WriteText(Result.AppConfigPath,"UserChoice: kept\n");
    Result=Durin::PrepareRuntimeStorage(Paths,"DurinGame.yaml"); EXPECT_TRUE(Result.Warnings.empty());
    EXPECT_EQ(ReadText(Result.AppConfigPath),"UserChoice: kept\n");
}

TEST(FRuntimeUserStorageTests, ExplicitMigrationCopiesOnlyAnAbsentDestination)
{
    const auto Root=Durin::Testing::CreateTestFixtureDirectory("MigrateOverrides"); auto Paths=MakePaths(Root);
    Paths.bUserOverridesOnly=true; Paths.LegacyUserConfig=Root/"OldInstallation/Saved/Configs/DurinGame.yaml";
    WriteText(Paths.LegacyUserConfig,"UserChoice: old\n");
    auto Result=Durin::PrepareRuntimeStorage(Paths,"DurinGame.yaml"); ASSERT_TRUE(Result.Warnings.empty());
    EXPECT_EQ(ReadText(Result.AppConfigPath),"UserChoice: old\n");
    WriteText(Result.AppConfigPath,"UserChoice: new\n"); WriteText(Paths.LegacyUserConfig,"UserChoice: changed\n");
    Result=Durin::PrepareRuntimeStorage(Paths,"DurinGame.yaml"); EXPECT_EQ(ReadText(Result.AppConfigPath),"UserChoice: new\n");
}

TEST(FRuntimeUserStorageTests, UnwritableRootHasNoInstallationFallback)
{
    const auto Root=Durin::Testing::CreateTestFixtureDirectory("BlockedUserStorage"); auto Paths=MakePaths(Root);
    Paths.bUserOverridesOnly=true; WriteText(Paths.SavedDirectory,"occupied by a file");
    const auto Result=Durin::PrepareRuntimeStorage(Paths,"DurinGame.yaml");
    EXPECT_FALSE(Result.Warnings.empty()); EXPECT_FALSE(std::filesystem::exists(Result.AppConfigPath));
}
