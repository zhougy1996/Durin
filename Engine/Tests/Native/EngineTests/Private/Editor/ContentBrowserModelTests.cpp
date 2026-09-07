#include "NativeAssetTestSupport.h"
#include "Panels/ContentBrowserModel.h"
#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Operations/ContentBrowserOperationService.h"

#include "Asset/Relocation.h"
#include "AssetRegistry/Publication.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "Thumbnail/AssetThumbnailPool.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Editor::ContentBrowser::Private;

	class FContentBrowserModelTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			Root = Durin::Testing::GetTestWorkDirectory()
				/ "ContentBrowserModel"
				/ testing::UnitTest::GetInstance()->current_test_info()->name();
			std::error_code Ec;
			Durin::Testing::RemoveTestWorkDirectory(Root, Ec);
			std::filesystem::create_directories(Root / "Content/A");
			std::filesystem::create_directories(Root / "Content/B");
			const std::array Definitions{
				FMountPoint{
					.VirtualRoot = "/ContentBrowserTests/",
					.Owner = EMountOwner::Test,
					.Root = Root / "Content",
					.bAutoScan = true,
					.bContentWritable = true}};
			Registry =
				std::make_unique<Testing::FScopedMountRegistryFixture>(
					Definitions);
			ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
			ShutdownAssetManager();
			CollectGarbage();
			ASSERT_TRUE(InitializeAssetManager());
		}

		static void TearDownTestSuite()
		{
			ShutdownAssetManager();
			CollectGarbage();
			ASSERT_TRUE(InitializeAssetManager());
		}

		std::filesystem::path Root;
		std::unique_ptr<Testing::FScopedMountRegistryFixture> Registry;
	};

	// A deterministic filesystem pause proves UI requests return before enumeration
	// finishes. The guard releases it even when a fatal test assertion exits early.
	struct FBrowserScanGate
	{
		std::mutex Mutex;
		std::condition_variable Changed;
		bool bStarted = false;
		bool bReleased = false;
		auto Pause() -> void
		{
			std::unique_lock Lock(Mutex);
			bStarted = true;
			Changed.notify_all();
			Changed.wait(Lock, [&] { return bReleased; });
		}
		auto WaitUntilStarted() -> bool
		{
			std::unique_lock Lock(Mutex);
			return Changed.wait_for(Lock, std::chrono::seconds(5), [&] { return bStarted; });
		}
		auto Release() -> void
		{
			std::lock_guard Lock(Mutex);
			bReleased = true;
			Changed.notify_all();
		}
	};
	struct FBrowserScanRelease
	{
		std::shared_ptr<FBrowserScanGate> Gate;
		~FBrowserScanRelease() { Gate->Release(); }
	};

	auto BuildDeletionPlan(
		FContentBrowserOperationService& Operations,
		std::span<const FContentBrowserItem> Items)
		-> FContentDeletionPlanPtr
	{
		std::unordered_set<std::string> Selection;
		for (const FContentBrowserItem& Item : Items)
			Selection.insert(Item.StableId());
		return Operations.BuildDeletionPlan(Items);
	}

	// Exercises the same service submission used by Panel, including initial freshness.
	struct FBrowserDeletionDriver
	{
		FContentBrowserOperationService& Operations;
		FContentDeletionPlanPtr Plan;
		FContentDeletionHooks Hooks;
		std::string Details;
		auto Execute() -> bool
		{
			const auto Result = Operations.ExecuteDeletion(Plan, Hooks);
			Details = Result.Status.Message;
			return static_cast<bool>(Result);
		}
		auto GetDetails() const -> const std::string& { return Details; }
	};

	class FNoOpEditorTransaction final : public Durin::Editor::ITransactionCustomChange
	{
	public:
		auto GetDescription() const -> std::string_view override { return "No-op"; }
		auto Undo() -> bool override { return true; }
		auto Redo() -> bool override { return true; }
	};

	class FRouteOnlyThumbnailRenderer final : public Editor::DThumbnailRenderer
	{
	public:
		explicit FRouteOnlyThumbnailRenderer(std::string InAssetClassName)
			: AssetClassName(std::move(InAssetClassName))
		{
		}

		auto GetRegistration() const -> Editor::FThumbnailRenderingInfo override
		{
			return {
				.AssetClassName = AssetClassName,
				.RendererName = "ContentBrowserRouteTest",
				.GeneratorSchemaVersion = 1};
		}

		auto CaptureGenerationRequest(
			const Editor::FAssetThumbnailRequest&,
			uint64,
			Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override
		{
			OutRequest = {};
			OutError = "Route-only test renderer.";
			return false;
		}

	private:
		std::string AssetClassName;
	};
} // namespace

TEST_F(FContentBrowserModelTests, MaintainsHistoryAndTruncatesForwardBranch)
{
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/B").generic_string()));
	ASSERT_EQ(Model.GetHistory().size(), 2);
	ASSERT_TRUE(Model.NavigateHistory(-1));
	EXPECT_EQ(Model.GetHistoryIndex(), 0);
	EXPECT_EQ(
		Model.GetCurrentPhysicalPath(),
		std::filesystem::absolute(Root / "Content/A")
			.lexically_normal()
			.generic_string());

	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	EXPECT_EQ(Model.GetHistory().size(), 2);
	EXPECT_EQ(Model.GetHistoryIndex(), 1);
	EXPECT_FALSE(Model.NavigateHistory(1));
}

TEST_F(FContentBrowserModelTests, IndexedProjectionRetainsSourceItemsAcrossRequeryAndDestruction)
{
	static_assert(std::ranges::random_access_range<const FContentBrowserItemRange>);
	static_assert(!std::ranges::contiguous_range<const FContentBrowserItemRange>);
	static_assert(std::same_as<std::ranges::range_reference_t<const FContentBrowserItemRange>,
		const FContentBrowserItem&>);
	FContentBrowserItemRange Original;
	const FContentBrowserItem* First = nullptr;
	{
		FContentBrowserModel Model;
		const auto Directory = (Root / "Content").generic_string();
		Model.SetSnapshotForTesting(Directory, {
			{.Name = "B", .PhysicalPath = Directory + "/B", .FileSize = 10},
			{.Name = "A", .PhysicalPath = Directory + "/A", .FileSize = 20}});
		Original = Model.GetItems();
		ASSERT_EQ(Original.size(), 2);
		EXPECT_EQ(Original.GetIndices()[0], 1);
		First = &Original.front();
		EXPECT_EQ(First, &Model.GetItemsSnapshot()[1]);
		Model.SetSort(EContentBrowserSortColumn::Size, true);
		EXPECT_EQ(Model.GetItems().front().Name, "B");
		EXPECT_EQ(&Model.GetItems()[1], First);
		EXPECT_EQ(Original.front().Name, "A");
		Model.SetSearch(Directory + "/B");
		ASSERT_EQ(Model.GetItems().size(), 1);
		EXPECT_EQ(&Model.GetItems().front(), &Model.GetItemsSnapshot()[0]);
		Model.SetSnapshotForTesting(Directory, {});
		EXPECT_TRUE(Model.GetItems().empty());
	}
	ASSERT_EQ(Original.size(), 2);
	EXPECT_EQ(&Original.front(), First);
	EXPECT_EQ(Original.front().Name, "A");
	EXPECT_EQ(std::distance(Original.begin(), Original.end()), 2);
	EXPECT_EQ((Original.end() - 1)->Name, "B");
}

TEST_F(FContentBrowserModelTests, QueryKeepsStableTiesAndCopiesOnlySelectedCommandInputs)
{
	auto Snapshot = std::make_shared<FContentBrowserItemsSnapshot>();
	Snapshot->Items = {
		{.Name = "Same", .PhysicalPath = "/content/first", .Extension = ".txt"},
		{.Name = "Same", .PhysicalPath = "/content/second", .Extension = ".txt"},
		{.Kind = EContentBrowserItemKind::Folder, .Name = "Folder", .PhysicalPath = "/content/folder"},
		{.Name = "Hidden", .PhysicalPath = "/content/.hidden/entry"},
		{.Name = "Outside", .PhysicalPath = "/outside/entry"}};
	FContentBrowserQuerySettings Query;
	Query.SortColumn = EContentBrowserSortColumn::Type;
	Query.bSortAscending = false;
	const auto Projection = ContentBrowserQuery::Project(Snapshot, "/content", Query);
	ASSERT_EQ(Projection.size(), 3);
	EXPECT_EQ(Projection[0].Name, "Folder");
	EXPECT_EQ(Projection[1].PhysicalPath, "/content/first");
	EXPECT_EQ(Projection[2].PhysicalPath, "/content/second");
	const auto Selected = ContentBrowserQuery::CopySelection(Projection,
		{"/content/second", "/content/.hidden/entry"});
	ASSERT_EQ(Selected.size(), 1);
	EXPECT_EQ(Selected.front().PhysicalPath, "/content/second");
	Query.Search = "Hidden";
	EXPECT_TRUE(ContentBrowserQuery::Project(Snapshot, "/content", Query).empty());
	Query.bShowHiddenFiles = true;
	EXPECT_EQ(ContentBrowserQuery::Project(Snapshot, "/content", Query).size(), 1);
	EXPECT_TRUE(ContentBrowserQuery::Project({}, "/content", Query).empty());
}

TEST_F(FContentBrowserModelTests, RepeatedNavigationToCurrentDirectoryKeepsPublishedSnapshot)
{
	FContentBrowserModel Model;
	const std::string Directory =
		std::filesystem::absolute(Root / "Content/A")
			.lexically_normal()
			.generic_string();
	ASSERT_TRUE(Model.NavigateToPhysical(Directory));
	Model.SetSnapshotForTesting(Directory, {
		{.Kind = EContentBrowserItemKind::File,
			.Name = "Stable.txt",
			.PhysicalPath = Directory + "/Stable.txt",
			.Extension = ".txt"},
	});

	ASSERT_TRUE(Model.NavigateToPhysical(Directory));
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Stable.txt");
	EXPECT_EQ(Model.GetHistory().size(), 1);
}

TEST_F(FContentBrowserModelTests, DirectoryNavigationPreservesTreeChildrenSnapshots)
{
	FContentBrowserModel Model;
	const std::string TreeRoot =
		std::filesystem::absolute(Root / "Content")
			.lexically_normal()
			.generic_string();
	Model.RefreshMountSnapshot();
	Model.RequestDirectoryChildrenSnapshot(TreeRoot);
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
	ASSERT_EQ(Model.GetDirectoryChildren(TreeRoot).size(), 2);

	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	EXPECT_TRUE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
	EXPECT_EQ(Model.GetDirectoryChildren(TreeRoot).size(), 2);
}

TEST_F(FContentBrowserModelTests, RejectsUnavailableDirectoryWithoutFilesystemException)
{
	const std::filesystem::path Unavailable = Root / "Content/B";
	FContentBrowserModel Model;
	Model.SetPathStatusQueryForTesting(
		[Unavailable](const std::filesystem::path& Path, std::error_code& Error) {
			if (Path == Unavailable)
			{
				Error = std::make_error_code(std::errc::permission_denied);
				return std::filesystem::file_status{};
			}
			return std::filesystem::status(Path, Error);
		});

	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	EXPECT_FALSE(Model.NavigateToPhysical(Unavailable.generic_string()));
	EXPECT_EQ(
		Model.FindNearestAvailableDirectory((Unavailable / "RemovedChild").generic_string()),
		std::filesystem::absolute(Root / "Content")
			.lexically_normal()
			.generic_string());
}

TEST_F(FContentBrowserModelTests, RoutesStaticMeshAssetsToThumbnails)
{
	InitializeDObjectSystem();
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/ThumbnailMesh", AssetPath));
	const Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Imported = AssetForge::Builtins::ImportStaticMeshForTest(
		(std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "MultiSection.gltf").generic_string(), AssetPath.ToString());
	ASSERT_TRUE(Imported) << Imported.Message;
	DStaticMesh* StaticMesh = Imported.Asset;
	const FAssetCatalogEntry AssetData =
		FindAssetExact(AssetPath);
	ASSERT_NE(AssetData, nullptr);
	std::string RegistrationError;
	auto ThumbnailRegistration =
		Editor::GetDefaultThumbnailManager().RegisterScoped(
			std::make_unique<FRouteOnlyThumbnailRenderer>(AssetData->AssetClassName),
			RegistrationError);
	ASSERT_TRUE(ThumbnailRegistration) << RegistrationError;

	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const auto It = std::ranges::find_if(
		Model.GetItems(),
		[&](const FContentBrowserItem& Item) {
			return Item.PackagePath == AssetPath;
		});
	ASSERT_NE(It, Model.GetItems().end());
	const std::string ExactAssetPath =
		Testing::MakePackageLeafTopLevelAssetPathForTests(AssetPath).ToString();
	EXPECT_EQ(It->VirtualPath, ExactAssetPath);
	EXPECT_EQ(It->ThumbnailIdentity, ExactAssetPath);
	EXPECT_TRUE(It->ThumbnailSourcePath.empty());
	EXPECT_EQ(It->ThumbnailFileSize, AssetData->FileSize);
	EXPECT_EQ(It->ThumbnailPackageFormatVersion, AssetData->FormatVersion);
	EXPECT_EQ(It->ThumbnailLastWriteTimeTicks, AssetData->LastWriteTimeTicks);

	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, RejectsExcludedMountsAndClearsStaleCurrentDirectory)
{
	Registry.reset();
	std::filesystem::create_directories(Root / "Excluded");
	const std::array Definitions{
		FMountPoint{
			.VirtualRoot = "/ContentBrowserTests/",
			.Owner = EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bContentWritable = true},
		FMountPoint{
			.VirtualRoot = "/ContentBrowserExcluded/",
			.Owner = EMountOwner::Test,
			.Root = Root / "Excluded",
			.bAutoScan = false,
			.bContentWritable = true}};
	Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(
		Definitions);
	ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	EXPECT_FALSE(Model.NavigateToPhysical((Root / "Excluded").generic_string()));
	EXPECT_TRUE(Model.PhysicalToVirtualDirectory(
		(Root / "Excluded").generic_string()).empty());
	EXPECT_EQ(
		Model.GetCurrentPhysicalPath(),
		std::filesystem::absolute(Root / "Content")
			.lexically_normal()
			.generic_string());

	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserOperationResult CreateExcluded =
		Operations.CreateFolder((Root / "Excluded").generic_string());
	EXPECT_FALSE(CreateExcluded);
	EXPECT_TRUE(CreateExcluded.Status.Message.find("automatically scanned")
		!= std::string::npos);

	Registry.reset();
	std::filesystem::create_directories(Root / "Replacement");
	const std::array ReplacementDefinitions{
		FMountPoint{
			.VirtualRoot = "/ContentBrowserReplacement/",
			.Owner = EMountOwner::Test,
			.Root = Root / "Replacement",
			.bAutoScan = true,
			.bContentWritable = true}};
	Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(
		ReplacementDefinitions);
	ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
	Model.RefreshMountSnapshot();
	EXPECT_TRUE(Model.GetCurrentPhysicalPath().empty());
	EXPECT_FALSE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	EXPECT_TRUE(Model.NavigateToPhysical((Root / "Replacement").generic_string()));
}

