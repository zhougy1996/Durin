#include "Engine/AssetManager.h"
#include "Engine/PrimaryDataAsset.h"
#include "Engine/Level.h"
#include "Asset/Asset.h"
#include "Asset/PackageSerialization.h"
#include "AssetRegistry/Catalog.h"
#include "DObject/Package.h"
#include "DObject/PackageFormat.h"
#include "DObject/PackageLinker.h"
#include "DObject/DObjectArray.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/TaskRuntime.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	auto Id(std::string_view Text) -> FPrimaryAssetId
	{
		FPrimaryAssetId Result;
		EXPECT_TRUE(FPrimaryAssetId::TryParse(Text, Result));
		return Result;
	}
	auto Path(std::string_view Name) -> FTopLevelAssetPath
	{
		FTopLevelAssetPath Result;
		EXPECT_TRUE(FTopLevelAssetPath::TryCreate("/Primary/" + std::string(Name) + "." + std::string(Name), Result));
		return Result;
	}
	class FPrimaryAssetTests : public testing::Test
	{
	protected:
		std::filesystem::path Root;
		TStrongObjectPtr<DAssetManager> Manager;
		bool bOwnScheduler = false;
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			ShutdownAssetRuntime(); CollectGarbage();
			Root = Testing::CreateTestFixtureDirectory("PrimaryAssets");
			Testing::RegisterMountPointForTests("/Primary/", Root.generic_string() + "/");
			ASSERT_TRUE(InitializeAssetRuntime());
			ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
			bOwnScheduler = !IsTaskSchedulerRunning();
			if (bOwnScheduler) ASSERT_TRUE(InitializeTaskScheduler(2));
			Manager = TStrongObjectPtr<DAssetManager>(NewObject<DAssetManager>(nullptr, FName("TestAssetManager")));
		}
		auto TearDown() -> void override
		{
			Manager->Shutdown(); Manager.Reset();
			ShutdownAssetRuntime(); CollectGarbage();
			if (bOwnScheduler) ShutdownTaskSystem(ETaskShutdownMode::Drain);
		}
		auto Write(std::string_view Name, FPrimaryAssetId Primary = {}, FAssetBundleData Bundles = {}, uint32 Version = ObjectPackage::DastV11FormatVersion) -> void
		{
			ObjectPackage::FLinkerTables Linker;
			Linker.FormatVersion = Version;
			const auto Asset = Path(Name);
			Linker.Summary.PackagePath = Asset.GetPackagePath();
			ObjectPackage::FPackageIndex Export;
			ASSERT_TRUE(ObjectPackage::FPackageIndex::TryExport(0, Export));
			Linker.Summary.TopLevelAssets.push_back({Export, Asset, "Durin::DObject", {}, Primary, Bundles});
			Linker.Exports.push_back({.ObjectName = std::string(Name), .ClassName = "Durin::DObject"});
			FByteBuffer Main, Bulk;
			const auto Result = ObjectPackage::WritePackage(Linker, Main, Bulk);
			ASSERT_TRUE(Result) << ObjectPackage::FormatPackageError(Result);
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Main, Root / (std::string(Name) + ".dasset")));
			ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		}
		auto Pump(const std::shared_ptr<FStreamableHandle>& Handle) -> void
		{
			if (!Handle) return;
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Handle->IsComplete() && std::chrono::steady_clock::now() < Deadline)
			{
				ProcessAsyncLoading(10, 100);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			ASSERT_TRUE(Handle->IsComplete());
		}
		auto BundleFixture() -> void
		{
			Write("A"); Write("B");
			FAssetBundleData Data;
			ASSERT_TRUE(Data.AddBundleAsset(FName("A"), Path("A")));
			ASSERT_TRUE(Data.AddBundleAsset(FName("B"), Path("B")));
			ASSERT_TRUE(Data.AddBundleAsset(FName("Broken"), Path("Absent")));
			Write("Root", Id("Scenario:Root"), Data);
			ASSERT_TRUE(Manager->StartInitialLoading());
		}
	};
}

