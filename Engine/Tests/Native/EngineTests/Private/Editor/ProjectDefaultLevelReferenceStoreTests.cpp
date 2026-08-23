#include <gtest/gtest.h>

#include "Asset/Load.h"
#include "Asset/Testing.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Level.h"

#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Settings/ProjectDefaultLevelReferenceStore.h"
#include "Yaml/Yaml.h"

namespace
{
	auto MakePath(std::string_view Text) -> Durin::FAssetPath
	{
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Text, Path));
		return Path;
	}

	auto Relocate(
		const Durin::FAssetPath& Source,
		const Durin::FAssetPath& Destination) -> Durin::Asset::FAssetResult
	{
		const Durin::Asset::FAssetRelocationMapping Mapping{Source, Destination};
		Durin::Asset::FAssetMutationSummary Summary;
		Durin::Asset::FAssetMutationTransaction Transaction;
		Durin::Asset::FAssetResult Result =
			Durin::Asset::PrepareAssetRelocationTransaction(
				std::span{&Mapping, 1}, Summary, Transaction);
		if (Result) Result = Transaction.Commit();
		return Result;
	}

	class FScopedStoreRegistration
	{
	public:
		explicit FScopedStoreRegistration(
			Durin::Asset::IAssetReferenceStore& Store)
			: Handle(Durin::Asset::RegisterAssetReferenceStore(&Store))
		{
		}

		~FScopedStoreRegistration()
		{
			Durin::Asset::UnregisterAssetReferenceStore(Handle);
		}

	private:
		Durin::Asset::FAssetReferenceStoreHandle Handle = 0;
	};

	struct FDefaultLevelScenario
	{
		std::filesystem::path Root;
		Durin::FProjectInfo Project;
		Durin::FAssetPath OldPath;
		Durin::FAssetPath NewPath;
	};

	auto BuildScenario(std::string_view Name) -> FDefaultLevelScenario
	{
		static const bool Initialized = [] {
			Durin::Testing::InitializeDObjectSystemForTests();
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
		-> std::unique_ptr<Durin::PathUtilities::FScopedMountRegistryFixture>
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::Asset::InitializeAssetManager();
		Durin::FPaths::SetDerivedDataCacheDirForTests(
			(Scenario.Root / "DerivedDataCache").generic_string());
		const std::array Mounts = {Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/DefaultLevelTests/",
			.Owner = Durin::PathUtilities::EMountOwner::Test,
			.Root = Scenario.Root / "Content",
			.bAutoScan = true,
			.bAuthoringWritable = true}};
		auto Fixture = std::make_unique<
			Durin::PathUtilities::FScopedMountRegistryFixture>(Mounts);
		EXPECT_TRUE(Durin::Asset::RefreshAssetCatalog(
			Durin::Asset::EAssetRegistryScanMode::FullValidation));
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
		EXPECT_TRUE(Durin::Asset::CreateAsset(Scenario.OldPath, Level));
		EXPECT_NE(Level, nullptr);
		if (Level) EXPECT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
		EXPECT_TRUE(Relocate(Scenario.OldPath, Scenario.NewPath));
		return Fixture;
	}

	auto LoadSettings(const FDefaultLevelScenario& Scenario) -> Durin::FYamlDocument
	{
		Durin::FYamlDocument Document;
		Durin::FYamlParseError Error;
		EXPECT_TRUE(Document.LoadFromFile(
			(Scenario.Root / "Configs" / "Project.yaml").generic_string(),
			&Error)) << Error.Message;
		return Document;
	}
}