TEST_F(FContentBrowserModelTests, ListsGameMountBeforeEngineMount)
{
	Registry.reset();
	std::filesystem::create_directories(Root / "EngineContent");
	std::filesystem::create_directories(Root / "GameContent");
	const std::array Definitions{
		FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = EMountOwner::Engine,
			.Root = Root / "EngineContent",
			.bAutoScan = true},
		FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = EMountOwner::ActiveProject,
			.Root = Root / "GameContent",
			.bAutoScan = true}};
	Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(
		Definitions);
	ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	ASSERT_EQ(Model.GetMounts().size(), 2u);
	EXPECT_EQ(Model.GetMounts()[0].VirtualRoot, "/Game/");
	EXPECT_EQ(Model.GetMounts()[1].VirtualRoot, "/Engine/");
}

TEST_F(FContentBrowserModelTests, RelocationDoesNotEnterEditorUndoHistory)
{
	InitializeDObjectSystem();
	FPackagePath SourcePath;
	FPackagePath DestinationPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/ForwardJobSource", SourcePath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/Folder/ForwardJobDestination",
		DestinationPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	const FAssetRelocation Mapping{
		SourcePath, DestinationPath};
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(IAssetTools::Get().RelocateAssets({
		.Mappings = {Mapping}}));
	EXPECT_TRUE(Transactions->GetUndoDescription().empty());
	EXPECT_EQ(ResolveAssetPath(SourcePath).FinalPath,
		DestinationPath);

	EXPECT_FALSE(Transactions->Undo());
	Transactions->Reset();
}

TEST_F(FContentBrowserModelTests, SearchesRecursivelyButBrowsesImmediateChildren)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FContentBrowserModel Model;
	Model.SetSnapshotForTesting(
		RootPath,
		{
			{.Kind = EContentBrowserItemKind::Folder,
				.Name = "Textures",
				.PhysicalPath = RootPath + "/Textures"},
			{.Kind = EContentBrowserItemKind::Asset,
				.Name = "Stone",
				.VirtualPath = "/ContentBrowserTests/Textures/Stone",
				.PhysicalPath = RootPath + "/Textures/Stone.dasset",
				.AssetClassName = "Durin::DTexture2D"},
		});

	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Textures");
	Model.SetSearch("stone");
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Stone");
}

TEST_F(FContentBrowserModelTests, RevealAssetClearsFiltersAndPublishesTarget)
{
	InitializeDObjectSystem();
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/A/RevealTarget", AssetPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	Model.SetSearch("does-not-match");
	ASSERT_TRUE(Model.GetItems().empty());

	const std::string Revealed = Model.RevealAsset(
		Testing::MakePackageLeafTopLevelAssetPathForTests(AssetPath).ToString());

	EXPECT_FALSE(Revealed.empty());
	EXPECT_TRUE(Model.GetSearch().empty());
	EXPECT_EQ(Model.GetTypeFilter(), EContentBrowserTypeFilter::All);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[&](const FContentBrowserItem& Item) {
			return Item.StableId() == Revealed;
		}));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, SingleAssetPackageUsesPackageNameAfterRelocation)
{
	InitializeDObjectSystem();
	FPackagePath SourcePath;
	FPackagePath DestinationPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/DisplayNameSource", SourcePath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/DisplayNameDestination", DestinationPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(IAssetTools::Get().RelocateAssets({
		.Mappings = {{SourcePath, DestinationPath}}}));

	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const FTopLevelAssetPath AssetPath =
		Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath);
	FTopLevelAssetPath RelocatedAssetPath;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(
		DestinationPath, AssetPath.GetAssetName(), RelocatedAssetPath));
	const std::string Revealed = Model.RevealAsset(RelocatedAssetPath.ToString());
	ASSERT_FALSE(Revealed.empty());
	const auto It = std::ranges::find_if(
		Model.GetItems(), [&](const FContentBrowserItem& Item) {
			return Item.StableId() == Revealed;
		});
	ASSERT_NE(It, Model.GetItems().end());
	EXPECT_EQ(It->Name, "DisplayNameDestination");
	EXPECT_EQ(It->VirtualPath, RelocatedAssetPath.ToString());
	EXPECT_TRUE(Testing::RemoveAssetPackageForTests(SourcePath));
	EXPECT_TRUE(Testing::RemoveAssetPackageForTests(DestinationPath));
}

TEST_F(FContentBrowserModelTests, RevealPhysicalItemPublishesNewFolderHiddenByFilters)
{
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	Model.SetSearch("does-not-match");
	ASSERT_TRUE(Model.GetItems().empty());

	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserOperationResult CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string());
	ASSERT_TRUE(CreateResult) << CreateResult.Status.Message;

	const std::string Revealed =
		Model.RevealPhysicalItem(CreateResult.FocusPhysicalPath);

	EXPECT_EQ(Revealed, CreateResult.FocusPhysicalPath);
	EXPECT_TRUE(Model.GetSearch().empty());
	EXPECT_EQ(Model.GetTypeFilter(), EContentBrowserTypeFilter::All);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[&](const FContentBrowserItem& Item) {
			return Item.Kind == EContentBrowserItemKind::Folder
				&& Item.StableId() == Revealed;
		}));
}

TEST_F(FContentBrowserModelTests, DefersRecursiveEnumerationUntilSearchStarts)
{
	const std::filesystem::path ScanRoot = Root / "Content/DeferredSearch";
	std::filesystem::create_directories(ScanRoot / "Nested");
	{
		std::ofstream DirectFile(ScanRoot / "Direct.txt");
		DirectFile << "direct";
		std::ofstream NestedFile(ScanRoot / "Nested/Needle.txt");
		NestedFile << "needle";
	}

	FContentBrowserModel Model;
	bool bObservedNestedFile = false;
	Model.SetEntryStatusQueryForTesting(
		[&](const std::filesystem::directory_entry& Entry,
			std::error_code& Error) {
			if (Entry.path().filename() == "Needle.txt")
				bObservedNestedFile = true;
			return Entry.symlink_status(Error);
		});

	ASSERT_TRUE(Model.NavigateToPhysical(ScanRoot.generic_string()));
	EXPECT_FALSE(bObservedNestedFile);
	EXPECT_EQ(std::ranges::count_if(
		Model.GetItems(),
		[](const FContentBrowserItem& Item) { return Item.Name == "Needle.txt"; }), 0);

	Model.SetSearch("needle");
	EXPECT_TRUE(bObservedNestedFile);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Needle.txt");

	bObservedNestedFile = false;
	Model.SetSearch(std::string_view{});
	EXPECT_FALSE(bObservedNestedFile);
}

TEST_F(FContentBrowserModelTests, ShowsOrdinaryFilesByDefaultAndHidesAssetPackages)
{
	{
		std::ofstream RawSource(Root / "Content/Raw.png");
		RawSource << "raw";
		std::ofstream AssetPackage(Root / "Content/Raw.dasset");
		AssetPackage << "package";
	}
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const auto File = std::ranges::find(Model.GetItems(), "Raw.png", &FContentBrowserItem::Name);
	ASSERT_NE(File, Model.GetItems().end());
	EXPECT_EQ(File->Kind, EContentBrowserItemKind::File);
	EXPECT_EQ(File->Extension, ".png");
	EXPECT_EQ(File->FileSize, 3);
	EXPECT_TRUE(std::ranges::none_of(
		Model.GetItems(),
		[](const FContentBrowserItem& Item) { return Item.Name == "Raw.dasset"; }));
}

TEST_F(FContentBrowserModelTests, SkipsFailedSnapshotEntryAndKeepsLaterEntries)
{
	const std::filesystem::path ScanRoot = Root / "Content/EnumerationSnapshot";
	std::filesystem::create_directories(ScanRoot);
	for (const std::string_view Name : {"First.txt", "Second.txt", "Third.txt"})
	{
		std::ofstream File(ScanRoot / Name);
		File << Name;
	}
	FContentBrowserModel Model;
	size_t QueryCount = 0;
	Model.SetEntryStatusQueryForTesting(
		[&](const std::filesystem::directory_entry& Entry,
			std::error_code& Error) {
			if (++QueryCount == 1)
			{
				Error = std::make_error_code(std::errc::permission_denied);
				return std::filesystem::file_status{};
			}
			return Entry.symlink_status(Error);
		});
	ASSERT_TRUE(Model.NavigateToPhysical(ScanRoot.generic_string()));
	EXPECT_EQ(Model.GetItems().size(), 2);
	ASSERT_EQ(Model.GetEnumerationDiagnostics().size(), 1);
	EXPECT_EQ(
		Model.GetEnumerationDiagnostics().front().Kind,
		FContentBrowserModel::EEnumerationDiagnosticKind::Entry);
	EXPECT_TRUE(Model.GetEnumerationDiagnostics().front().Message.find("Skipped entry")
		!= std::string::npos);
}