TEST_F(FPrimaryAssetTests, MetadataDiscoveryConstructsNoAssetsAndReadsLegacyPackages)
{
	Write("Root", Id("Scenario:Root"));
	Write("Legacy", {}, {}, ObjectPackage::DastV10FormatVersion);
	const auto Before = GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates).size();
	ASSERT_TRUE(Manager->StartInitialLoading());
	EXPECT_EQ(Manager->GetPrimaryAssetPath(Id("Scenario:Root")).GetAssetPath(), Path("Root"));
	EXPECT_EQ(Manager->GetPrimaryAssetIdList(FPrimaryAssetType(FName("Scenario"))).size(), 1u);
	EXPECT_EQ(GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates).size(), Before);
	EXPECT_EQ(FindResidentPackage(Path("Root").GetPackagePath()), nullptr);
	EXPECT_FALSE(FindAssetExact(Path("Legacy"))->PrimaryAssetId.IsValid());
}

TEST_F(FPrimaryAssetTests, PrimaryDataAssetsAndMapsPublishNativeIdsOnSave)
{
	auto* Package = CreatePackage(Path("Data").GetPackagePath());
	TStrongObjectPtr<DPackage> Owner(Package);
	auto* Data = NewObject<DPrimaryDataAsset>(Package, FName("Data"), EObjectFlags::Public);
	ASSERT_EQ(Package->FindTopLevelAsset(FName("Data")), Data);
	const auto Saved = SavePackage(Package);
	ASSERT_TRUE(Saved) << Saved.Message;
	const auto Entry = FindAssetExact(Path("Data"));
	ASSERT_TRUE(Entry);
	EXPECT_EQ(Entry->PrimaryAssetId, Data->GetPrimaryAssetId());
	EXPECT_TRUE(Entry->PrimaryAssetId.IsValid());
	auto* MapPackage = CreatePackage(Path("Map").GetPackagePath());
	TStrongObjectPtr<DPackage> MapOwner(MapPackage);
	auto* Map = NewObject<DLevel>(MapPackage, FName("Map"), EObjectFlags::Public);
	ASSERT_EQ(MapPackage->FindTopLevelAsset(FName("Map")), Map);
	const auto MapSaved = SavePackage(MapPackage);
	ASSERT_TRUE(MapSaved) << MapSaved.Message;
	EXPECT_EQ(FindAssetExact(Path("Map"))->PrimaryAssetId, Id("Map:Map"));
}

TEST_F(FPrimaryAssetTests, ManagerOwnershipOutlivesObservationAndUnloadPreservesNativeOwner)
{
	Write("Root", Id("Scenario:Root"));
	ASSERT_TRUE(Manager->StartInitialLoading());
	auto Handle = Manager->LoadPrimaryAsset(Id("Scenario:Root")); Pump(Handle);
	ASSERT_FALSE(Handle->HasError()) << Handle->GetError().Message;
	Handle->ReleaseHandle(); Handle.reset(); CollectGarbage();
	auto* Asset = Manager->GetPrimaryAssetObject(Id("Scenario:Root"));
	ASSERT_NE(Asset, nullptr);
	TStrongObjectPtr<DObject> Native(Asset);
	TWeakObjectPtr<DObject> Weak(Asset);
	EXPECT_TRUE(Manager->UnloadPrimaryAsset(Id("Scenario:Root")));
	CollectGarbage(); EXPECT_NE(Weak.Get(), nullptr);
	Native.Reset(); CollectGarbage(); EXPECT_EQ(Weak.Get(), nullptr);
}

TEST_F(FPrimaryAssetTests, BundlesLoadOnlyDeclaredTargetsAndFailuresPreserveCurrentState)
{
	BundleFixture();
	const std::array A{FName("A")}, Broken{FName("Broken")};
	auto First = Manager->LoadPrimaryAsset(Id("Scenario:Root"), A); Pump(First);
	ASSERT_FALSE(First->HasError()); First.reset();
	EXPECT_NE(FindResidentPackage(Path("A").GetPackagePath()), nullptr);
	EXPECT_EQ(FindResidentPackage(Path("B").GetPackagePath()), nullptr);
	auto Failed = Manager->LoadPrimaryAsset(Id("Scenario:Root"), Broken); Pump(Failed);
	ASSERT_TRUE(Failed->HasError());
	EXPECT_EQ(Manager->GetPrimaryAssetBundleState(Id("Scenario:Root")), std::vector<FName>(A.begin(), A.end()));
	EXPECT_NE(Manager->GetPrimaryAssetObject(Id("Scenario:Root")), nullptr);
}

