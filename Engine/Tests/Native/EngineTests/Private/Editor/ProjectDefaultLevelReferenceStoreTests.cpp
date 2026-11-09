#include "NativeAssetRuntimeTestSupport.h"
#include "AssetTools/Relocation.h"
#include "AssetTools/RedirectorFixup.h"
#include "AssetTools/MutationTesting.h"
#include <gtest/gtest.h>

#include "Asset/Load.h"
#include "Asset/Testing.h"
#include "AssetRegistry/Scan.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Editor/EditorStartup.h"
#include "Engine/World.h"

#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Project.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Settings/ProjectDefaultLevelReferenceStore.h"
#include "Yaml/Yaml.h"

namespace
{
	auto MakePath(std::string_view Text) -> Durin::FPackagePath
	{
		Durin::FPackagePath Path;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Text, Path));
		return Path;
	}

	auto Relocate(
		const Durin::FPackagePath& Source,
		const Durin::FPackagePath& Destination) -> Durin::FAssetWriteResult
	{
		const Durin::FAssetRelocationMapping Mapping{Source, Destination};
		return Durin::RelocateAssets(std::span{&Mapping, 1}).Result;
	}

	class FScopedStoreRegistration
	{
	public:
		explicit FScopedStoreRegistration(
			Durin::IAssetReferenceStore& Store)
			: Handle(Durin::RegisterAssetReferenceStore(&Store))
		{
		}

		~FScopedStoreRegistration()
		{
			Durin::UnregisterAssetReferenceStore(Handle);
		}

	private:
		Durin::FAssetReferenceStoreHandle Handle = 0;
	};

	struct FDefaultLevelScenario
	{
		std::filesystem::path Root;
		Durin::FProjectInfo Project;
		Durin::FPackagePath OldPath;
		Durin::FPackagePath NewPath;
	};

	auto BuildScenario(std::string_view Name) -> FDefaultLevelScenario
	{
		static const bool Initialized = [] {
			Durin::Testing::InitializeAssetSystemForTests();
			(void)Durin::DLevel::StaticClass();
			return true;
		}();
		(void)Initialized;

		FDefaultLevelScenario Scenario;
		Scenario.Root = Durin::Testing::CreateTestFixtureDirectory(
			std::format("ProjectDefaultLevel{}", Name));
		std::filesystem::create_directories(Scenario.Root / "Content");
		std::filesystem::create_directories(Scenario.Root / "Configs");
		Scenario.Project = {
			.Name = std::string(Name),
			.ProjectFile = (Scenario.Root / "Test.dproject").generic_string(),
			.ProjectDir = Scenario.Root.generic_string() + "/",
			.ContentDir = (Scenario.Root / "Content").generic_string() + "/",
			.MountRoot = "/DefaultLevelTests/"};
		return Scenario;
	}

	auto ConfigureAssets(FDefaultLevelScenario& Scenario)
		-> std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture>
	{
		Durin::ShutdownAssetRuntime();
		Durin::CollectGarbage();
		Durin::InitializeAssetRuntime();
		Durin::FPaths::SetDerivedDataCacheDirForTests(
			(Scenario.Root / "DerivedDataCache").generic_string());
		const std::array Mounts = {Durin::FMountPoint{
			.VirtualRoot = "/DefaultLevelTests/",
			.Owner = Durin::EMountOwner::Test,
			.Root = Scenario.Root / "Content",
			.bAutoScan = true,
			.bContentWritable = true}};
		auto Fixture = std::make_unique<
			Durin::Testing::FScopedMountRegistryFixture>(Mounts);
		EXPECT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Scenario.OldPath = MakePath("/DefaultLevelTests/Levels/Old");
		Scenario.NewPath = MakePath("/DefaultLevelTests/Levels/New");
		std::ofstream Settings(Scenario.Root / "Configs" / "Project.yaml");
		EXPECT_TRUE(Settings.is_open());
		Settings << "Game:\n"
			<< "  DefaultLevel: " << Scenario.OldPath.ToString() << "\n"
			<< "  Preserve: Keep\n"
			<< "RootValue: 17\n";
		EXPECT_TRUE(Settings.good());
		Settings.close();
		Durin::DLevel* Level = nullptr;
		EXPECT_TRUE(Durin::CreatePackageLeafAssetForTesting(Scenario.OldPath, Level));
		EXPECT_NE(Level, nullptr);
		if (Level) EXPECT_TRUE(Durin::SavePackage(Level->GetPackage()));
		EXPECT_TRUE(Relocate(Scenario.OldPath, Scenario.NewPath));
		return Fixture;
	}

	auto LoadSettings(const FDefaultLevelScenario& Scenario) -> Durin::FYamlDocument
	{
		Durin::FYamlDocument Document;
		const auto Loaded = Document.LoadFromFile(Scenario.Root / "Configs" / "Project.yaml");
		EXPECT_TRUE(Loaded) << Loaded.error().ToString();
		return Document;
	}
}