TEST_F(FContentBrowserModelTests, SkipsFailedTreeEntryAndCachesDirectorySnapshot)
{
	const std::filesystem::path TreeRoot = Root / "Content/EnumerationTree";
	for (const std::string_view Name : {"First", "Second", "Third"})
		std::filesystem::create_directories(TreeRoot / Name);
	size_t QueryCount = 0;
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
	Model.SetEntryStatusQueryForTesting(
		[&](const std::filesystem::directory_entry& Entry,
			std::error_code& Error) {
			if (Entry.path().parent_path() == TreeRoot && ++QueryCount == 1)
			{
				Error = std::make_error_code(std::errc::permission_denied);
				return std::filesystem::file_status{};
			}
			return Entry.symlink_status(Error);
		});
	Model.RequestDirectoryChildrenSnapshot(TreeRoot.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	EXPECT_EQ(Model.GetDirectoryChildren(TreeRoot.generic_string()).size(), 2);
	ASSERT_EQ(Model.GetEnumerationDiagnostics().size(), 1);
	EXPECT_EQ(
		Model.GetEnumerationDiagnostics().front().Kind,
		FContentBrowserModel::EEnumerationDiagnosticKind::Entry);
	EXPECT_TRUE(Model.GetDirectoryChildren(
		(Root / "MissingDirectory").generic_string()).empty());
	EXPECT_EQ(Model.GetEnumerationDiagnostics().size(), 1);
}

TEST_F(FContentBrowserModelTests, KeepsPublishedChildrenStableWhileDistinctSnapshotsGrowCache)
{
	const std::filesystem::path TreeRoot = Root / "Content/StableTreeCache";
	constexpr size_t BranchCount = 256;
	std::vector<std::filesystem::path> Branches;
	Branches.reserve(BranchCount);
	for (size_t Index = 0; Index < BranchCount; ++Index)
	{
		std::filesystem::path Branch = TreeRoot / std::format("Branch{:03}", Index);
		std::filesystem::create_directories(Branch);
		Branches.push_back(std::move(Branch));
	}

	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
	Model.RequestDirectoryChildrenSnapshot(TreeRoot.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const std::span<const std::filesystem::path> PublishedChildren =
		Model.GetDirectoryChildren(TreeRoot.generic_string());
	ASSERT_EQ(PublishedChildren.size(), BranchCount);
	const std::filesystem::path* PublishedStorage = PublishedChildren.data();
	const std::vector<std::filesystem::path> ExpectedChildren(
		PublishedChildren.begin(), PublishedChildren.end());

	for (const std::filesystem::path& Branch : Branches)
		Model.RequestDirectoryChildrenSnapshot(Branch.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();

	EXPECT_EQ(PublishedChildren.data(), PublishedStorage);
	EXPECT_TRUE(std::ranges::equal(PublishedChildren, ExpectedChildren));
}

TEST_F(FContentBrowserModelTests, AsyncPublishedSnapshotsSurviveTaskResetRefreshAndModelDestruction)
{
	InitializeDObjectSystem();
	const auto Directory = (Root / "Content/Owned").generic_string();
	std::filesystem::create_directories(std::filesystem::path(Directory) / "Original");
	FContentBrowserItemRange RetainedItems;
	std::shared_ptr<const FContentBrowserDirectorySnapshot> RetainedTree;
	{
		FContentBrowserModel Model(true);
		ASSERT_TRUE(Model.NavigateToPhysical(Directory));
		Model.RequestDirectoryChildrenSnapshot(Directory);
		Model.WaitForPendingSnapshotsForTesting();
		RetainedItems = Model.GetItems();
		RetainedTree = Model.GetDirectorySnapshot(Directory);
		ASSERT_EQ(RetainedItems.size(), 1);
		ASSERT_TRUE(RetainedTree);
		ASSERT_EQ(RetainedTree->Children.size(), 1);
		EXPECT_GT(RetainedTree->Version, 0);
		EXPECT_FALSE(Model.PumpPendingSnapshots());

		std::filesystem::create_directories(std::filesystem::path(Directory) / "Next");
		Model.RefreshItemsSnapshot();
		Model.RequestDirectoryChildrenSnapshot(Directory);
		Model.WaitForPendingSnapshotsForTesting();
		EXPECT_EQ(Model.GetItems().size(), 2);
		const auto NextTree = Model.GetDirectorySnapshot(Directory);
		ASSERT_TRUE(NextTree);
		EXPECT_EQ(NextTree->Children.size(), 2);
		EXPECT_GT(NextTree->Version, RetainedTree->Version);
	}
	ASSERT_EQ(RetainedItems.size(), 1);
	EXPECT_EQ(RetainedItems.front().Name, "Original");
	EXPECT_EQ(RetainedItems.front().VirtualPath, "/ContentBrowserTests/Owned/Original/");
	ASSERT_EQ(RetainedTree->Children.size(), 1);
	EXPECT_EQ(RetainedTree->Children.front().filename(), "Original");
}

TEST_F(FContentBrowserModelTests, AsyncNavigationCancelsAndCoalescesOldRequests)
{
	InitializeDObjectSystem();
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/B";
	const auto C = Root / "Content/C";
	std::filesystem::create_directories(A / "Old");
	std::filesystem::create_directories(B / "Skipped");
	std::filesystem::create_directories(C / "Latest");
	auto Gate = std::make_shared<FBrowserScanGate>();
	auto SkippedVisits = std::make_shared<std::atomic<int>>(0);
	FContentBrowserModel Model(true);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate, SkippedVisits, A, B](const auto& Entry, auto& Error) {
		if (Entry.path().parent_path() == A) Gate->Pause();
		if (Entry.path().parent_path() == B) ++*SkippedVisits;
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	EXPECT_TRUE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	ASSERT_TRUE(Model.NavigateToPhysical(B.generic_string()));
	ASSERT_TRUE(Model.NavigateToPhysical(C.generic_string()));
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	ASSERT_FALSE(Model.IsLoading());
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Latest");
	EXPECT_EQ(SkippedVisits->load(), 0);
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), C.generic_string());
}

TEST_F(FContentBrowserModelTests, AsyncSearchPublishesUsingLatestQueryWithoutRestartingCapture)
{
	InitializeDObjectSystem();
	const auto A = Root / "Content/A";
	std::filesystem::create_directories(A / "Nested/Needle");
	FContentBrowserModel Model(true);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Model.WaitForPendingSnapshotsForTesting();
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	Model.SetSearch("unmatched");
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const uint64 Generation = Model.GetRequestGeneration();
	Model.SetSearch("Needle");
	Model.SetSort(EContentBrowserSortColumn::Name, false);
	EXPECT_EQ(Model.GetRequestGeneration(), Generation);
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Needle");
}

TEST_F(FContentBrowserModelTests, AsyncTreeDiscardsResultsInvalidatedDuringEnumeration)
{
	InitializeDObjectSystem();
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Old");
	FContentBrowserModel Model(true);
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	(void)Model.PumpPendingSnapshots();
	ASSERT_TRUE(Gate->WaitUntilStarted());
	Model.CancelPendingSnapshots();
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	Model.SetEntryStatusQueryForTesting({});
	std::filesystem::create_directory(A / "New");
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	Model.WaitForPendingSnapshotsForTesting();
	const auto Published = Model.GetDirectorySnapshot(A.generic_string());
	ASSERT_NE(Published, nullptr);
	EXPECT_EQ(Published->Children.size(), 2);
	Model.CancelPendingSnapshots();
	EXPECT_EQ(Published->Children.size(), 2);
}

TEST_F(FContentBrowserModelTests, AsyncScanFailureTerminatesLoadingAndAllowsRetry)
{
	InitializeDObjectSystem();
	std::filesystem::create_directory(Root / "Content/A/Child");
	FContentBrowserModel Model(true);
	Model.SetEntryStatusQueryForTesting([](const auto&, auto&) -> std::filesystem::file_status {
		throw std::runtime_error("injected capture failure");
	});
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	ASSERT_FALSE(Model.GetEnumerationDiagnostics().empty());
	EXPECT_NE(Model.GetEnumerationDiagnostics().front().Message.find("injected capture failure"), std::string::npos);
	Model.SetEntryStatusQueryForTesting({});
	Model.RefreshItemsSnapshot();
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_TRUE(Model.GetEnumerationDiagnostics().empty());
	ASSERT_EQ(Model.GetItems().size(), 1);
}

TEST_F(FContentBrowserModelTests, AsyncRejectedAdmissionDoesNotFallBackToSynchronousEnumeration)
{
	InitializeDObjectSystem();
	auto Scope = CreateTaskScope();
	ASSERT_TRUE(Scope.IsValid());
	(void)Scope.Close(ETaskScopeCloseMode::Cancel);
	(void)Scope.Wait();
	FContentBrowserModel Model(true, Scope.GetToken());
	auto Visits = std::make_shared<std::atomic<int>>(0);
	Model.SetEntryStatusQueryForTesting([Visits](const auto& Entry, auto& Error) {
		++*Visits;
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	Model.RequestDirectoryChildrenSnapshot((Root / "Content").generic_string());
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Visits->load(), 0);
	EXPECT_FALSE(Model.GetEnumerationDiagnostics().empty());
}

TEST_F(FContentBrowserModelTests, AsyncRevealReturnsIdentityBeforeDirectoryCaptureCompletes)
{
	InitializeDObjectSystem();
	const auto Folder = Root / "Content/A/Revealed";
	std::filesystem::create_directory(Folder);
	FContentBrowserModel Model(true);
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	EXPECT_EQ(Model.RevealPhysicalItem(Folder.generic_string()), Folder.generic_string());
	ASSERT_TRUE(Gate->WaitUntilStarted());
	EXPECT_TRUE(Model.IsLoading());
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().StableId(), Folder.generic_string());
}

TEST_F(FContentBrowserModelTests, RetainedDirectorySnapshotSurvivesInvalidationAndModelDestruction)
{
	std::shared_ptr<const FContentBrowserDirectorySnapshot> Published;
	const std::string TreeRoot = (Root / "Content").generic_string();
	{
		FContentBrowserModel Model;
		ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot));
		Model.RequestDirectoryChildrenSnapshot(TreeRoot);
		Model.RefreshRequestedDirectoryChildrenSnapshots();
		Published = Model.GetDirectorySnapshot(TreeRoot);
		ASSERT_NE(Published, nullptr);
		ASSERT_EQ(Published->Children.size(), 2);
		std::filesystem::create_directory(Root / "Content/C");
		Model.RefreshItemsSnapshot();
		EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
		Model.RequestDirectoryChildrenSnapshot(TreeRoot);
		Model.RefreshRequestedDirectoryChildrenSnapshots();
		const auto Next = Model.GetDirectorySnapshot(TreeRoot);
		ASSERT_NE(Next, nullptr);
		EXPECT_GT(Next->Version, Published->Version);
		EXPECT_EQ(Next->Children.size(), 3);
	}
	EXPECT_EQ(Published->Children.size(), 2);
	EXPECT_EQ(Published->Children.front().filename(), "A");
}

TEST_F(FContentBrowserModelTests, SearchEditsAndViewSettingsReuseCapturedData)
{
	std::filesystem::create_directories(Root / "Content/A/Nested");
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const uint64 DirectoryVersion = Model.GetSnapshotVersion();
	Model.SetSearch("Nested");
	const uint64 RecursiveVersion = Model.GetSnapshotVersion();
	EXPECT_GT(RecursiveVersion, DirectoryVersion);
	ASSERT_EQ(Model.GetItems().size(), 1);
	Model.SetSearch("Nest");
	Model.SetSort(EContentBrowserSortColumn::Modified, false);
	Model.SetShowHiddenFiles(true);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	EXPECT_EQ(Model.GetSnapshotVersion(), RecursiveVersion);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Nested");
}

TEST_F(FContentBrowserModelTests, StagesDeepTreeRequestsWithoutInvalidatingAncestorSpan)
{
	const std::filesystem::path TreeRoot = Root / "Content/DeepStableTreeCache";
	constexpr size_t Depth = 16;
	std::vector<std::filesystem::path> Chain;
	Chain.reserve(Depth);
	std::filesystem::path Parent = TreeRoot;
	for (size_t Level = 0; Level < Depth; ++Level)
	{
		const size_t SiblingCount = Level == 0 ? 64 : 8;
		for (size_t Sibling = 0; Sibling < SiblingCount; ++Sibling)
			std::filesystem::create_directories(
				Parent / std::format("Sibling{:02}", Sibling));
		std::filesystem::path Child = Parent / std::format("Chain{:02}", Level);
		std::filesystem::create_directories(Child);
		Chain.push_back(Child);
		Parent = std::move(Child);
	}

	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
	size_t EntryQueryCount = 0;
	Model.SetEntryStatusQueryForTesting(
		[&](const std::filesystem::directory_entry& Entry,
			std::error_code& Error) {
			++EntryQueryCount;
			return Entry.symlink_status(Error);
		});

	Model.RequestDirectoryChildrenSnapshot(TreeRoot.generic_string());
	EXPECT_EQ(EntryQueryCount, 0);
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(TreeRoot.generic_string()));
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const std::span<const std::filesystem::path> RootChildren =
		Model.GetDirectoryChildren(TreeRoot.generic_string());
	ASSERT_EQ(RootChildren.size(), 65);
	const std::filesystem::path* RootStorage = RootChildren.data();
	const std::vector<std::filesystem::path> ExpectedRootChildren(
		RootChildren.begin(), RootChildren.end());

	for (const std::filesystem::path& Directory : Chain)
	{
		const size_t QueriesBeforeRequest = EntryQueryCount;
		Model.RequestDirectoryChildrenSnapshot(Directory.generic_string());
		EXPECT_EQ(EntryQueryCount, QueriesBeforeRequest);
		EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(Directory.generic_string()));
		EXPECT_EQ(RootChildren.data(), RootStorage);
		EXPECT_TRUE(std::ranges::equal(RootChildren, ExpectedRootChildren));

		Model.RefreshRequestedDirectoryChildrenSnapshots();
		EXPECT_TRUE(Model.HasDirectoryChildrenSnapshot(Directory.generic_string()));
		EXPECT_EQ(RootChildren.data(), RootStorage);
		EXPECT_TRUE(std::ranges::equal(RootChildren, ExpectedRootChildren));
	}
}

TEST_F(FContentBrowserModelTests, FiltersFilesSeparatelyAndKeepsHiddenContentOptIn)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FContentBrowserModel Model;
	Model.SetSnapshotForTesting(
		RootPath,
		{
			{.Kind = EContentBrowserItemKind::Folder,
				.Name = ".internal",
				.PhysicalPath = RootPath + "/.internal"},
			{.Kind = EContentBrowserItemKind::Asset,
				.Name = "Stone",
				.PhysicalPath = RootPath + "/Stone.dasset",
				.AssetClassName = "Durin::DTexture2D"},
			{.Kind = EContentBrowserItemKind::File,
				.Name = "Stone.png",
				.PhysicalPath = RootPath + "/Stone.png",
				.Extension = ".png"},
			{.Kind = EContentBrowserItemKind::File,
				.Name = "notes.txt",
				.PhysicalPath = RootPath + "/.internal/notes.txt",
				.Extension = ".txt"},
		});

	ASSERT_EQ(Model.GetItems().size(), 2);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "Stone.png");
	Model.SetShowHiddenFiles(true);
	ASSERT_EQ(Model.GetItems().size(), 2);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[](const FContentBrowserItem& Item) { return Item.Name == ".internal"; }));
	Model.SetSearch("notes");
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().Name, "notes.txt");
}

TEST_F(FContentBrowserModelTests, HidesRedirectorsButPreservesFoldersAndSupportsExplicitFilter)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FObjectPath Destination;
	ASSERT_TRUE(FObjectPath::TryCreate(
		"/ContentBrowserTests/Final/Stone.Stone", Destination));
	FPackagePath RedirectorPackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/OldStone", RedirectorPackagePath));
	FContentBrowserModel Model;
	Model.SetSnapshotForTesting(
		RootPath,
		{
			{.Kind = EContentBrowserItemKind::Folder,
				.Name = "Aliases",
				.VirtualPath = "/ContentBrowserTests/Aliases",
				.PhysicalPath = RootPath + "/Aliases"},
			{.Kind = EContentBrowserItemKind::Redirector,
				.Name = "OldStone",
				.VirtualPath = "/ContentBrowserTests/OldStone.OldStone",
				.PackagePath = RedirectorPackagePath,
				.PhysicalPath = RootPath + "/OldStone.dasset",
				.AssetClassName = "Durin::DAssetRedirector",
				.RedirectDestination = Destination},
		});

	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(Model.GetItems().front().Kind, EContentBrowserItemKind::Folder);
	Model.SetShowRedirectors(true);
	ASSERT_EQ(Model.GetItems().size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(), [](const FContentBrowserItem& Item) {
			return Item.Kind == EContentBrowserItemKind::Redirector;
		}));
	Model.SetShowRedirectors(false);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Redirectors);
	ASSERT_EQ(Model.GetItems().size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(), [](const FContentBrowserItem& Item) {
			return Item.Kind == EContentBrowserItemKind::Redirector
				&& Item.Name == "OldStone";
		}));
	Model.SetSearch("Final/Stone");
	ASSERT_EQ(Model.GetItems().size(), 1u);
}