TEST_F(FPrimaryAssetTests, SupersededTransitionsAndUnloadReloadIgnoreStaleCallbacks)
{
	BundleFixture();
	const std::array Ids{Id("Scenario:Root")};
	const std::array A{FName("A")}, B{FName("B")};
	auto Old = Manager->LoadPrimaryAsset(Ids[0], A);
	auto New = Manager->LoadPrimaryAsset(Ids[0], B); Pump(New); Pump(Old);
	ASSERT_FALSE(New->HasError());
	EXPECT_EQ(Manager->GetPrimaryAssetBundleState(Ids[0]), std::vector<FName>(B.begin(), B.end()));
	auto Pending = Manager->LoadPrimaryAsset(Ids[0], A);
	ASSERT_TRUE(Manager->UnloadPrimaryAsset(Ids[0]));
	auto Reload = Manager->LoadPrimaryAsset(Ids[0], B); Pump(Reload); Pump(Pending);
	EXPECT_EQ(Manager->GetPrimaryAssetBundleState(Ids[0]), std::vector<FName>(B.begin(), B.end()));
}

TEST_F(FPrimaryAssetTests, BundleRemovalAndCancellationPreserveLastUsableState)
{
	BundleFixture();
	const std::array Ids{Id("Scenario:Root")};
	const std::array A{FName("A")}, B{FName("B")};
	auto First = Manager->LoadPrimaryAsset(Ids[0], A); Pump(First); First.reset();
	auto Canceled = Manager->LoadPrimaryAsset(Ids[0], B); Canceled->CancelHandle();
	ProcessAsyncLoading(10, 100);
	EXPECT_EQ(Manager->GetPrimaryAssetBundleState(Ids[0]), std::vector<FName>(A.begin(), A.end()));
	auto Removed = Manager->ChangeBundleStateForPrimaryAssets(Ids, B, A); Pump(Removed);
	ASSERT_FALSE(Removed->HasError());
	EXPECT_EQ(Manager->GetPrimaryAssetBundleState(Ids[0]), std::vector<FName>(B.begin(), B.end()));
	Removed.reset(); CollectGarbage();
	EXPECT_EQ(FindResidentPackage(Path("A").GetPackagePath()), nullptr);
}

TEST_F(FPrimaryAssetTests, IndependentPrimariesDoNotRetainEachOthersPackagesAndShutdownDrainsPending)
{
	Write("A", Id("Scenario:A")); Write("B", Id("Scenario:B"));
	ASSERT_TRUE(Manager->StartInitialLoading());
	const std::array Ids{Id("Scenario:A"), Id("Scenario:B")};
	auto Batch = Manager->LoadPrimaryAssets(Ids); Pump(Batch); ASSERT_FALSE(Batch->HasError()); Batch.reset();
	EXPECT_TRUE(Manager->UnloadPrimaryAsset(Ids[0])); CollectGarbage();
	EXPECT_EQ(FindResidentPackage(Path("A").GetPackagePath()), nullptr);
	EXPECT_NE(FindResidentPackage(Path("B").GetPackagePath()), nullptr);
	auto Pending = Manager->LoadPrimaryAsset(Ids[0]);
	Manager->Shutdown(); Pump(Pending); CollectGarbage();
	EXPECT_EQ(Manager->GetPrimaryAssetObject(Ids[1]), nullptr);
}

TEST_F(FPrimaryAssetTests, DuplicateIdsAreDiagnosedWithBothPathsAndInvalidationRemovesStaleMappings)
{
	Write("A", Id("Scenario:Same"));
	ASSERT_TRUE(Manager->StartInitialLoading());
	Write("B", Id("Scenario:Same"));
	EXPECT_FALSE(Manager->GetPrimaryAssetPath(Id("Scenario:Same")).IsValid());
	const auto Diagnostics = Manager->GetDiagnostics();
	ASSERT_FALSE(Diagnostics.empty());
	EXPECT_NE(Diagnostics[0].find(Path("A").ToString()), std::string::npos);
	EXPECT_NE(Diagnostics[0].find(Path("B").ToString()), std::string::npos);
	ASSERT_TRUE(std::filesystem::remove(Root / "B.dasset"));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(Manager->GetPrimaryAssetPath(Id("Scenario:Same")).GetAssetPath(), Path("A"));
	Write("A", Id("Scenario:Renamed"));
	EXPECT_FALSE(Manager->GetPrimaryAssetPath(Id("Scenario:Same")).IsValid());
	EXPECT_EQ(Manager->GetPrimaryAssetPath(Id("Scenario:Renamed")).GetAssetPath(), Path("A"));
}

