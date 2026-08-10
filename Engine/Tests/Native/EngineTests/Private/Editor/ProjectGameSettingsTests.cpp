#include "Engine/ProjectGameSettings.h"

#include "NativeTestSupport.h"
#include "Yaml/Yaml.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeSettingsFile(std::string_view Name) -> std::filesystem::path
	{
		const std::filesystem::path Root =
			Durin::Testing::CreateTestFixtureDirectory(std::format("ProjectGameSettings{}", Name));
		std::filesystem::create_directories(Root / "Configs");
		return Root / "Configs" / "Project.yaml";
	}

	auto WriteSettings(const std::filesystem::path& File, std::string_view Text) -> void
	{
		std::ofstream Stream(File);
		ASSERT_TRUE(Stream.is_open());
		Stream << Text;
		ASSERT_TRUE(Stream.good());
	}
}

TEST(FProjectGameSettingsTests, MissingFileAndMissingPairSelectLifecycleOnlyPlay)
{
	const std::filesystem::path File = MakeSettingsFile("Missing");
	Durin::FProjectGameSettings Settings;
	const Durin::FProjectGameSettingsResult Result =
		Durin::FProjectGameSettingsStore(File).Load(Settings);
	ASSERT_TRUE(Result);
	EXPECT_TRUE(Settings.DefaultLevel.empty());
	EXPECT_FALSE(Settings.HasNativeGameplay());
	const Durin::FNativeGameModeResolution Resolution =
		Durin::ResolveNativeGameMode(Settings);
	EXPECT_TRUE(Resolution);
	EXPECT_FALSE(Resolution.HasNativeGameplay());
}

TEST(FProjectGameSettingsTests, ReadsOnlyTheGameSectionAndRequiresCompleteNativePair)
{
	const std::filesystem::path File = MakeSettingsFile("Schema");
	WriteSettings(File,
		"Editor:\n"
		"  DefaultLevel: /Game/Levels/Legacy\n"
		"Game:\n"
		"  DefaultLevel: /Game/Levels/Current\n"
		"  NativeModule: Sandbox\n"
		"  GameModeClass: Durin::Sandbox::ADefaultGameMode\n");
	Durin::FProjectGameSettings Settings;
	ASSERT_TRUE(Durin::FProjectGameSettingsStore(File).Load(Settings));
	EXPECT_EQ(Settings.DefaultLevel, "/Game/Levels/Current");
	EXPECT_EQ(Settings.NativeModule, "Sandbox");
	EXPECT_EQ(Settings.GameModeClass, "Durin::Sandbox::ADefaultGameMode");
	EXPECT_TRUE(Settings.HasNativeGameplay());

	WriteSettings(File,
		"Game:\n"
		"  NativeModule: Sandbox\n");
	const Durin::FProjectGameSettingsResult Partial =
		Durin::FProjectGameSettingsStore(File).Load(Settings);
	EXPECT_EQ(Partial.Error,
		Durin::EProjectGameSettingsError::IncompleteNativeGameplayPair);
	EXPECT_NE(Partial.Message.find("Game.NativeModule"), std::string::npos);
	EXPECT_NE(Partial.Message.find("Game.GameModeClass"), std::string::npos);
}

TEST(FProjectGameSettingsTests, DefaultLevelUpdatePreservesNativeAndUnrelatedSettings)
{
	const std::filesystem::path File = MakeSettingsFile("Update");
	WriteSettings(File,
		"Game:\n"
		"  DefaultLevel: /Game/Levels/Old\n"
		"  NativeModule: Sandbox\n"
		"  GameModeClass: Durin::Sandbox::ADefaultGameMode\n"
		"  Preserve: Keep\n"
		"RootValue: 17\n");
	const Durin::FProjectGameSettingsResult Save =
		Durin::FProjectGameSettingsStore(File).SaveDefaultLevel(
			"/Game/Levels/New");
	ASSERT_TRUE(Save) << Save.Message;
	Durin::FYamlDocument Document;
	Durin::FYamlParseError Error;
	ASSERT_TRUE(Document.LoadFromFile(File.generic_string(), &Error)) << Error.Message;
	const Durin::FYamlNodeView Game = Document.GetRootView().GetView("Game");
	EXPECT_EQ(Game.GetView("DefaultLevel").GetString(), "/Game/Levels/New");
	EXPECT_EQ(Game.GetView("NativeModule").GetString(), "Sandbox");
	EXPECT_EQ(Game.GetView("GameModeClass").GetString(),
		"Durin::Sandbox::ADefaultGameMode");
	EXPECT_EQ(Game.GetView("Preserve").GetString(), "Keep");
	EXPECT_EQ(Document.GetRootView().GetView("RootValue").GetInt(), 17);
}

TEST(FProjectGameSettingsTests, RejectsWrongNodeShapesWithoutMutatingTheFile)
{
	const std::filesystem::path File = MakeSettingsFile("Shapes");
	WriteSettings(File, "Game: invalid\n");
	Durin::FProjectGameSettings Settings;
	const Durin::FProjectGameSettingsResult Result =
		Durin::FProjectGameSettingsStore(File).Load(Settings);
	EXPECT_EQ(Result.Error, Durin::EProjectGameSettingsError::InvalidGameSection);
	std::vector<Durin::uint8> Bytes;
	const Durin::FProjectGameSettingsResult Update =
		Durin::FProjectGameSettingsStore(File).BuildDefaultLevelUpdate(
			"/Game/Levels/New", Bytes);
	EXPECT_EQ(Update.Error, Durin::EProjectGameSettingsError::InvalidGameSection);
	EXPECT_TRUE(Bytes.empty());

	WriteSettings(File,
		"Game:\n"
		"  NativeModule: [Sandbox]\n"
		"  GameModeClass: Durin::Sandbox::ADefaultGameMode\n");
	Bytes.clear();
	const Durin::FProjectGameSettingsResult InvalidScalar =
		Durin::FProjectGameSettingsStore(File).BuildDefaultLevelUpdate(
			"/Game/Levels/New", Bytes);
	EXPECT_EQ(InvalidScalar.Error, Durin::EProjectGameSettingsError::InvalidScalar);
	EXPECT_TRUE(Bytes.empty());

	WriteSettings(File,
		"Game:\n"
		"  NativeModule: Sandbox\n");
	const Durin::FProjectGameSettingsResult PartialPair =
		Durin::FProjectGameSettingsStore(File).SaveDefaultLevel(
			"/Game/Levels/New");
	EXPECT_EQ(PartialPair.Error,
		Durin::EProjectGameSettingsError::IncompleteNativeGameplayPair);
	std::ifstream Stream(File);
	ASSERT_TRUE(Stream.is_open());
	const std::string Unchanged{
		std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	EXPECT_EQ(Unchanged, "Game:\n  NativeModule: Sandbox\n");
}