TEST_F(FContentBrowserModelTests, KeepsFoldersFirstAndSortsEqualKeysStably)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FContentBrowserModel Model;
	Model.SetSnapshotForTesting(
		RootPath,
		{
			{.Kind = EContentBrowserItemKind::Asset,
				.Name = "Same",
				.PhysicalPath = RootPath + "/second.dasset",
				.AssetClassName = "Durin::DMaterial",
				.FileSize = 64},
			{.Kind = EContentBrowserItemKind::Folder,
				.Name = "Folder",
				.PhysicalPath = RootPath + "/Folder"},
			{.Kind = EContentBrowserItemKind::Asset,
				.Name = "Same",
				.PhysicalPath = RootPath + "/first.dasset",
				.AssetClassName = "Durin::DMaterial",
				.FileSize = 64},
		});
	Model.SetSort(EContentBrowserSortColumn::Size, true);

	ASSERT_EQ(Model.GetItems().size(), 3);
	EXPECT_EQ(Model.GetItems()[0].Kind, EContentBrowserItemKind::Folder);
	EXPECT_TRUE(Model.GetItems()[1].PhysicalPath.ends_with("second.dasset"));
	EXPECT_TRUE(Model.GetItems()[2].PhysicalPath.ends_with("first.dasset"));
}

TEST_F(FContentBrowserModelTests, OperationsRejectCollisionsAndUnmanagedFolders)
{
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});

	const std::filesystem::path Source = Root / "Content/source.txt";
	const std::filesystem::path Collision = Root / "Content/taken.txt";
	{
		std::ofstream SourceFile(Source);
		SourceFile << "source";
		std::ofstream CollisionFile(Collision);
		CollisionFile << "collision";
	}
	const FContentBrowserItem SourceItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "source.txt",
		.PhysicalPath = Source.generic_string(),
		.Extension = ".txt"};
	const FContentBrowserOperationResult CollisionResult =
		Operations.Rename(SourceItem, "taken.txt");
	EXPECT_FALSE(CollisionResult);
	EXPECT_EQ(CollisionResult.Status.Message, "An item with that name already exists.");

	const std::filesystem::path Folder = Root / "Content/Unmanaged";
	std::filesystem::create_directories(Folder);
	{
		std::ofstream Unmanaged(Folder / "notes.txt");
		Unmanaged << "unmanaged";
	}
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Unmanaged",
		.VirtualPath = "/ContentBrowserTests/Unmanaged/",
		.PhysicalPath = Folder.generic_string()};
	const FContentBrowserOperationResult FolderResult =
		Operations.Rename(FolderItem, "Renamed");
	EXPECT_FALSE(FolderResult);
	EXPECT_TRUE(FolderResult.Status.Message.starts_with(
		"Folder contains an unmanaged file:"));
	EXPECT_TRUE(std::filesystem::exists(Folder / "notes.txt"));
}

TEST_F(FContentBrowserModelTests, DuplicatesAssetGraphWithFirstAvailableCopyName)
{
	InitializeDObjectSystem();
	FPackagePath SourcePath;
	FPackagePath DuplicatePath;
	FPackagePath PastedPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/Original", SourcePath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/Original_Copy2", DuplicatePath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/A/Original", PastedPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Material));
	FMaterialStaticProperties Properties = Material->GetStaticProperties();
	Properties.bTwoSided = true;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	{
		std::ofstream Occupied(Root / "Content/Original_Copy.dasset");
		Occupied << "unregistered collision";
	}

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "Original",
		.VirtualPath = Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath).ToString(),
		.PackagePath = SourcePath,
		.PhysicalPath = (Root / "Content/Original.dasset").generic_string(),
		.AssetClassName = DMaterial::StaticClass()
			->GetQualifiedName().ToString()};

	const FContentBrowserOperationResult Result = Operations.Duplicate(Item);

	ASSERT_TRUE(Result) << Result.Status.Message;
	EXPECT_EQ(Result.RevealAssetPath,
		Testing::MakePackageLeafTopLevelAssetPathForTests(DuplicatePath).ToString());
	EXPECT_EQ(Result.FocusPhysicalPath,
		std::filesystem::absolute(Root / "Content/Original_Copy2.dasset")
			.lexically_normal().generic_string());
	DMaterial* Duplicate = nullptr;
	ASSERT_TRUE(LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(DuplicatePath), Duplicate));
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_NE(Duplicate, Material);
	EXPECT_EQ(Duplicate->GetStaticProperties(), Properties);
	EXPECT_FALSE(Duplicate->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::exists(Result.FocusPhysicalPath));
	const FContentBrowserOperationResult PastedResult = Operations.Duplicate(
		Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath),
		"/ContentBrowserTests/A/");
	ASSERT_TRUE(PastedResult) << PastedResult.Status.Message;
	EXPECT_EQ(PastedResult.RevealAssetPath,
		Testing::MakePackageLeafTopLevelAssetPathForTests(PastedPath).ToString());
	DMaterial* Pasted = nullptr;
	ASSERT_TRUE(LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(PastedPath), Pasted));
	ASSERT_NE(Pasted, nullptr);
	EXPECT_EQ(Pasted->GetStaticProperties(), Properties);
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(PastedPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(DuplicatePath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(SourcePath));
}

TEST_F(FContentBrowserModelTests, UnclaimedSameStemFileRenamesIndependently)
{
	InitializeDObjectSystem();
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/Independent", AssetPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const std::filesystem::path FilePath = Root / "Content/Independent.txt";
	{
		std::ofstream File(FilePath);
		File << "ordinary";
	}
	FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(QueryAssetCompanionOwnership(FilePath, Ownership));
	EXPECT_EQ(
		Ownership.State,
		EAssetCompanionOwnershipState::Unclaimed);

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem FileItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "Independent.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	const FContentBrowserOperationResult Result =
		Operations.Rename(FileItem, "Renamed.txt");
	ASSERT_TRUE(Result) << Result.Status.Message;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/Renamed.txt"));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, OwnedCompanionIsProtectedAndCommittedFolderMoveWarnsOnCleanup)
{
	InitializeDObjectSystem();
	const std::filesystem::path Folder = Root / "Content/OwnedFolder";
	std::filesystem::create_directories(Folder);
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/OwnedFolder/Owned", AssetPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const std::filesystem::path Companion = Folder / "Owned.meta";
	{
		std::ofstream File(Companion);
		File << "owned";
	}
	struct FContributorReset
	{
		FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { UnregisterAssetDeleteContributor(Handle); }
	};
	const FAssetDeleteContributorHandle Contributor =
	RegisterAssetDeleteContributor(
		DMaterial::StaticClass(),
		[AssetPath, Companion](const FAssetData& Data,
			const FAssetPackageInspection&,
			FAssetDeleteContribution& Contribution) -> FAssetResult {
			if (Data.PackagePath == AssetPath)
				Contribution.Files.push_back(Companion);
			return {};
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};

	FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(QueryAssetCompanionOwnership(Companion, Ownership));
	ASSERT_EQ(Ownership.State, EAssetCompanionOwnershipState::Owned);
	ASSERT_EQ(Ownership.Owners.size(), 1);
	EXPECT_EQ(Ownership.Owners.front(), AssetPath);
	bool bMoveCalled = false;
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [&](std::span<const FEditorAssetMove>) -> FAssetResult {
			bMoveCalled = true;
			return {};
		});
	const FContentBrowserItem CompanionItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "Owned.meta",
		.PhysicalPath = Companion.generic_string(),
		.Extension = ".meta"};
	const FContentBrowserOperationResult RenameResult =
		Operations.Rename(CompanionItem, "Other.meta");
	EXPECT_FALSE(RenameResult);
	EXPECT_TRUE(RenameResult.Status.Message.find(AssetPath.ToString())
		!= std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Companion));

	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "OwnedFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentBrowserOperationResult FolderResult =
		Operations.Rename(FolderItem, "MovedFolder");
	EXPECT_TRUE(FolderResult);
	EXPECT_TRUE(bMoveCalled);
	EXPECT_TRUE(FolderResult.Warning.find("source folder")
		!= std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Folder));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, FolderRenameSucceedsWithWarningAfterInjectedCleanupFailure)
{
	InitializeDObjectSystem();
	const std::filesystem::path Folder = Root / "Content/CleanupSource";
	std::filesystem::create_directories(Folder);
	FPackagePath SourcePath;
	FPackagePath DestinationPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/CleanupSource/Asset", SourcePath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/CleanupDestination/Asset", DestinationPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FAssetMutationJob MoveJob;
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[&](std::span<const FEditorAssetMove> Moves) -> FAssetResult {
			std::vector<FAssetRelocationMapping> Mappings;
			Mappings.reserve(Moves.size());
			for (const FEditorAssetMove& Move : Moves)
				Mappings.push_back({Move.OldPath, Move.NewPath});
			FAssetRelocationSummary Summary;
			FAssetResult Result = PrepareAssetRelocationJob(
				Mappings, Summary, MoveJob);
			return Result ? MoveJob.ResumeForward() : Result;
		},
		[](const std::filesystem::path&, std::error_code& Error) {
			Error = std::make_error_code(std::errc::permission_denied);
			return false;
		});
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "CleanupSource",
		.PhysicalPath = Folder.generic_string()};

	const FContentBrowserOperationResult Result =
		Operations.Rename(FolderItem, "CleanupDestination");

	ASSERT_TRUE(Result) << Result.Status.Message;
	EXPECT_TRUE(Result.Warning.find("cleanup failed") != std::string::npos
		|| Result.Warning.find("could not be removed") != std::string::npos);
	EXPECT_EQ(Result.FocusPhysicalPath,
		std::filesystem::absolute(Root / "Content/CleanupDestination")
			.lexically_normal().generic_string());
	EXPECT_EQ(ResolveAssetPath(SourcePath).FinalPath, DestinationPath);
}

TEST_F(FContentBrowserModelTests, RejectsOrdinaryMutationsInReadOnlyMount)
{
	Registry.reset();
	const std::array Definitions{
		FMountPoint{
			.VirtualRoot = "/ContentBrowserReadOnly/",
			.Owner = EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bContentWritable = false}};
	Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(
		Definitions);
	ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();

	const std::filesystem::path File = Root / "Content/notes.txt";
	const std::filesystem::path Folder = Root / "Content/WritableOrdinaryFolder";
	{
		std::ofstream Stream(File);
		Stream << "notes";
	}
	std::filesystem::create_directories(Folder);
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});

	const FContentBrowserOperationResult CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string());
	EXPECT_FALSE(CreateResult);
	EXPECT_EQ(CreateResult.Status.Error, EAssetError::ReadOnlyMode);
	EXPECT_TRUE(CreateResult.Status.Message.find("read-only") != std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/New Folder"));

	const FContentBrowserItem FileItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "notes.txt",
		.PhysicalPath = File.generic_string(),
		.Extension = ".txt"};
	const FContentBrowserOperationResult FileResult =
		Operations.Rename(FileItem, "renamed.txt");
	EXPECT_FALSE(FileResult) << FileResult.Status.Message;
	EXPECT_EQ(FileResult.Status.Error, EAssetError::ReadOnlyMode)
		<< FileResult.Status.Message;
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/renamed.txt"));

	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "WritableOrdinaryFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentBrowserOperationResult FolderResult =
		Operations.Rename(FolderItem, "RenamedFolder");
	EXPECT_FALSE(FolderResult);
	EXPECT_EQ(FolderResult.Status.Error, EAssetError::ReadOnlyMode);
	EXPECT_TRUE(std::filesystem::is_directory(Folder));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/RenamedFolder"));
}

TEST_F(FContentBrowserModelTests, AllowsOrdinaryMutationsInWritableAutoScanMount)
{
	const std::filesystem::path File = Root / "Content/notes.txt";
	const std::filesystem::path Folder = Root / "Content/WritableOrdinaryFolder";
	{
		std::ofstream Stream(File);
		Stream << "notes";
	}
	std::filesystem::create_directories(Folder);
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});

	const FContentBrowserOperationResult CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string());
	ASSERT_TRUE(CreateResult) << CreateResult.Status.Message;
	EXPECT_TRUE(std::filesystem::is_directory(CreateResult.FocusPhysicalPath));

	const FContentBrowserItem FileItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "notes.txt",
		.PhysicalPath = File.generic_string(),
		.Extension = ".txt"};
	const FContentBrowserOperationResult FileResult =
		Operations.Rename(FileItem, "renamed.txt");
	ASSERT_TRUE(FileResult) << FileResult.Status.Message;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/renamed.txt"));

	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "WritableOrdinaryFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentBrowserOperationResult FolderResult =
		Operations.Rename(FolderItem, "RenamedFolder");
	ASSERT_TRUE(FolderResult) << FolderResult.Status.Message;
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/RenamedFolder"));
}