TEST_F(FPrimaryAssetTests, RedirectsApplyTypeThenIdAndRejectCycles)
{
	Write("Root", Id("Scenario:Root"));
	FAssetManagerSettings Settings;
	Settings.PrimaryAssetTypeRedirects.emplace(FPrimaryAssetType(FName("Old")), FPrimaryAssetType(FName("Scenario")));
	Settings.PrimaryAssetIdRedirects.emplace(Id("Scenario:Old"), Id("Scenario:Root"));
	ASSERT_TRUE(Manager->StartInitialLoading(Settings));
	EXPECT_EQ(Manager->GetPrimaryAssetPath(Id("Old:Old")).GetAssetPath(), Path("Root"));
	EXPECT_FALSE(Manager->GetPrimaryAssetPath(Id("Old:Missing")).IsValid());
	Manager->Shutdown();
	Settings.PrimaryAssetIdRedirects.emplace(Id("Scenario:Root"), Id("Scenario:Old"));
	EXPECT_FALSE(Manager->StartInitialLoading(Settings));
}

TEST_F(FPrimaryAssetTests, StableLogicalIdsFollowRelocationAndNoWorkCompletesImmediately)
{
	Write("A", Id("Scenario:Stable"));
	ASSERT_TRUE(Manager->StartInitialLoading());
	auto First = Manager->LoadPrimaryAsset(Id("Scenario:Stable")); Pump(First);
	ASSERT_FALSE(First->HasError()); First.reset();
	bool bCalled = false;
	auto NoWork = Manager->LoadPrimaryAsset(Id("Scenario:Stable"), {}, {.OnComplete = [&](FStreamableHandle&) { bCalled = true; }});
	EXPECT_EQ(NoWork, nullptr); EXPECT_TRUE(bCalled);
	ASSERT_TRUE(Manager->UnloadPrimaryAsset(Id("Scenario:Stable"))); CollectGarbage();
	ASSERT_TRUE(std::filesystem::remove(Root / "A.dasset"));
	Write("B", Id("Scenario:Stable"));
	EXPECT_EQ(Manager->GetPrimaryAssetPath(Id("Scenario:Stable")).GetAssetPath(), Path("B"));
	ASSERT_TRUE(std::filesystem::remove(Root / "B.dasset"));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	EXPECT_FALSE(Manager->GetPrimaryAssetPath(Id("Scenario:Stable")).IsValid());
	auto Missing = Manager->LoadPrimaryAsset(Id("Scenario:Stable")); Pump(Missing);
	ASSERT_NE(Missing, nullptr); EXPECT_TRUE(Missing->HasError());
}

TEST_F(FPrimaryAssetTests, DeletingLoadedMetadataDoesNotTurnTheNextLoadIntoStaleNoWork)
{
	Write("Root", Id("Scenario:Root"));
	ASSERT_TRUE(Manager->StartInitialLoading());
	auto First = Manager->LoadPrimaryAsset(Id("Scenario:Root")); Pump(First);
	ASSERT_FALSE(First->HasError()); First.reset();
	ASSERT_TRUE(std::filesystem::remove(Root / "Root.dasset"));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	auto Missing = Manager->LoadPrimaryAsset(Id("Scenario:Root"));
	ASSERT_NE(Missing, nullptr); Pump(Missing); EXPECT_TRUE(Missing->HasError());
	EXPECT_NE(Manager->GetPrimaryAssetObject(Id("Scenario:Root")), nullptr);
}

TEST_F(FPrimaryAssetTests, DuplicateAdmissionAndTypeRedirectCyclesFailExplicitly)
{
	Write("A", Id("Scenario:Same")); Write("B", Id("Scenario:Same"));
	EXPECT_FALSE(Manager->StartInitialLoading());
	ASSERT_TRUE(std::filesystem::remove(Root / "B.dasset"));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FAssetManagerSettings Settings;
	Settings.PrimaryAssetTypeRedirects.emplace(FPrimaryAssetType(FName("A")), FPrimaryAssetType(FName("B")));
	Settings.PrimaryAssetTypeRedirects.emplace(FPrimaryAssetType(FName("B")), FPrimaryAssetType(FName("A")));
	EXPECT_FALSE(Manager->StartInitialLoading(Settings));
}