TEST(FProjectDefaultLevelReferenceStoreTests, FixUpRewritesYamlAndPreservesOtherSettings)
{
	FDefaultLevelScenario Scenario = BuildScenario("Rewrite");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::FPackagePath NotifiedPath;
	Durin::Editor::Level::FProjectDefaultLevelReferenceStore Store(
		[&](const Durin::FPackagePath& Path) { NotifiedPath = Path; },
		[&] { return &Scenario.Project; });
	FScopedStoreRegistration Registration(Store);
	ASSERT_TRUE(Durin::FixUpRedirectors(
		std::span{&Scenario.OldPath, 1},
		Durin::EAssetRedirectorFixupMode::RewriteAndDelete).Result);
	EXPECT_EQ(NotifiedPath, Scenario.NewPath);
	EXPECT_EQ(Durin::FindPackageExact(
		Scenario.OldPath), nullptr);
	const Durin::FYamlDocument Settings = LoadSettings(Scenario);
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("DefaultLevel").GetString(), Scenario.NewPath.ToString());
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("Preserve").GetString(), "Keep");
	EXPECT_EQ(Settings.GetRootView().GetView("RootValue").GetInt(), 17);
}

TEST(FProjectDefaultLevelReferenceStoreTests, VerificationFailureRetainsPublishedSettings)
{
	FDefaultLevelScenario Scenario = BuildScenario("Restore");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::FPackagePath NotifiedPath;
	Durin::Editor::Level::FProjectDefaultLevelReferenceStore Store(
		[&](const Durin::FPackagePath& Path) { NotifiedPath = Path; },
		[&] { return &Scenario.Project; });
	FScopedStoreRegistration Registration(Store);
	Durin::SetAssetRedirectorFixupFailurePointForTesting(
		Durin::EAssetRedirectorFixupFailurePoint::Verify);
	const auto Details = Durin::FixUpRedirectors(
		std::span{&Scenario.OldPath, 1},
		Durin::EAssetRedirectorFixupMode::RewriteAndDelete);
	const auto& Result = Details.Result;
	Durin::SetAssetRedirectorFixupFailurePointForTesting(
		Durin::EAssetRedirectorFixupFailurePoint::None);
	EXPECT_EQ(Result.Error, Durin::EAssetWriteError::IoError);
	EXPECT_EQ(NotifiedPath, Scenario.NewPath);
	const auto Alias = Durin::FindPackageExact(
		Scenario.OldPath);
	ASSERT_NE(Alias, nullptr);
	EXPECT_TRUE(Alias->TopLevelAssets.front().IsRedirector());
	const Durin::FYamlDocument Settings = LoadSettings(Scenario);
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("DefaultLevel").GetString(), Scenario.NewPath.ToString());
}

TEST(FProjectDefaultLevelReferenceStoreTests, CookContributesCanonicalRootWithoutEditingYaml)
{
	FDefaultLevelScenario Scenario = BuildScenario("CookRoot");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::Editor::Level::FProjectDefaultLevelReferenceStore Store(
		{}, [&] { return &Scenario.Project; });
	FScopedStoreRegistration Registration(Store);

	Durin::FAssetReferenceStoreSnapshot Snapshot;
	ASSERT_TRUE(Store.CaptureSnapshot(Snapshot));
	ASSERT_EQ(Snapshot.Occurrences.size(), 1u);
	EXPECT_TRUE(Snapshot.Occurrences.front().bCookRoot);
	EXPECT_EQ(
		Snapshot.Occurrences.front().ExpectedClass,
		Durin::DLevel::StaticClass()->GetQualifiedName().ToString());

	std::vector<Durin::FPackagePath> Reachable;
	ASSERT_TRUE(Durin::BuildCookReachability(
		{}, Reachable));
	EXPECT_EQ(Reachable, (std::vector{Scenario.NewPath}));
	const Durin::FYamlDocument Settings = LoadSettings(Scenario);
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("DefaultLevel").GetString(), Scenario.OldPath.ToString());
}

TEST(FProjectDefaultLevelReferenceStoreTests, ResolvesUniqueLevelWithoutInferringPackageLeafName)
{
	FDefaultLevelScenario Scenario = BuildScenario("UniqueLevel");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::FSoftObjectPath LevelPath;
	const auto Result =
		Durin::ResolveLevelPackage(Scenario.OldPath, LevelPath);
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(LevelPath.GetPackagePath(), Scenario.NewPath);
	EXPECT_EQ(LevelPath.GetAssetPath().GetAssetName(), "Old");
	EXPECT_NE(LevelPath.GetAssetPath().GetAssetName(),
		Scenario.NewPath.GetPackageName());
}