TEST_F(FContentBrowserModelTests, OperationsPropagateMoveFailureAndUseRecursiveDeletionPreflight)
{
	FContentBrowserModel Model;
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {
				EAssetError::IoError,
				"Injected move failure."};
		});
	const FContentBrowserItem AssetItem{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "Old",
		.VirtualPath = "/ContentBrowserTests/Old.Old",
		.PackagePath = [] {
			FPackagePath Path;
			FPackagePath::TryCreate("/ContentBrowserTests/Old", Path);
			return Path;
		}(),
		.PhysicalPath = (Root / "Content/Old.dasset").generic_string()};
	const FContentBrowserOperationResult MoveResult =
		Operations.Rename(AssetItem, "New");
	EXPECT_FALSE(MoveResult);
	EXPECT_EQ(MoveResult.Status.Message, "Injected move failure.");

	const std::filesystem::path Folder = Root / "Content/NonEmpty";
	std::filesystem::create_directories(Folder);
	{
		std::ofstream Child(Folder / "child.txt");
		Child << "blocked";
	}
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "NonEmpty",
		.PhysicalPath = Folder.generic_string()};
	const std::array Items{FolderItem};
	const std::unordered_set<std::string> Selection{FolderItem.StableId()};
	const FContentDeletionPlanPtr Plan =
		Operations.BuildDeletionPlan(Items);
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[&](const FContentDeletionFingerprint& Entry) {
			return Entry.Kind == EContentDeletionEntryKind::OrdinaryFile
				&& Entry.PhysicalPath
					== std::filesystem::absolute(Folder / "child.txt")
						.lexically_normal()
						.generic_string();
		}));
	EXPECT_TRUE(std::filesystem::exists(Folder));
	EXPECT_TRUE(std::filesystem::exists(Folder / "child.txt"));

}

TEST_F(FContentBrowserModelTests, PackageRenameReturnsRelocatedAssetIdentity)
{
	FContentBrowserModel Model;
	FPackagePath OldPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/OldPackage", OldPath));
	const FContentBrowserItem AssetItem{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "OldPackage",
		.VirtualPath = "/ContentBrowserTests/OldPackage.AssetObject",
		.PackagePath = OldPath,
		.PhysicalPath = (Root / "Content/OldPackage.dasset").generic_string()};
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult { return {}; });

	const FContentBrowserOperationResult Result =
		Operations.Rename(AssetItem, "NewPackage");

	ASSERT_TRUE(Result) << Result.Status.Message;
	EXPECT_EQ(Result.RevealAssetPath,
		"/ContentBrowserTests/NewPackage.AssetObject");
}

TEST_F(FContentBrowserModelTests, RefreshesSnapshotAfterFolderMutation)
{
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	Model.RequestDirectoryChildrenSnapshot((Root / "Content").generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const size_t ChildrenBefore = Model.GetDirectoryChildren(
		(Root / "Content").generic_string()).size();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});

	const FContentBrowserOperationResult CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string());
	ASSERT_TRUE(CreateResult) << CreateResult.Status.Message;
	Model.RefreshItemsSnapshot();
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(
		(Root / "Content").generic_string()));
	Model.RequestDirectoryChildrenSnapshot((Root / "Content").generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	EXPECT_EQ(
		Model.GetDirectoryChildren((Root / "Content").generic_string()).size(),
		ChildrenBefore + 1);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[&](const FContentBrowserItem& Item) {
			return Item.PhysicalPath == CreateResult.FocusPhysicalPath;
		}));
}

TEST_F(FContentBrowserModelTests, BuildsRecursiveFilterIndependentDeletionPlan)
{
	const std::filesystem::path Tree = Root / "Content/Tree";
	const std::filesystem::path Nested = Tree / "Nested";
	const std::filesystem::path Hidden = Tree / ".hidden";
	std::filesystem::create_directories(Nested);
	std::filesystem::create_directories(Hidden);
	{
		std::ofstream Visible(Tree / "visible.txt");
		Visible << "visible";
		std::ofstream HiddenFile(Hidden / "secret.txt");
		HiddenFile << "secret";
	}

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem RootItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Tree",
		.PhysicalPath = Tree.generic_string()};
	const FContentBrowserItem NestedItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Nested",
		.PhysicalPath = Nested.generic_string()};
	const std::array Items{RootItem, NestedItem};
	const std::unordered_set<std::string> Selection{
		RootItem.StableId(), NestedItem.StableId()};

	const FContentDeletionPlanPtr Plan =
		Operations.BuildDeletionPlan(Items);
	ASSERT_NE(Plan, nullptr);
	EXPECT_TRUE(Plan->CanExecute());
	ASSERT_EQ(Plan->MaximalRoots.size(), 1);
	EXPECT_EQ(Plan->Summary.FolderCount, 3);
	EXPECT_EQ(Plan->Summary.FileCount, 2);
	EXPECT_EQ(Plan->Summary.AssetCount, 0);
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[&](const FContentDeletionFingerprint& Entry) {
			return Entry.PhysicalPath.ends_with("/.hidden/secret.txt");
		}));
	EXPECT_TRUE(std::filesystem::exists(Hidden / "secret.txt"));
	EXPECT_TRUE(Operations.IsDeletionPlanCurrent(*Plan));

	const uint64 BeforeDigest = Plan->MaximalRoots.front().Fingerprint.Digest;
	{
		std::ofstream HiddenFile(Hidden / "secret.txt", std::ios::app);
		HiddenFile << " changed";
	}
	const FContentDeletionPlanPtr Updated =
		Operations.BuildDeletionPlan(Items);
	ASSERT_EQ(Updated->MaximalRoots.size(), 1);
	EXPECT_NE(Updated->MaximalRoots.front().Fingerprint.Digest, BeforeDigest);
	EXPECT_FALSE(Operations.IsDeletionPlanCurrent(*Plan));
}

TEST_F(FContentBrowserModelTests, BlocksUnknownPackagesWithoutOmittingThem)
{
	const std::filesystem::path Folder = Root / "Content/UnknownPackage";
	std::filesystem::create_directories(Folder);
	{
		std::ofstream Package(Folder / "Unregistered.dasset");
		Package << "not a registered package";
	}
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "UnknownPackage",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&FolderItem, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::UnknownPackage;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[](const FContentDeletionFingerprint& Entry) {
			return Entry.Kind == EContentDeletionEntryKind::UnknownPackage;
		}));
}

TEST(FContentDeletionAnalysisTests, RejectsReadOnlyMountBeforeMutation)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "ContentDeletionReadOnly";
	std::error_code Ec;
	Durin::Testing::RemoveTestWorkDirectory(Root, Ec);
	std::filesystem::create_directories(Root / "Content/Folder");
	const std::array Definitions{
		FMountPoint{
			.VirtualRoot = "/ContentDeletionReadOnly/",
			.Owner = EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bContentWritable = false}};
	Testing::FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Folder{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Folder",
		.PhysicalPath = (Root / "Content/Folder").generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&Folder, 1});
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::ReadOnlyMount;
		}));
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/Folder"));

	const FContentBrowserItem MountRoot{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Content",
		.PhysicalPath = (Root / "Content").generic_string()};
	const FContentDeletionPlanPtr MountRootPlan = Operations.BuildDeletionPlan(
		std::span{&MountRoot, 1});
	EXPECT_TRUE(std::ranges::any_of(
		MountRootPlan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::MountRoot;
		}));
}

TEST_F(FContentBrowserModelTests, BatchAnalysisExcludesInternalReferences)
{
	InitializeDObjectSystem();
	FPackagePath BasePath;
	FPackagePath InternalPath;
	FPackagePath ExternalPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Base", BasePath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Internal", InternalPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/External", ExternalPath));
	DMaterial* Base = nullptr;
	DMaterialInstance* Internal = nullptr;
	DMaterialInstance* External = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(BasePath, Base));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(InternalPath, Internal));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(ExternalPath, External));
	ASSERT_TRUE(Internal->SetParent(Base));
	ASSERT_TRUE(External->SetParent(Base));
	ASSERT_TRUE(SavePackage(Base->GetPackage()));
	ASSERT_TRUE(SavePackage(Internal->GetPackage()));
	ASSERT_TRUE(SavePackage(External->GetPackage()));

	FAssetDeletionOperation Job;
	const std::array PartialPaths{BasePath, InternalPath};
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion(
		{.AssetPaths = {PartialPaths.begin(), PartialPaths.end()}}, Job));
	EXPECT_TRUE(std::ranges::any_of(
		Job.GetBlockers(),
		[&](const FAssetDeletionBlocker& Blocker) {
			return Blocker.AssetPath == BasePath
				&& Blocker.RelatedAssetPath == ExternalPath;
		}));

	const std::array CompletePaths{BasePath, InternalPath, ExternalPath};
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion(
		{.AssetPaths = {CompletePaths.begin(), CompletePaths.end()}}, Job));
	EXPECT_TRUE(Job.GetBlockers().empty());
	EXPECT_EQ(Job.GetEntries().size(), 3);

	ASSERT_TRUE(UnloadPackage(ExternalPath));
	ASSERT_TRUE(UnloadPackage(InternalPath));
	ASSERT_TRUE(UnloadPackage(BasePath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(ExternalPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(InternalPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(BasePath));
}

TEST_F(FContentBrowserModelTests, BatchAnalysisBlocksAmbiguousCompanionOwnership)
{
	InitializeDObjectSystem();
	const std::filesystem::path SharedCompanion =
		Root / "Content/Shared.comp";
	{
		std::ofstream Shared(SharedCompanion);
		Shared << "shared companion";
	}
	struct FContributorReset
	{
		FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { UnregisterAssetDeleteContributor(Handle); }
	};
	const FAssetDeleteContributorHandle Contributor =
	RegisterAssetDeleteContributor(
		DMaterial::StaticClass(),
		[SharedCompanion](const FAssetData& Data,
			const FAssetPackageInspection&,
			FAssetDeleteContribution& Contribution) -> FAssetResult {
			if (Data.PackagePath.GetView().starts_with(
					"/ContentBrowserTests/Companion"))
				Contribution.Files.push_back(SharedCompanion);
			return {};
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};

	FPackagePath FirstPath;
	FPackagePath SecondPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/CompanionFirst", FirstPath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/CompanionSecond", SecondPath));
	DMaterial* First = nullptr;
	DMaterial* Second = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FirstPath, First));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SecondPath, Second));
	ASSERT_TRUE(SavePackage(First->GetPackage()));
	ASSERT_TRUE(SavePackage(Second->GetPackage()));
	FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(QueryAssetCompanionOwnership(
		SharedCompanion, Ownership));
	EXPECT_EQ(
		Ownership.State,
		EAssetCompanionOwnershipState::Ambiguous);
	EXPECT_EQ(Ownership.Owners.size(), 2);

	FAssetDeletionOperation Job;
	const std::array Paths{FirstPath, SecondPath};
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion(
		{.AssetPaths = {Paths.begin(), Paths.end()}}, Job));
	EXPECT_TRUE(std::ranges::any_of(
		Job.GetBlockers(),
		[](const FAssetDeletionBlocker& Blocker) {
			return Blocker.Kind
				== EAssetDeletionBlocker::CompanionOwnershipConflict;
		}));

	ASSERT_TRUE(UnloadPackage(SecondPath));
	ASSERT_TRUE(UnloadPackage(FirstPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(SecondPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(FirstPath));
}

TEST_F(FContentBrowserModelTests, BatchRevalidationDetectsNewExternalReference)
{
	InitializeDObjectSystem();
	FPackagePath BasePath;
	FPackagePath ExternalPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/RevalidateBase", BasePath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/RevalidateExternal", ExternalPath));
	DMaterial* Base = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(BasePath, Base));
	ASSERT_TRUE(SavePackage(Base->GetPackage()));

	FAssetDeletionOperation Job;
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {BasePath}}, Job));
	ASSERT_TRUE(Job.GetBlockers().empty());

	DMaterialInstance* External = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(ExternalPath, External));
	ASSERT_TRUE(External->SetParent(Base));
	ASSERT_TRUE(SavePackage(External->GetPackage()));
	const FAssetOperationResult Commit = Job.Delete({
		.Delete = [] { return FAssetResult{}; },
	});
	EXPECT_EQ(Commit.State, EAssetOperationTerminalState::Rejected);

	ASSERT_TRUE(UnloadPackage(ExternalPath));
	ASSERT_TRUE(UnloadPackage(BasePath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(ExternalPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(BasePath));
}

TEST_F(FContentBrowserModelTests, DeletionOperationExecutesOnlyThePreparedOwnerOnce)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/OwnedDeletion", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const std::filesystem::path File = FindAssetExact(Path)->PhysicalPath;

	uint32 Calls = 0;
	const FAssetDeletionCommit Commit{
		.Delete = [&]() -> FAssetResult {
			++Calls;
			std::error_code Ec;
			std::filesystem::remove(File, Ec);
			return Ec ? FAssetResult{EAssetError::IoError, Ec.message()} : FAssetResult{};
		}};
	FAssetDeletionOperation Original;
	EXPECT_FALSE(Original.Delete(Commit));
	EXPECT_EQ(Calls, 0);
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Original));
	FAssetDeletionOperation Owner = std::move(Original);
	EXPECT_FALSE(Original.Delete(Commit));
	EXPECT_EQ(Calls, 0);
	ASSERT_TRUE(Owner.Delete(Commit));
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	EXPECT_EQ(FindAssetExact(Path), nullptr);
	EXPECT_FALSE(std::filesystem::exists(File));
	EXPECT_FALSE(Owner.Delete(Commit));
	EXPECT_EQ(Calls, 1);
}