TEST_F(FPrimaryAssetTests, TypeSettingsAndExplicitRuleDefaultsAreAppliedWithoutLoading)
{
	Write("Root", Id("Scenario:Root")); Write("Other", Id("Other:Other"));
	FAssetManagerSettings Settings;
	Settings.DefaultRules.Priority = 7;
	Settings.PrimaryAssetTypesToScan.push_back({.PrimaryAssetType = FPrimaryAssetType(FName("Scenario")), .AssetBaseClass = "Durin::DObject", .Directories = {"/Primary"}});
	Settings.PrimaryAssetRules[Id("Scenario:Root")].Priority = 0;
	ASSERT_TRUE(Manager->StartInitialLoading(Settings));
	EXPECT_EQ(Manager->GetPrimaryAssetRules(Id("Scenario:Root")).Priority, 0);
	EXPECT_FALSE(Manager->GetPrimaryAssetPath(Id("Other:Other")).IsValid());
	Manager->Shutdown(); Settings.PrimaryAssetTypesToScan[0].bHasBlueprintClasses = true;
	EXPECT_FALSE(Manager->StartInitialLoading(Settings));
}

TEST_F(FPrimaryAssetTests, SettingsStoreRejectsUnsupportedCookAndBlueprintSettings)
{
	const auto File = Root / "Project.yaml";
	const auto Save = [&](std::string_view Text) { return FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text)), File); };
	ASSERT_TRUE(Save("Game:\n  DefaultLevel: /Primary/Map\nAssetManager:\n  Types:\n    - Type: Scenario\n      Directories: [/Primary]\n      Rules: {Priority: 3}\n"));
	FAssetManagerSettings Settings;
	ASSERT_TRUE(FProjectAssetManagerSettingsStore(File).Load(Settings));
	ASSERT_EQ(Settings.PrimaryAssetTypesToScan.size(), 1u);
	EXPECT_EQ(Settings.PrimaryAssetTypesToScan[0].Rules.Priority, 3);
	ASSERT_TRUE(Save("AssetManager:\n  DefaultRules: {CookRule: ProductionNeverCook}\n"));
	EXPECT_FALSE(FProjectAssetManagerSettingsStore(File).Load(Settings));
	ASSERT_TRUE(Save("AssetManager:\n  Types:\n    - {Type: Scenario, BlueprintClasses: true}\n"));
	EXPECT_FALSE(FProjectAssetManagerSettingsStore(File).Load(Settings));
}

TEST(PrimaryAssetValueTests, IdentityAndBundleEncodingRejectMalformedInputs)
{
	FPrimaryAssetId Value;
	for (auto Text : {"", ":A", "A:", "A:B:C", "None:A"}) EXPECT_FALSE(FPrimaryAssetId::TryParse(Text, Value));
	ASSERT_TRUE(FPrimaryAssetId::TryParse("Scenario:Root", Value));
	FAssetBundleData Data;
	ASSERT_TRUE(Data.AddBundleAsset(FName("Game"), Path("B")));
	ASSERT_TRUE(Data.AddBundleAsset(FName("Game"), Path("A")));
	ASSERT_TRUE(Data.AddBundleAsset(FName("Game"), Path("A")));
	ASSERT_EQ(Data.Bundles[0].BundleAssets.size(), 2u);
	FBinaryWriter Writer; WriteAssetManagementMetadata(Writer, Value, Data);
	FBinaryReader Reader(Writer.GetBytes());
	FPrimaryAssetId Loaded; FAssetBundleData LoadedData;
	ASSERT_TRUE(ReadAssetManagementMetadata(Reader, Loaded, LoadedData));
	EXPECT_EQ(Loaded, Value); EXPECT_EQ(LoadedData, Data);
	Data.Bundles.push_back(Data.Bundles[0]);
	FBinaryWriter Bad; WriteAssetManagementMetadata(Bad, Value, Data);
	FBinaryReader BadReader(Bad.GetBytes()); LoadedData = {};
	EXPECT_FALSE(ReadAssetManagementMetadata(BadReader, Loaded, LoadedData));
}