TEST(FProjectDefaultLevelReferenceStoreTests, RejectsPackageWithoutTopLevelLevel)
{
	FDefaultLevelScenario Scenario = BuildScenario("NoLevel");
	auto MountFixture = ConfigureAssets(Scenario);
	const Durin::FPackagePath WorldPath =
		MakePath("/DefaultLevelTests/Worlds/OnlyWorld");
	Durin::DWorld* World = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(WorldPath, World));
	ASSERT_NE(World, nullptr);
	EXPECT_FALSE(Durin::SavePackage(World->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(World->GetPackage(), {.Mode = Durin::EAssetPackageSaveMode::Complete}));

	Durin::FSoftObjectPath LevelPath;
	const auto Result =
		Durin::ResolveLevelPackage(WorldPath, LevelPath);
	EXPECT_EQ(Result.Error, Durin::EAssetReadError::TypeMismatch);
	EXPECT_FALSE(LevelPath.IsValid());
}

TEST(FProjectDefaultLevelReferenceStoreTests, RejectsPackageWithMultipleTopLevelLevels)
{
	FDefaultLevelScenario Scenario = BuildScenario("MultipleLevels");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::DPackage* Package =
		Durin::FindResidentPackage(Scenario.NewPath);
	ASSERT_NE(Package, nullptr);
	Durin::DLevel* Secondary =
		Durin::NewObject<Durin::DLevel>(Package, "Secondary");
	ASSERT_NE(Secondary, nullptr);
	ASSERT_TRUE(Durin::SavePackage(Package));

	Durin::FSoftObjectPath LevelPath;
	const auto Result =
		Durin::ResolveLevelPackage(Scenario.NewPath, LevelPath);
	EXPECT_EQ(Result.Error, Durin::EAssetReadError::InvalidPackageType);
	EXPECT_FALSE(LevelPath.IsValid());
}

TEST(FProjectDefaultLevelReferenceStoreTests, StartupResolutionFollowsRedirectsAndChecksTheResolvedProjectMount)
{
	FDefaultLevelScenario Scenario = BuildScenario("StartupResolution");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::FSoftObjectPath Path;
	std::string Error;
	ASSERT_TRUE(Durin::Editor::Startup::ResolveStartupLevel(Scenario.Project, Path, Error)) << Error;
	EXPECT_EQ(Path.GetPackagePath(), Scenario.NewPath);
	EXPECT_EQ(Path.GetAssetPath().GetAssetName(), "Old");
	EXPECT_TRUE(Error.empty());
	// A similarly prefixed mount must not admit a resolved level from another project.
	Scenario.Project.MountRoot = "/DefaultLevelTest";
	EXPECT_FALSE(Durin::Editor::Startup::ResolveStartupLevel(Scenario.Project, Path, Error));
	EXPECT_FALSE(Path.IsValid());
	EXPECT_NE(Error.find("outside the selected project mount"), std::string::npos);
}

TEST(FProjectDefaultLevelReferenceStoreTests, StartupResolutionAcceptsNoLevelAndRejectsMalformedSettingsAndPaths)
{
	FDefaultLevelScenario Scenario = BuildScenario("StartupConfiguration");
	Durin::FSoftObjectPath Path;
	std::string Error = "stale error";
	ASSERT_TRUE(Durin::Editor::Startup::ResolveStartupLevel(Scenario.Project, Path, Error)) << Error;
	EXPECT_FALSE(Path.IsValid());
	EXPECT_TRUE(Error.empty());
	for (const auto Text : {"Game: [", "Game:\n  DefaultLevel: not-an-absolute-package\n"})
	{
		std::ofstream Settings(Scenario.Root / "Configs" / "Project.yaml");
		Settings << Text;
		Settings.close();
		EXPECT_FALSE(Durin::Editor::Startup::ResolveStartupLevel(Scenario.Project, Path, Error));
		EXPECT_FALSE(Path.IsValid());
		EXPECT_FALSE(Error.empty());
	}
}

TEST(FProjectDefaultLevelReferenceStoreTests, StartupResolutionRejectsMissingAndAmbiguousLevels)
{
	FDefaultLevelScenario Scenario = BuildScenario("StartupInvalidAssets");
	auto MountFixture = ConfigureAssets(Scenario);
	auto Store = Durin::FProjectGameSettingsStore::ForProject(Scenario.Project);
	ASSERT_TRUE(Store.SaveDefaultLevel("/DefaultLevelTests/Levels/Missing"));
	Durin::FSoftObjectPath Path;
	std::string Error;
	EXPECT_FALSE(Durin::Editor::Startup::ResolveStartupLevel(Scenario.Project, Path, Error));
	EXPECT_FALSE(Error.empty());
	ASSERT_TRUE(Store.SaveDefaultLevel(Scenario.NewPath.ToString()));
	auto* Package = Durin::FindResidentPackage(Scenario.NewPath);
	ASSERT_NE(Package, nullptr);
	ASSERT_NE(Durin::NewObject<Durin::DLevel>(Package, "Secondary"), nullptr);
	ASSERT_TRUE(Durin::SavePackage(Package));
	EXPECT_FALSE(Durin::Editor::Startup::ResolveStartupLevel(Scenario.Project, Path, Error));
	EXPECT_FALSE(Path.IsValid());
	EXPECT_FALSE(Error.empty());
}