TEST_F(FContentBrowserModelTests, DeletionBlocksFailedCompanionInspectionBeforeCallback)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/CorruptDeletion", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
	const std::filesystem::path File = FindAssetExact(Path)->PhysicalPath;
	{
		std::ofstream Corrupt(File, std::ios::binary | std::ios::trunc);
		Corrupt << "invalid package";
	}
	FAssetDeletionOperation Operation;
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	EXPECT_TRUE(std::ranges::any_of(Operation.GetBlockers(), [](const auto& Blocker) {
		return Blocker.Kind == EAssetDeletionBlocker::CompanionInspectionFailed;
	}));
	bool bCalled = false;
	EXPECT_FALSE(Operation.Delete({.Delete = [&]() -> FAssetResult {
		bCalled = true;
		return {};
	}}));
	EXPECT_FALSE(bCalled);
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_NE(FindAssetExact(Path), nullptr);
}

TEST_F(FContentBrowserModelTests, DeletionCompanionInspectionDoesNotLoadPackageDependencies)
{
	InitializeDObjectSystem();
	FPackagePath BasePath, OwnerPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/CompanionBase", BasePath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/CompanionOwner", OwnerPath));
	DMaterial* Base = nullptr;
	DMaterialInstance* Owner = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(BasePath, Base));
	ASSERT_TRUE(SavePackage(Base->GetPackage()));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	ASSERT_TRUE(Owner->SetParent(Base));
	ASSERT_TRUE(SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(UnloadPackage(OwnerPath));
	ASSERT_TRUE(UnloadPackage(BasePath));
	const auto Companion = Root / "Content/Owner.source";
	{
		std::ofstream Output(Companion);
		Output << "companion";
	}
	struct FContributorReset
	{
		FAssetDeleteContributorHandle Handle;
		~FContributorReset() { UnregisterAssetDeleteContributor(Handle); }
	};
	FContributorReset Reset{RegisterAssetDeleteContributor(DMaterialInstance::StaticClass(),
		[&](const FAssetData& Data, const FAssetPackageInspection& Inspection,
			FAssetDeleteContribution& Contribution) -> FAssetResult {
			if (Data.PackagePath == OwnerPath)
			{
				EXPECT_NE(Inspection.FindField("Parent"), nullptr);
				Contribution.Files.push_back(Companion);
			}
			return {};
		})};
	ASSERT_NE(Reset.Handle, 0);
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {OwnerPath}}, Operation));
	ASSERT_EQ(Operation.GetEntries().size(), 1);
	EXPECT_EQ(Operation.GetEntries().front().CompanionFiles, std::vector{Companion});
	EXPECT_EQ(FindResidentPackage(BasePath), nullptr);
	EXPECT_EQ(FindResidentPackage(OwnerPath), nullptr);
	const auto PackageFile = Operation.GetEntries().front().RegistryEntry.PhysicalPath;
	ASSERT_TRUE(Operation.Delete({.Delete = [&]() -> FAssetResult {
		std::error_code Ec;
		std::filesystem::remove(PackageFile, Ec);
		if (Ec) return {EAssetError::IoError, Ec.message()};
		std::filesystem::remove(Companion, Ec);
		return Ec ? FAssetResult{EAssetError::IoError, Ec.message()} : FAssetResult{};
	}}));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	EXPECT_NE(FindAssetExact(BasePath), nullptr);
}

TEST_F(FContentBrowserModelTests, DeletionRevalidatesExternalProviderFingerprint)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ProviderDeletion", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	struct FStore : IAssetReferenceStore
	{
		std::string Fingerprint = "first";
		bool bFail = true;
		auto CaptureSnapshot(FAssetReferenceStoreSnapshot& Snapshot) -> FAssetResult override
		{
			if (bFail) return {EAssetError::IoError, "Injected provider inspection failure."};
			Snapshot = {.ProviderId = "deletion-test", .Fingerprint = Fingerprint};
			return {};
		}
		auto PrepareRewrite(std::span<const FAssetReferenceRewrite>, std::string_view,
			FAssetReferenceStoreRewriteContribution&) -> FAssetResult override
		{
			return {EAssetError::InUse, "Deletion must not rewrite references."};
		}
	} Store;
	struct FStoreReset
	{
		FAssetReferenceStoreHandle Handle;
		~FStoreReset() { UnregisterAssetReferenceStore(Handle); }
	} Reset{RegisterAssetReferenceStore(&Store)};
	ASSERT_NE(Reset.Handle, 0);
	FAssetDeletionOperation Operation;
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	EXPECT_TRUE(std::ranges::any_of(Operation.GetBlockers(), [](const auto& Blocker) {
		return Blocker.Kind == EAssetDeletionBlocker::ReferenceStoreInspectionFailed;
	}));
	Store.bFail = false;
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	Store.Fingerprint = "second";
	bool bCalled = false;
	EXPECT_FALSE(Operation.Delete({.Delete = [&]() -> FAssetResult {
		bCalled = true;
		return {};
	}}));
	EXPECT_FALSE(bCalled);
	EXPECT_NE(FindResidentPackage(Path), nullptr);
	EXPECT_TRUE(std::filesystem::exists(FindAssetExact(Path)->PhysicalPath));
}

TEST_F(FContentBrowserModelTests, BlocksDirectoryReparseTraversal)
{
	const std::filesystem::path Folder = Root / "Content/ReparseRoot";
	const std::filesystem::path Target = Root / "ReparseTarget";
	const std::filesystem::path Link = Folder / "Linked";
	std::filesystem::create_directories(Folder);
	std::filesystem::create_directories(Target);
	std::error_code Ec;
	std::filesystem::create_directory_symlink(Target, Link, Ec);
	if (Ec) GTEST_SKIP() << "Directory symlinks are unavailable: " << Ec.message();

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ReparseRoot",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&FolderItem, 1});
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::ReparsePoint;
		}));
}

TEST_F(FContentBrowserModelTests, EmptyFolderDeletionIsPermanent)
{
	const std::filesystem::path Folder = Root / "Content/EmptyFolder";
	std::filesystem::create_directories(Folder);
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "EmptyFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Plan->Summary.FolderCount, 1);
	EXPECT_EQ(Plan->Summary.AssetCount, 0);
	EXPECT_EQ(Plan->Summary.FileCount, 0);

	FBrowserDeletionDriver Deletion(Operations, Plan);
	ASSERT_TRUE(Deletion.Execute());
	EXPECT_FALSE(std::filesystem::exists(Folder));
}

TEST_F(FContentBrowserModelTests, RejectsExternalCompanionOutsideContentMount)
{
	InitializeDObjectSystem();
	const std::filesystem::path Folder = Root / "Content/UnsafeCompanion";
	const std::filesystem::path OutsideFile = Root / "Outside/innocent.txt";
	std::filesystem::create_directories(Folder);
	std::filesystem::create_directories(OutsideFile.parent_path());
	{
		std::ofstream File(OutsideFile);
		File << "must not be staged";
	}

	struct FContributorReset
	{
		FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { UnregisterAssetDeleteContributor(Handle); }
	};
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/UnsafeCompanion/Material", AssetPath));
	const FAssetDeleteContributorHandle Contributor =
		RegisterAssetDeleteContributor(
			DMaterial::StaticClass(),
			[AssetPath, OutsideFile](const FAssetData& Data,
				const FAssetPackageInspection&,
				FAssetDeleteContribution& Contribution) -> FAssetResult {
				if (Data.PackagePath == AssetPath)
					Contribution.Files.push_back(OutsideFile);
				return {};
			});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "UnsafeCompanion",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&Item, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[&](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::OutsideMount
				&& Blocker.PhysicalPath
					== std::filesystem::absolute(OutsideFile)
						.lexically_normal().generic_string();
		}));
	EXPECT_FALSE(std::ranges::any_of(
		Plan->MaximalRoots,
		[&](const FContentDeletionRoot& RootEntry) {
			return RootEntry.OriginalPath
				== std::filesystem::absolute(OutsideFile)
					.lexically_normal().generic_string();
		}));
	EXPECT_TRUE(std::filesystem::is_regular_file(OutsideFile));

	ASSERT_TRUE(UnloadPackage(AssetPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, RejectsExternalCompanionReparsePoint)
{
	InitializeDObjectSystem();
	const std::filesystem::path Folder = Root / "Content/ReparseCompanion";
	const std::filesystem::path Target = Root / "Outside/reparse-target.txt";
	const std::filesystem::path Companion = Root / "Content/ReparseCompanion.meta";
	std::filesystem::create_directories(Folder);
	std::filesystem::create_directories(Target.parent_path());
	{
		std::ofstream File(Target);
		File << "must not be staged through a reparse point";
	}
	std::error_code Ec;
	std::filesystem::create_symlink(Target, Companion, Ec);
	if (Ec) GTEST_SKIP() << "File symlinks are unavailable: " << Ec.message();

	struct FContributorReset
	{
		FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { UnregisterAssetDeleteContributor(Handle); }
	};
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/ReparseCompanion/Material", AssetPath));
	const FAssetDeleteContributorHandle Contributor =
		RegisterAssetDeleteContributor(
			DMaterial::StaticClass(),
			[AssetPath, Companion](const FAssetData& Data,
				const FAssetPackageInspection&,
				FAssetDeleteContribution& Contribution) -> FAssetResult {
				if (Data.PackagePath == AssetPath)
					Contribution.Files.push_back(Companion);
				return {};
			});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ReparseCompanion",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&Item, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::ReparsePoint;
		}));
	EXPECT_TRUE(std::filesystem::is_regular_file(Target));

	ASSERT_TRUE(UnloadPackage(AssetPath));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, MixedFolderAndExternalCompanionDeleteTogether)
{
	InitializeDObjectSystem();
	const std::filesystem::path Folder = Root / "Content/Mixed";
	const std::filesystem::path Nested = Folder / "Nested";
	const std::filesystem::path OrdinaryFile = Folder / "notes.txt";
	const std::filesystem::path Companion = Root / "Content/Mixed.materialmeta";
	std::filesystem::create_directories(Nested);
	{
		std::ofstream File(OrdinaryFile);
		File << "ordinary file";
		std::ofstream CompanionFile(Companion);
		CompanionFile << "managed companion";
	}

	struct FContributorReset
	{
		FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { UnregisterAssetDeleteContributor(Handle); }
	};
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/Mixed/Nested/Material", AssetPath));
	const FAssetDeleteContributorHandle Contributor =
	RegisterAssetDeleteContributor(
		DMaterial::StaticClass(),
		[AssetPath, Companion](const FAssetData& Data,
			const FAssetPackageInspection&,
			FAssetDeleteContribution& Contribution) -> FAssetResult {
			if (Data.PackagePath == AssetPath)
				Contribution.Files.push_back(Companion);
			return {};
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Mixed",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Plan->Summary.AssetCount, 1);
	EXPECT_EQ(Plan->Summary.FileCount, 1);
	EXPECT_EQ(Plan->Summary.CompanionCount, 1);
	EXPECT_EQ(Plan->Summary.FolderCount, 2);
	ASSERT_EQ(Plan->MaximalRoots.size(), 2);

	FBrowserDeletionDriver Deletion(Operations, Plan);
	ASSERT_TRUE(Deletion.Execute());
	EXPECT_FALSE(std::filesystem::exists(Folder));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	EXPECT_EQ(FindAssetExact(AssetPath), nullptr);
}

TEST_F(FContentBrowserModelTests, DestructiveDeletionRemovesRegistryAndResidency)
{
	InitializeDObjectSystem();
	FPackagePath AssetPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/DestructiveMaterial", AssetPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const std::filesystem::path PackagePath =
		FindAssetExact(AssetPath)->PhysicalPath;

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "DestructiveMaterial",
		.VirtualPath = AssetPath.ToString(),
		.PhysicalPath = PackagePath.generic_string(),
		.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString()};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());

	FBrowserDeletionDriver Deletion(Operations, Plan);
	ASSERT_TRUE(Deletion.Execute());
	EXPECT_FALSE(std::filesystem::exists(PackagePath));
	EXPECT_EQ(FindAssetExact(AssetPath), nullptr);
	EXPECT_EQ(FindResidentPackage(AssetPath), nullptr);
}