TEST(FProjectDefaultLevelReferenceStoreTests, FixUpRewritesYamlAndPreservesOtherSettings)
{
	FDefaultLevelScenario Scenario = BuildScenario("Rewrite");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::FAssetPath NotifiedPath;
	Durin::Editor::Level::FProjectDefaultLevelReferenceStore Store(
		[&](const Durin::FAssetPath& Path) { NotifiedPath = Path; },
		[&] { return &Scenario.Project; });
	FScopedStoreRegistration Registration(Store);
	Durin::Asset::FAssetRedirectorFixupSummary Summary;
	Durin::Asset::FAssetMutationTransaction Transaction;
	ASSERT_TRUE(Durin::Asset::PrepareRedirectorFixupTransaction(
		std::span{&Scenario.OldPath, 1},
		Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
		Summary,
		Transaction));
	ASSERT_TRUE(Transaction.Commit());
	EXPECT_EQ(NotifiedPath, Scenario.NewPath);
	EXPECT_EQ(Durin::Asset::FindAssetExact(
		Scenario.OldPath), nullptr);
	const Durin::FYamlDocument Settings = LoadSettings(Scenario);
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("DefaultLevel").GetString(), Scenario.NewPath.ToString());
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("Preserve").GetString(), "Keep");
	EXPECT_EQ(Settings.GetRootView().GetView("RootValue").GetInt(), 17);
}

TEST(FProjectDefaultLevelReferenceStoreTests, VerificationFailureRestoresYamlAndAlias)
{
	FDefaultLevelScenario Scenario = BuildScenario("Restore");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::FAssetPath NotifiedPath;
	Durin::Editor::Level::FProjectDefaultLevelReferenceStore Store(
		[&](const Durin::FAssetPath& Path) { NotifiedPath = Path; },
		[&] { return &Scenario.Project; });
	FScopedStoreRegistration Registration(Store);
	Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
		Durin::Asset::EAssetRedirectorFixupFailurePoint::Verify);
	Durin::Asset::FAssetRedirectorFixupSummary Summary;
	Durin::Asset::FAssetMutationTransaction Transaction;
	Durin::Asset::FAssetResult Result =
		Durin::Asset::PrepareRedirectorFixupTransaction(
			std::span{&Scenario.OldPath, 1},
			Durin::Asset::EAssetRedirectorFixupMode::RewriteAndDelete,
			Summary,
			Transaction);
	if (Result) Result = Transaction.Commit();
	Durin::Asset::SetAssetRedirectorFixupFailurePointForTesting(
		Durin::Asset::EAssetRedirectorFixupFailurePoint::None);
	EXPECT_EQ(Result.Error, Durin::Asset::EAssetError::IoError);
	EXPECT_EQ(NotifiedPath, Scenario.OldPath);
	const auto Alias = Durin::Asset::FindAssetExact(
		Scenario.OldPath);
	ASSERT_NE(Alias, nullptr);
	EXPECT_EQ(Alias->EntryKind,
		Durin::Asset::EAssetRegistryEntryKind::Redirector);
	const Durin::FYamlDocument Settings = LoadSettings(Scenario);
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("DefaultLevel").GetString(), Scenario.OldPath.ToString());
}

TEST(FProjectDefaultLevelReferenceStoreTests, CookContributesCanonicalRootWithoutEditingYaml)
{
	FDefaultLevelScenario Scenario = BuildScenario("CookRoot");
	auto MountFixture = ConfigureAssets(Scenario);
	Durin::Editor::Level::FProjectDefaultLevelReferenceStore Store(
		{}, [&] { return &Scenario.Project; });
	FScopedStoreRegistration Registration(Store);

	Durin::Asset::FAssetReferenceStoreSnapshot Snapshot;
	ASSERT_TRUE(Store.CaptureSnapshot(Snapshot));
	ASSERT_EQ(Snapshot.Occurrences.size(), 1u);
	EXPECT_TRUE(Snapshot.Occurrences.front().bCookRoot);
	EXPECT_EQ(
		Snapshot.Occurrences.front().ExpectedClass,
		Durin::DLevel::StaticClass()->GetQualifiedName().ToString());

	std::vector<Durin::FAssetPath> Reachable;
	ASSERT_TRUE(Durin::Asset::BuildCookReachability(
		{}, Reachable));
	EXPECT_EQ(Reachable, (std::vector{Scenario.NewPath}));
	const Durin::FYamlDocument Settings = LoadSettings(Scenario);
	EXPECT_EQ(Settings.GetRootView().GetView("Game")
		.GetView("DefaultLevel").GetString(), Scenario.OldPath.ToString());
}