TEST_F(FContentBrowserModelTests, RedirectorDeletionRequiresClosureAndIsPermanent)
{
	InitializeDObjectSystem();
	FPackagePath OldPath;
	FPackagePath FinalPath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/DeleteRedirectOld", OldPath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/DeleteRedirectFinal", FinalPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(OldPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const FAssetRelocationMapping Mapping{OldPath, FinalPath};
	FAssetRelocationSummary Summary;
	FAssetMutationJob Relocation;
	ASSERT_TRUE(PrepareAssetRelocationJob(
		std::span{&Mapping, 1}, Summary, Relocation));
	ASSERT_TRUE(Relocation.ResumeForward());

	FAssetCatalogEntry AliasData =
		FindAssetExact(OldPath);
	FAssetCatalogEntry FinalData =
		FindAssetExact(FinalPath);
	ASSERT_NE(AliasData, nullptr);
	ASSERT_NE(FinalData, nullptr);
	const FContentBrowserItem AliasItem{
		.Kind = EContentBrowserItemKind::Redirector,
		.Name = "DeleteRedirectOld",
		.VirtualPath = Testing::MakePackageLeafTopLevelAssetPathForTests(OldPath).ToString(),
		.PackagePath = OldPath,
		.PhysicalPath = AliasData->PhysicalPath,
		.AssetClassName = AliasData->AssetClassName,
		.RedirectDestination = Testing::MakePackageLeafAssetObjectPathForTests(
			AliasData->RedirectDestination)};
	const FContentBrowserItem FinalItem{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "DeleteRedirectFinal",
		.VirtualPath = Testing::MakePackageLeafTopLevelAssetPathForTests(FinalPath).ToString(),
		.PackagePath = FinalPath,
		.PhysicalPath = FinalData->PhysicalPath,
		.AssetClassName = FinalData->AssetClassName};

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentDeletionPlanPtr AliasOnly = Operations.BuildDeletionPlan(
		std::span{&AliasItem, 1});
	EXPECT_TRUE(std::ranges::any_of(
		AliasOnly->Blockers, [](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind
				== EContentDeletionBlocker::RedirectorTargetNotSelected;
		}));
	const FContentDeletionPlanPtr TargetOnly = Operations.BuildDeletionPlan(
		std::span{&FinalItem, 1});
	EXPECT_TRUE(std::ranges::any_of(
		TargetOnly->Blockers, [](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind
				== EContentDeletionBlocker::TargetRedirectorsNotSelected;
		}));

	const std::array Items{AliasItem, FinalItem};
	const FContentDeletionPlanPtr Complete = BuildDeletionPlan(
		Operations, Items);
	ASSERT_TRUE(Complete->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Complete->Warnings, [](const FContentDeletionWarning& Warning) {
			return Warning.Details.find("authored old path")
				!= std::string::npos;
		}));

	FBrowserDeletionDriver Deletion(Operations, Complete);
	ASSERT_TRUE(Deletion.Execute());
	EXPECT_EQ(FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(FindAssetExact(FinalPath), nullptr);
}

TEST_F(FContentBrowserModelTests, DestructiveDeletionRevalidatesConfirmedBytes)
{
	const std::filesystem::path Folder = Root / "Content/ConflictFolder";
	const std::filesystem::path SourceFile = Folder / "source.txt";
	std::filesystem::create_directories(Folder);
	{
		std::ofstream File(SourceFile);
		File << "source";
	}
	const auto ConfirmedWriteTime = std::filesystem::last_write_time(SourceFile);
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ConflictFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, std::span{&Item, 1});
	{
		std::ofstream File(SourceFile, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(SourceFile, ConfirmedWriteTime);
	FBrowserDeletionDriver Deletion(Operations, Plan);
	EXPECT_FALSE(Deletion.Execute());
	EXPECT_TRUE(std::filesystem::exists(SourceFile));
}

TEST_F(FContentBrowserModelTests, DeletionPlanRejectsSameSizeTimestampPreservingRewrite)
{
	const std::filesystem::path FilePath = Root / "Content/rewrite.txt";
	{
		std::ofstream File(FilePath);
		File << "source";
	}
	const auto ConfirmedWriteTime = std::filesystem::last_write_time(FilePath);
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "rewrite.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	{
		std::ofstream File(FilePath, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(FilePath, ConfirmedWriteTime);
	EXPECT_FALSE(Operations.IsDeletionPlanCurrent(*Plan));
	FBrowserDeletionDriver Deletion(Operations, Plan);
	EXPECT_FALSE(Deletion.Execute());
	EXPECT_TRUE(std::filesystem::exists(FilePath));
}

TEST_F(FContentBrowserModelTests, DeletionRejectsNewUnconfirmedDescendant)
{
	const std::filesystem::path Folder = Root / "Content/StagedRewrite";
	const std::filesystem::path SourceFile = Folder / "source.txt";
	std::filesystem::create_directories(Folder);
	{
		std::ofstream File(SourceFile);
		File << "source";
	}
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "StagedRewrite",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, std::span{&Item, 1});
	const std::filesystem::path Unexpected = Folder / "unexpected.txt";
	{
		std::ofstream File(Unexpected);
		File << "new";
	}
	FBrowserDeletionDriver Deletion(Operations, Plan);
	EXPECT_FALSE(Deletion.Execute());
	EXPECT_TRUE(std::filesystem::exists(SourceFile));
	EXPECT_TRUE(std::filesystem::exists(Unexpected));
}

TEST_F(FContentBrowserModelTests, DestructiveDeletionFailureContinuesForward)
{
	const std::array Paths{
		Root / "Content/first.txt", Root / "Content/second.txt"};
	for (const std::filesystem::path& Path : Paths)
	{
		std::ofstream File(Path);
		File << Path.filename().generic_string();
	}
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File,
			.Name = "first.txt", .PhysicalPath = Paths[0].generic_string(),
			.Extension = ".txt"},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File,
			.Name = "second.txt", .PhysicalPath = Paths[1].generic_string(),
			.Extension = ".txt"}};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, Items);

	FContentDeletionHooks Hooks;
	bool bFailSecondRoot = true;
	Hooks.RemoveAll = [&bFailSecondRoot](
		const std::filesystem::path& Path) -> std::error_code {
		if (Path.filename() == "second.txt" && std::exchange(bFailSecondRoot, false))
			return std::make_error_code(std::errc::io_error);
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Path, Error);
		return Error;
	};
	FBrowserDeletionDriver Deletion(Operations, Plan, Hooks);
	EXPECT_FALSE(Deletion.Execute());
	EXPECT_FALSE(std::filesystem::exists(Paths[0]));
	EXPECT_TRUE(std::filesystem::exists(Paths[1]));
	EXPECT_TRUE(Deletion.GetDetails().find("irreversible") != std::string::npos);
	EXPECT_TRUE(Deletion.Execute()) << Deletion.GetDetails();
	EXPECT_FALSE(std::filesystem::exists(Paths[1]));
}

TEST_F(FContentBrowserModelTests, CompletedDeletionLeavesNoRecoveryArtifact)
{
	const std::filesystem::path FilePath = Root / "Content/evicted.txt";
	{
		std::ofstream File(FilePath);
		File << "evict me";
	}
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperationService Operations(
		FContentBrowserPaths{}, [](std::span<const FEditorAssetMove>) -> FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "evicted.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	const FContentDeletionPlanPtr Plan = BuildDeletionPlan(
		Operations, std::span{&Item, 1});
	FBrowserDeletionDriver Deletion(Operations, Plan);
	ASSERT_TRUE(Deletion.Execute());
	EXPECT_FALSE(std::filesystem::exists(FilePath));
	EXPECT_FALSE(std::filesystem::exists(Root / "Undo"));
}

TEST_F(FContentBrowserModelTests, PartialRootDeletionResumesThroughService)
{
	const auto Folder = Root / "Content/partial";
	std::filesystem::create_directories(Folder);
	std::ofstream(Folder / "first.txt") << "first";
	std::ofstream(Folder / "second.txt") << "second";
	FContentBrowserModel Model;
	FContentBrowserOperationService Operations(FContentBrowserPaths{},
		[](std::span<const FEditorAssetMove>) -> FAssetResult { return {}; });
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Folder,
		.Name = "partial", .PhysicalPath = Folder.generic_string()};
	const auto Plan = BuildDeletionPlan(Operations, std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	bool bFail = true;
	FBrowserDeletionDriver Deletion(Operations, Plan, {.RemoveAll = [&](const auto& Path) {
		std::error_code Error;
		if (std::exchange(bFail, false))
		{
			std::filesystem::remove(Path / "first.txt", Error);
			return std::make_error_code(std::errc::io_error);
		}
		Durin::Testing::RemoveTestWorkDirectory(Path, Error);
		return Error;
	}});
	EXPECT_FALSE(Deletion.Execute());
	EXPECT_FALSE(Operations.IsDeletionPlanCurrent(*Plan));
	EXPECT_TRUE(Deletion.Execute()) << Deletion.GetDetails();
	EXPECT_FALSE(std::filesystem::exists(Folder / "second.txt"));
}

TEST_F(FContentBrowserModelTests, DeletionProjectionFailureRetainsStructuredCommittedState)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ProjectionFailure", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto Physical = FindAssetExact(Path)->PhysicalPath;
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	int Calls = 0;
	const FAssetDeletionCommit Commit{.Delete = [&]() -> FAssetResult {
		++Calls;
		std::filesystem::remove(Physical);
		// Reconciliation between physical removal and publication changes the revision.
		const auto Refresh = RefreshAssetRegistry(EAssetRegistryScanMode::Incremental);
		EXPECT_TRUE(Refresh);
		return {};
	}};
	const auto Result = Operation.Delete(Commit);
	EXPECT_EQ(Result.State, EAssetOperationTerminalState::ContentCommittedProjectionPending);
	EXPECT_TRUE(Result);
	EXPECT_FALSE(Operation.Delete(Commit));
	EXPECT_EQ(Calls, 1);
}

TEST_F(FContentBrowserModelTests, ServiceRechecksPolicyAndPublishesFilesystemChangeOnce)
{
	bool bAllowed = true;
	int Publications = 0;
	FContentBrowserOperationService Service({}, {}, {}, {},
		[&] { ++Publications; }, [&] { return bAllowed; });
	ASSERT_TRUE(Service.QueryMutation());
	bAllowed = false;
	EXPECT_FALSE(Service.CreateFolder((Root / "Content").generic_string()));
	EXPECT_EQ(Publications, 0);
	bAllowed = true;
	const auto Result = Service.CreateFolder((Root / "Content").generic_string());
	ASSERT_TRUE(Result);
	EXPECT_TRUE(std::filesystem::is_directory(Result.FocusPhysicalPath));
	EXPECT_EQ(Publications, 1);
	Service.StopRequestAdmission();
	EXPECT_FALSE(Service.CreateFolder((Root / "Content").generic_string()));
	EXPECT_EQ(Publications, 1);
}

TEST_F(FContentBrowserModelTests, ServiceRetainsStructuredAssetFailureAndFixUpScope)
{
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A", Path));
	int Calls = 0;
	int Publications = 0;
	FContentBrowserOperationService Service({}, {}, {},
		[&](std::span<const FPackagePath>) -> FContentBrowserOperationResult {
			++Calls;
			return FAssetOperationResult{
				.Kind = EAssetOperationKind::FixUpRedirectors,
				.State = EAssetOperationTerminalState::ForwardPending,
				.Persistence = EAssetOperationPersistenceState::PartiallyPersisted,
				.AffectedAssets = {Path},
				.OperationId = "retained-job"};
		}, [&] { ++Publications; });
	EXPECT_FALSE(Service.FixUpRedirectorsInFolder(""));
	EXPECT_EQ(Calls, 0);
	const std::array Paths{Path};
	const auto Result = Service.FixUpRedirectors(Paths);
	ASSERT_TRUE(Result.AssetResult);
	EXPECT_EQ(Result.AssetResult->State, EAssetOperationTerminalState::ForwardPending);
	EXPECT_EQ(Result.AssetResult->OperationId, "retained-job");
	EXPECT_EQ(Result.AssetResult->Persistence, EAssetOperationPersistenceState::PartiallyPersisted);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Publications, 1);
}

TEST_F(FContentBrowserModelTests, AssetDeletionSessionResumesAfterPartialRemovalAndReconciliation)
{
	InitializeDObjectSystem();
	std::vector<FContentBrowserItem> Items;
	std::vector<FPackagePath> Packages;
	for (const auto* Name : {"FirstRecovery", "SecondRecovery"})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(std::string("/ContentBrowserTests/") + Name, Path));
		DMaterial* Material = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		Packages.push_back(Path);
		Items.push_back({.Kind = EContentBrowserItemKind::Asset, .Name = Name,
			.PhysicalPath = FindAssetExact(Path)->PhysicalPath});
	}
	int Publications = 0;
	FContentBrowserOperationService Service({}, {}, {}, {}, [&] { ++Publications; });
	const auto Confirmation = Service.BuildDeletionPlan(Items);
	ASSERT_TRUE(Confirmation->CanExecute());
	int Calls = 0;
	const auto First = Service.ExecuteDeletion(Confirmation, {.RemoveAll = [&](const auto& Path) {
		if (++Calls == 2) return std::make_error_code(std::errc::io_error);
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Path, Error);
		return Error;
	}});
	ASSERT_TRUE(First.AssetResult);
	EXPECT_EQ(First.AssetResult->State, EAssetOperationTerminalState::ForwardPending);
	EXPECT_EQ(Publications, 0);
	Service.DismissDeletion(Confirmation);
	EXPECT_EQ(Service.GetPendingDeletion(), Confirmation);
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
	const auto Retry = Service.ExecuteDeletion(Confirmation);
	ASSERT_TRUE(Retry) << Retry.Status.Message;
	EXPECT_EQ(Publications, 1);
	for (const auto& Item : Items) EXPECT_FALSE(std::filesystem::exists(Item.PhysicalPath));
	for (const auto& Path : Packages) EXPECT_FALSE(FindAssetExact(Path));
	EXPECT_FALSE(Service.GetPendingDeletion());
	EXPECT_FALSE(Service.ExecuteDeletion(Confirmation));
	EXPECT_EQ(Publications, 1);
}

TEST_F(FContentBrowserModelTests, DeletionSessionProtectsRecreatedRootsAndRemainingBytes)
{
	const auto First = Root / "Content/first.txt";
	const auto Second = Root / "Content/second.txt";
	std::ofstream(First) << "original";
	std::ofstream(Second) << "remaining";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .Name = "first", .PhysicalPath = First.generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .Name = "second", .PhysicalPath = Second.generic_string()}};
	FContentBrowserOperationService Service;
	const auto Plan = Service.BuildDeletionPlan(Items);
	int Calls = 0;
	EXPECT_FALSE(Service.ExecuteDeletion(Plan, {.RemoveAll = [&](const auto& Path) {
		if (++Calls == 2) return std::make_error_code(std::errc::io_error);
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Path, Error);
		return Error;
	}}));
	std::ofstream(First) << "replacement";
	EXPECT_FALSE(Service.ExecuteDeletion(Plan));
	EXPECT_EQ(Calls, 2);
	EXPECT_TRUE(std::filesystem::exists(First));
	std::filesystem::remove(First);
	std::ofstream(Second) << "changed";
	EXPECT_FALSE(Service.ExecuteDeletion(Plan));
	EXPECT_EQ(Calls, 2);
	EXPECT_TRUE(std::filesystem::exists(Second));
}

TEST_F(FContentBrowserModelTests, StaleConfirmationRequiresNewHandleAndUsesCapturedRequest)
{
	const auto Folder = Root / "Content/confirmation";
	std::filesystem::create_directories(Folder);
	std::ofstream(Folder / "before.txt") << "before";
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Folder,
		.Name = "confirmation", .PhysicalPath = Folder.generic_string()};
	FContentBrowserOperationService Service;
	const auto Plan = Service.BuildDeletionPlan(std::span{&Item, 1});
	std::ofstream(Folder / "after.txt") << "after";
	const auto Stale = Service.ExecuteDeletion(Plan);
	ASSERT_TRUE(Stale.ReplacementConfirmation);
	EXPECT_EQ(Stale.ReplacementConfirmation->Summary.FileCount, 2u);
	EXPECT_FALSE(Service.ExecuteDeletion(Plan));
	EXPECT_TRUE(std::filesystem::exists(Folder));
	Service.DismissDeletion(Stale.ReplacementConfirmation);
	EXPECT_FALSE(Service.ExecuteDeletion(Stale.ReplacementConfirmation));
	EXPECT_TRUE(std::filesystem::exists(Folder));
}

TEST_F(FContentBrowserModelTests, ServicePublishesProjectionPendingWithoutRepeatingPhysicalDelete)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ServiceProjection", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "ServiceProjection", .PhysicalPath = FindAssetExact(Path)->PhysicalPath};
	int Publications = 0;
	FContentBrowserOperationService Service({}, {}, {}, {}, [&] { ++Publications; });
	const auto Plan = Service.BuildDeletionPlan(std::span{&Item, 1});
	int Calls = 0;
	const auto Result = Service.ExecuteDeletion(Plan, {.RemoveAll = [&](const auto& Physical) {
		++Calls;
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Physical, Error);
		EXPECT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
		return Error;
	}});
	ASSERT_TRUE(Result.AssetResult);
	EXPECT_EQ(Result.AssetResult->State, EAssetOperationTerminalState::ContentCommittedProjectionPending);
	EXPECT_EQ(Publications, 1);
	EXPECT_FALSE(Service.ExecuteDeletion(Plan));
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(Publications, 1);
}

TEST_F(FContentBrowserModelTests, ServiceDoesNotDispatchOrPublishIdentityMoves)
{
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Identity", Path));
	int Calls = 0;
	int Publications = 0;
	FContentBrowserOperationService Service({}, [&](auto) -> FAssetResult {
		++Calls;
		return {};
	}, {}, {}, [&] { ++Publications; });
	const std::array Moves{FEditorAssetMove{Path, Path}};
	EXPECT_TRUE(Service.Move(Moves));
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "Identity", .PackagePath = Path};
	EXPECT_TRUE(Service.Rename(Item, "Identity"));
	EXPECT_EQ(Calls, 0);
	EXPECT_EQ(Publications, 0);
}

TEST_F(FContentBrowserModelTests, ScopedRefreshPreservesUnrelatedRowsAndTreeIdentity)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/AB";
	std::filesystem::create_directories(A / "Child");
	std::filesystem::create_directories(B);
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const auto Tree = Model.GetDirectorySnapshot(A.generic_string());
	const auto Version = Model.GetSnapshotVersion();
	const auto Generation = Model.GetRequestGeneration();
	FContentChangeBatch Changes{.Changes = {{EContentChangeKind::Modified,
		(B / "asset.dasset").generic_string(), (B / "asset.dasset").generic_string()}}};
	EXPECT_FALSE(Model.ApplyContentChanges(Changes));
	EXPECT_EQ(Model.GetSnapshotVersion(), Version);
	EXPECT_EQ(Model.GetRequestGeneration(), Generation);
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string()), Tree);
	Changes.Changes[0].OldPhysicalPath = Changes.Changes[0].NewPhysicalPath = (A / "asset.dasset").generic_string();
	EXPECT_TRUE(Model.ApplyContentChanges(Changes));
	EXPECT_NE(Model.GetSnapshotVersion(), Version);
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string()), Tree);
}

TEST_F(FContentBrowserModelTests, ScopedRefreshDistinguishesRecursiveSearchAndDirectoryStructure)
{
	const auto A = Root / "Content/A";
	std::filesystem::create_directories(A / "Nested");
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	FContentChangeBatch Changes{.Changes = {{EContentChangeKind::Added, {},
		(A / "Nested/file.txt").generic_string()}}};
	EXPECT_FALSE(Model.ApplyContentChanges(Changes));
	Model.SetSearch("file");
	EXPECT_TRUE(Model.ApplyContentChanges(Changes));
	Model.SetSearch("");
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	std::filesystem::create_directory(A / "New");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "New").generic_string(), {}, {}, true}}}));
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
}

TEST_F(FContentBrowserModelTests, DirectoryRenameMigratesNavigationAndDeletionFallsBack)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/B";
	std::filesystem::create_directories(A / "Nested");
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	ASSERT_TRUE(Model.NavigateToPhysical((A / "Nested").generic_string()));
	std::filesystem::rename(A, B);
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Renamed,
		A.generic_string(), B.generic_string(), {}, {}, true}}}));
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), (B / "Nested").generic_string());
	EXPECT_EQ(Model.GetHistory().front(), B.generic_string());
	std::filesystem::remove(B / "Nested");
	std::filesystem::remove(B);
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Removed,
		B.generic_string(), {}, {}, {}, true}}}));
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), (Root / "Content").generic_string());
}

TEST_F(FContentBrowserModelTests, UnrelatedChangesPreserveInFlightCaptureAndRelatedChangesCoalesce)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/B";
	std::filesystem::create_directory(A / "Original");
	auto Gate = std::make_shared<FBrowserScanGate>();
	FContentBrowserModel Model(true);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Generation = Model.GetRequestGeneration();
	EXPECT_FALSE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(B / "Other").generic_string(), {}, {}, true}}}));
	EXPECT_EQ(Model.GetRequestGeneration(), Generation);
	std::filesystem::create_directory(A / "Latest");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "Latest").generic_string(), {}, {}, true}}}));
	EXPECT_GT(Model.GetRequestGeneration(), Generation);
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_EQ(Model.GetItems().size(), 2u);
	EXPECT_FALSE(Model.IsLoading());
}

TEST_F(FContentBrowserModelTests, MountedReconciliationFencePreventsPrematurePublication)
{
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Child");
	uint64 Current = 1;
	uint64 Acknowledged = 1;
	FContentBrowserModel Model(true);
	Model.SetMountedContentValidation([&] { return Current; }, [&] { return Acknowledged; });
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	++Current;
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_TRUE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	Acknowledged = Current;
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Model.GetItems().size(), 1u);
}

TEST_F(FContentBrowserModelTests, ScopedServiceNotificationsDescribeCommittedFilesystemOperations)
{
	std::vector<FContentChangeBatch> Batches;
	FContentBrowserOperationService Service;
	Service.SetScopedContentNotifier([&](FContentChangeBatch Batch) { Batches.push_back(std::move(Batch)); });
	const auto Created = Service.CreateFolder((Root / "Content/A").generic_string());
	ASSERT_TRUE(Created);
	ASSERT_EQ(Batches.size(), 1u);
	ASSERT_FALSE(Batches[0].bFullRefresh);
	ASSERT_EQ(Batches[0].Changes.size(), 1u);
	EXPECT_EQ(Batches[0].Changes[0].Kind, EContentChangeKind::Added);
	EXPECT_TRUE(Batches[0].Changes[0].bDirectory);
	FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Folder, .Name = "New Folder",
		.PhysicalPath = Created.FocusPhysicalPath};
	const auto Renamed = Service.Rename(Item, "Renamed");
	ASSERT_TRUE(Renamed);
	ASSERT_EQ(Batches.size(), 2u);
	EXPECT_EQ(Batches[1].Changes[0].OldPhysicalPath, Created.FocusPhysicalPath);
	EXPECT_EQ(Batches[1].Changes[0].NewPhysicalPath, Renamed.FocusPhysicalPath);
	const auto Failed = Service.Rename(Item, "Gone");
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Batches.size(), 2u);
}

TEST_F(FContentBrowserModelTests, CatalogChangesValidateInFlightScopeWithoutGlobalCancellation)
{
	InitializeDObjectSystem();
	FPackagePath APath, BPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Asset", APath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/Asset", BPath));
	DMaterial *AAsset = nullptr, *BAsset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(APath, AAsset));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(BPath, BAsset));
	ASSERT_TRUE(SavePackage(AAsset->GetPackage()));
	ASSERT_TRUE(SavePackage(BAsset->GetPackage()));
	std::filesystem::create_directory(Root / "Content/A/Gate");
	auto Gate = std::make_shared<FBrowserScanGate>();
	FContentBrowserModel Model(true);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Generation = Model.GetRequestGeneration();
	const auto Revision = GetAssetCatalogRevision();
	auto Publication = CaptureAssetRegistryPublication();
	Publication.Assets.at(BPath).SearchableNames = {"ChangedOutsideScope"};
	ASSERT_TRUE(PublishAssetRegistryPublication(Publication));
	const auto Changes = CaptureAssetCatalogChanges(Revision);
	ASSERT_FALSE(Changes.bFullRefresh);
	ASSERT_EQ(Changes.Changes.size(), 1u);
	EXPECT_EQ(Changes.Changes[0].Kind, EContentChangeKind::Modified);
	EXPECT_EQ(Changes.Changes[0].NewAssetPath, BPath.ToString());
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_EQ(Model.GetRequestGeneration(), Generation);
	const auto CommittedRevision = GetAssetCatalogRevision();
	EXPECT_FALSE(PublishAssetRegistryPublication(Publication));
	EXPECT_EQ(GetAssetCatalogRevision(), CommittedRevision);
	EXPECT_TRUE(CaptureAssetCatalogChanges(CommittedRevision).Changes.empty());
	Publication = CaptureAssetRegistryPublication();
	Publication.Assets.at(APath).SearchableNames = {"ChangedInsideScope"};
	ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_GT(Model.GetRequestGeneration(), Generation);
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), (Root / "Content/A").generic_string());
}

TEST_F(FContentBrowserModelTests, LocalTreeInvalidationPreservesUnrelatedInFlightEnumeration)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/B";
	std::filesystem::create_directory(A / "Child");
	FContentBrowserModel Model(true);
	ASSERT_TRUE(Model.NavigateToPhysical(B.generic_string()));
	Model.WaitForPendingSnapshotsForTesting();
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	(void)Model.PumpPendingSnapshots();
	ASSERT_TRUE(Gate->WaitUntilStarted());
	std::filesystem::create_directory(B / "New");
	Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(B / "New").generic_string(), {}, {}, true}}});
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	EXPECT_EQ(Model.GetDirectoryChildren(A.generic_string()).size(), 1u);
}

TEST_F(FContentBrowserModelTests, ThumbnailDependencyDeletionInvalidatesOnlyCapturedDependents)
{
	InitializeDObjectSystem();
	FPackagePath BasePath, OwnerPath, OtherPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Base", BasePath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Owner", OwnerPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Other", OtherPath));
	DMaterial *Base = nullptr, *Other = nullptr;
	DMaterialInstance* Owner = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(BasePath, Base));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(OwnerPath, Owner));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(OtherPath, Other));
	ASSERT_TRUE(Owner->SetParent(Base));
	ASSERT_TRUE(SavePackage(Base->GetPackage()));
	ASSERT_TRUE(SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(SavePackage(Other->GetPackage()));
	FContentBrowserThumbnailReferences References;
	for (const auto& Path : {OwnerPath, OtherPath})
	{
		const auto Asset = FindAssetExact(Path);
		ASSERT_TRUE(Asset);
		const auto Identity = Asset->TopLevelAssets.front().AssetPath.ToString();
		References.Request({.Identity = Identity, .Asset = Editor::FAssetThumbnailPackageFingerprint{
			.AssetPath = Asset->TopLevelAssets.front().AssetPath, .PackagePath = Path,
			.AssetClassName = Asset->AssetClassName}});
	}
	ASSERT_EQ(References.GetAssetReferenceCountForTesting(), 2u);
	const auto Revision = GetAssetCatalogRevision();
	auto Publication = CaptureAssetRegistryPublication();
	Publication.Assets.erase(BasePath);
	Publication.ReferenceFingerprints.erase(BasePath);
	ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
	References.ApplyContentChanges(CaptureAssetCatalogChanges(Revision));
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 1u);
}

TEST_F(FContentBrowserModelTests, AssetAdditionIntroducesMissingDirectoryWithoutInvalidatingExistingChildren)
{
	const auto A = Root / "Content/A";
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	std::filesystem::create_directories(A / "New/Nested");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "New/Nested/Asset.dasset").generic_string()}}}));
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(Model.GetItems().front().Name, "New");
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const auto Tree = Model.GetDirectorySnapshot(A.generic_string());
	const auto Version = Model.GetSnapshotVersion();
	EXPECT_FALSE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "New/Nested/Another.dasset").generic_string()}}}));
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string()), Tree);
	EXPECT_EQ(Model.GetSnapshotVersion(), Version);
}

TEST_F(FContentBrowserModelTests, FreshCatalogCaptureCanProceedWhileMountedRevisionIsSuppressed)
{
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Child");
	uint64 Current = 1;
	FContentBrowserModel Model(true);
	Model.SetMountedContentValidation([&] { return Current; }, [] { return uint64{1}; });
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Current = 2;
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	// This is the refresh requested by the coordinator's independent registry branch.
	Model.RefreshItemsSnapshot(false);
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Model.GetItems().size(), 1u);
}
