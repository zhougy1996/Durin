#include "AssetDeleteModel.h"
#include "ObjectTools.h"
#include "AssetTools/DeleteItemsHost.h"
#include "ThumbnailManagerTestSupport.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "Editor/ContentBrowserModelFixture.h"
#include "Editor/ContentBrowserOperationFixtures.h"
#include "Editor/EditorEngine.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Editor/ContentBrowserQueryTestSupport.h"
#include "Editor/ContentBrowserTransferTestSupport.h"
#include "Editor/ContentBrowserItemTestSupport.h"
#include "NativeAssetBuildTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "Actors/ActorChange.h"
#include "AssetTools/MutationTesting.h"
#include "NativeAssetRenameTestSupport.h"
#include "Asset/RegistryOperations.h"
#include "Asset/Testing/PackageSaveTesting.h"
#include "Asset/Load.h"
#include "NativeAssetTestSupport.h"
#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserRefreshCoordinator.h"
#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Editor/EditorTransactionTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "AssetRegistry/Publication.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Transactions/Transaction.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Texture/Texture2D.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

using Durin::ContentBrowser::Private::TestKind;
using Durin::ContentBrowser::Private::TestName;
using Durin::ContentBrowser::Private::TestVirtualPath;
using Durin::ContentBrowser::Private::TestPhysicalPath;
using Durin::ContentBrowser::Private::TestPackagePath;
using Durin::ContentBrowser::Private::TestAssetClassName;
using Durin::ContentBrowser::Private::TestExtension;
using Durin::ContentBrowser::Private::TestRedirectDestination;
using Durin::ContentBrowser::Private::TestFileSize;
using Durin::ContentBrowser::Private::TestPackageFileSize;
using Durin::ContentBrowser::Private::TestBulkFileSize;
using Durin::ContentBrowser::Private::TestLastWriteTime;

namespace
{
	using namespace Durin;
	using FContentBrowserItem = Durin::ContentBrowser::Private::FStorageItemFixture;
	using Durin::ContentBrowser::EContentBrowserItemKind;
	using namespace Durin::ContentBrowser::Private;

	auto AssetItem(const FTopLevelAssetPath& Path) -> FContentBrowserItem
	{
		FContentBrowserItem Item;
		Item.Kind = EContentBrowserItemKind::Asset;
		Item.VirtualPath = Path.ToString();
		Item.PackagePath = Path.GetPackagePath();
		return Item;
	}
	auto AssetItem(const FPackagePath& Path) -> FContentBrowserItem
	{
		return AssetItem(Testing::MakePackageLeafTopLevelAssetPathForTests(Path));
	}

	// The compiling aggregate is a process authority and cannot restart between cases.
	class FContentBrowserCompilationEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			InitializeAssetCompilingManager();
		}
		auto TearDown() -> void override { ShutdownAssetCompilingManager(); }
	};
	[[maybe_unused]] testing::Environment* GContentBrowserCompilationEnvironment =
		testing::AddGlobalTestEnvironment(new FContentBrowserCompilationEnvironment);

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
			ShutdownAssetRuntime();
			// Each test remounts the same logical root. Release prior editor-retained exports first.
			for (auto* Package : GetLiveAssetPackages())
				if (Package->GetPackagePath().starts_with("/ContentBrowserTests/"))
					for (auto* Asset : Package->GetTopLevelAssets()) Asset->ClearFlags(EObjectFlags::Standalone);
			CollectGarbage(Durin::EObjectFlags::Standalone);
			ASSERT_TRUE(InitializeAssetRuntime());
			ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		}

		static void TearDownTestSuite()
		{
			ShutdownAssetRuntime();
			// Each test remounts the same logical root. Release prior editor-retained exports first.
			for (auto* Package : GetLiveAssetPackages())
				if (Package->GetPackagePath().starts_with("/ContentBrowserTests/"))
					for (auto* Asset : Package->GetTopLevelAssets()) Asset->ClearFlags(EObjectFlags::Standalone);
			CollectGarbage(Durin::EObjectFlags::Standalone);
			ASSERT_TRUE(InitializeAssetRuntime());
			ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		}

		std::string OperationError, OperationWarning;
		FContentChangeBatch ObservedChanges;
		FContentBrowserItem NewItem;
		template<typename T> auto Observe(T& Service) -> void
		{
			Service.SetDiagnosticSink([this](ELogLevel Level, std::string_view Message) {
				if (Level >= ELogLevel::Error) OperationError = Message;
				else if (Level == ELogLevel::Warn) OperationWarning = Message;
			});
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
		auto WaitUntilStarted(std::function<void()> Pump = {}) -> bool
		{
			std::unique_lock Lock(Mutex);
			if (!Pump) return Changed.wait_for(Lock, std::chrono::seconds(5), [&] { return bStarted; });
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!bStarted && std::chrono::steady_clock::now() < Deadline)
			{
				Lock.unlock();
				Pump();
				Lock.lock();
				Changed.wait_until(Lock, std::min(Deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)), [&] { return bStarted; });
			}
			return bStarted;
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

	class FNoOpEditorTransaction final : public Durin::FActorCommandChange
	{
	public:
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override
		{ return {}; }

		auto GetDescription() const -> std::string_view override { return "No-op"; }
		auto Replay(::Durin::ETransactionOperation Operation) -> Durin::FActorChangeResult override
		{
			if (Operation == ::Durin::ETransactionOperation::Undo)
			{
				return {};
			}
			else
			{
				return {};
			}
		}
	};

	class FRouteOnlyThumbnailRenderer final : public DThumbnailRenderer
	{
		DURIN_TEST_THUMBNAIL_RENDERER(FRouteOnlyThumbnailRenderer)
	public:
		auto CanVisualizeAsset(DObject*) const -> bool override { return true; }
		auto Draw(DObject*, FThumbnailDrawContext&) -> void override { ADD_FAILURE() << "Unexpected drawing."; }

	};
} // namespace

TEST_F(FContentBrowserModelTests, MaintainsHistoryAndTruncatesForwardBranch)
{
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/B").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetHistory().size(), 2);
	ASSERT_TRUE(Model.NavigateHistory(-1));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetHistoryIndex(), 0);
	EXPECT_EQ(
		Model.GetCurrentPhysicalPath(),
		std::filesystem::absolute(Root / "Content/A")
			.lexically_normal()
			.generic_string());

	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetHistory().size(), 2);
	EXPECT_EQ(Model.GetHistoryIndex(), 1);
	EXPECT_FALSE(Model.NavigateHistory(1));
	FContentBrowserModelFixtureAccess::Wait(Model);
}

TEST_F(FContentBrowserModelTests, IndexedProjectionRetainsSourceItemsAcrossRequeryAndDestruction)
{
	static_assert(std::ranges::random_access_range<const FContentBrowserItemRange>);
	static_assert(!std::ranges::contiguous_range<const FContentBrowserItemRange>);
	static_assert(std::same_as<std::ranges::range_reference_t<const FContentBrowserItemRange>,
		const Durin::ContentBrowser::FContentBrowserItem&>);
	FContentBrowserItemRange Original;
	const Durin::ContentBrowser::FContentBrowserItem* First = nullptr;
	{
		FProviderQueryFixture ModelSource;
		FContentBrowserModel Model(ModelSource);
		const auto Directory = (Root / "Content").generic_string();
		Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::SetSnapshot(Model, Directory, Durin::ContentBrowser::Private::ItemDescriptions({
			{.Name = "B", .PhysicalPath = Directory + "/B", .FileSize = 10},
			{.Name = "A", .PhysicalPath = Directory + "/A", .FileSize = 20}}));
		Original = Model.GetItems();
		ASSERT_EQ(Original.size(), 2);
		EXPECT_EQ(Original.GetIndices()[0], 1);
		First = &Original.front();
		EXPECT_EQ(First, &Model.GetItemsSnapshot()[1]);
		Model.SetSort(EContentBrowserSortColumn::Size, true);
		FContentBrowserModelFixtureAccess::Wait(Model);
		EXPECT_EQ(TestName(Model.GetItems().front()), "B");
		EXPECT_EQ(&Model.GetItems()[1], First);
		EXPECT_EQ(TestName(Original.front()), "A");
		Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Search(Model, "/ContentBrowserTests/B");
		ASSERT_EQ(Model.GetItems().size(), 1);
		EXPECT_EQ(&Model.GetItems().front(), &Model.GetItemsSnapshot()[0]);
		Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::SetSnapshot(Model, Directory, {});
		EXPECT_TRUE(Model.GetItems().empty());
	}
	ASSERT_EQ(Original.size(), 2);
	EXPECT_EQ(&Original.front(), First);
	EXPECT_EQ(TestName(Original.front()), "A");
	EXPECT_EQ(std::distance(Original.begin(), Original.end()), 2);
	EXPECT_EQ((Original.end() - 1)->GetItemName(), "B");
}

TEST_F(FContentBrowserModelTests, QueryKeepsStableTiesAndCopiesOnlySelectedCommandInputs)
{
	auto Snapshot = std::make_shared<FContentBrowserItemsSnapshot>();
	Snapshot->Items = ItemDescriptions({
		{.Name = "Same", .PhysicalPath = "/content/first", .Extension = ".txt"},
		{.Name = "Same", .PhysicalPath = "/content/second", .Extension = ".txt"},
		{.Kind = EContentBrowserItemKind::Folder, .Name = "Folder", .PhysicalPath = "/content/folder"},
		{.Name = "Hidden", .PhysicalPath = "/content/.hidden/entry"},
		{.Name = "Outside", .PhysicalPath = "/outside/entry"}});
	FContentBrowserQuerySettings Query;
	Query.SortColumn = EContentBrowserSortColumn::Type;
	Query.bSortAscending = false;
	FProviderQueryFixture Provider;
	for (auto& Item : Snapshot->Items) Item = Provider.BindItemForTesting(std::move(Item));
	const auto Projection = ContentBrowserQuery::Project(Snapshot, "/content", Query);
	ASSERT_EQ(Projection.size(), 3);
	EXPECT_EQ(TestName(Projection[0]), "Folder");
	EXPECT_EQ(TestPhysicalPath(Projection[1]), "/content/first");
	EXPECT_EQ(TestPhysicalPath(Projection[2]), "/content/second");
	const auto Selected = ContentBrowserQuery::CopySelection(Projection,
		{Projection[2].GetItemKey(), Snapshot->Items[3].GetItemKey()});
	ASSERT_EQ(Selected.size(), 1);
	EXPECT_EQ(TestPhysicalPath(Selected.front()), "/content/second");
	Query.Search = "Hidden";
	EXPECT_TRUE(ContentBrowserQuery::Project(Snapshot, "/content", Query).empty());
	Query.bShowHiddenFiles = true;
	EXPECT_EQ(ContentBrowserQuery::Project(Snapshot, "/content", Query).size(), 1);
	EXPECT_TRUE(ContentBrowserQuery::Project({}, "/content", Query).empty());
}

TEST_F(FContentBrowserModelTests, BulkPayloadVisibilityDoesNotDependOnOwnershipOrHiddenFileSettings)
{
	auto Snapshot = std::make_shared<FContentBrowserItemsSnapshot>();
	Snapshot->Items = ItemDescriptions({
		{.Kind = EContentBrowserItemKind::Asset, .Name = "Mesh", .PhysicalPath = "/content/Mesh.dasset"},
		{.Name = "Mesh.dbulk", .PhysicalPath = "/content/Mesh.dbulk"},
		{.Name = "Orphan.DBULK", .PhysicalPath = "/content/Orphan.DBULK"},
		{.Name = "Mesh.fbx", .PhysicalPath = "/content/Mesh.fbx"},
		{.Name = "settings.json", .PhysicalPath = "/content/settings.json"},
		{.Name = "README.md", .PhysicalPath = "/content/README.md"},
		{.Name = "Nested.dbulk", .PhysicalPath = "/content/sub/Nested.dbulk"},
		{.Name = ".hidden.txt", .PhysicalPath = "/content/.hidden.txt"},
		{.Kind = EContentBrowserItemKind::Folder, .Name = "folder.dbulk", .PhysicalPath = "/content/folder.dbulk"}});
	FProviderQueryFixture Provider;
	for (auto& Item : Snapshot->Items) Item = Provider.BindItemForTesting(std::move(Item));
	FContentBrowserQuerySettings Query;
	EXPECT_EQ(ContentBrowserQuery::Project(Snapshot, "/content", Query).size(), 5);
	Query.TypeFilter = EContentBrowserTypeFilter::Files;
	EXPECT_EQ(ContentBrowserQuery::Project(Snapshot, "/content", Query).size(), 4);
	Query.bShowHiddenFiles = true;
	EXPECT_EQ(ContentBrowserQuery::Project(Snapshot, "/content", Query).size(), 5);
	Query.Search = "Nested.dbulk";
	EXPECT_TRUE(ContentBrowserQuery::Project(Snapshot, "/content", Query).empty());
	Query.Search = "Orphan";
	EXPECT_TRUE(ContentBrowserQuery::Project(Snapshot, "/content", Query).empty());
	Query.Search = "Mesh";
	const auto Files = ContentBrowserQuery::Project(Snapshot, "/content", Query);
	ASSERT_EQ(Files.size(), 1);
	EXPECT_EQ(TestName(Files.front()), "Mesh.fbx");
	EXPECT_EQ(Snapshot->Items.size(), 9);
}

TEST_F(FContentBrowserModelTests, RepeatedNavigationToCurrentDirectoryKeepsPublishedSnapshot)
{
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	const std::string Directory =
		std::filesystem::absolute(Root / "Content/A")
			.lexically_normal()
			.generic_string();
	ASSERT_TRUE(Model.NavigateToPhysical(Directory));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::SetSnapshot(Model, Directory, Durin::ContentBrowser::Private::ItemDescriptions({
		{.Kind = EContentBrowserItemKind::File,
			.Name = "Stable.txt",
			.PhysicalPath = Directory + "/Stable.txt",
			.Extension = ".txt"},
	}));

	ASSERT_TRUE(Model.NavigateToPhysical(Directory));
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Stable.txt");
	EXPECT_EQ(Model.GetHistory().size(), 1);
}

TEST_F(FContentBrowserModelTests, DirectoryNavigationPreservesTreeChildrenSnapshots)
{
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	const std::string TreeRoot =
		std::filesystem::absolute(Root / "Content")
			.lexically_normal()
			.generic_string();
	Model.RefreshMountSnapshot();
	Model.RequestDirectoryChildrenSnapshot(TreeRoot);
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
	ASSERT_EQ(Model.GetDirectorySnapshot(TreeRoot)->Children.size(), 2);

	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
	EXPECT_EQ(Model.GetDirectorySnapshot(TreeRoot)->Children.size(), 2);
}

TEST_F(FContentBrowserModelTests, RejectsUnavailableDirectoryWithoutFilesystemException)
{
	const std::filesystem::path Unavailable = Root / "Content/B";
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.NavigateToPhysical(Unavailable.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
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
	const FPackageCatalogEntry AssetData =
		FindPackageExact(AssetPath);
	ASSERT_NE(AssetData, nullptr);
	auto ThumbnailRegistration = Durin::FThumbnailManagerTestAccess::RegisterScoped(GetDefaultThumbnailManager(),
			Durin::MakeThumbnailRenderer<FRouteOnlyThumbnailRenderer>(), TestAssetClassName(AssetData->TopLevelAssets.front()));
	ASSERT_TRUE(ThumbnailRegistration);

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto It = std::ranges::find_if(
		Model.GetItems(),
		[&](const auto& Item) {
			return TestPackagePath(Item) == AssetPath;
		});
	ASSERT_NE(It, Model.GetItems().end());
	const std::string ExactAssetPath =
		Testing::MakePackageLeafTopLevelAssetPathForTests(AssetPath).ToString();
	EXPECT_EQ(TestVirtualPath(*(It)), ExactAssetPath);
	FContentBrowserThumbnailReferences References;
	References.Request(*It, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 1u);
	// Renderer registration belongs to the request service, not a published Model row.
	ThumbnailRegistration = {};
	References.Request(*It, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 0u);

	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, DirectoryProjectionUsesMountedPathsWithoutSyntheticChildren)
{
	const auto Content = (Root / "Content").generic_string();
	const auto Child = (Root / "Content/A").generic_string();
	EXPECT_EQ(ContentStorage::FContentMountPaths::VirtualToPhysical("/ContentBrowserTests/"), Content);
	EXPECT_EQ(ContentStorage::FContentMountPaths::VirtualToPhysical("/contentbrowsertests"), Content);
	EXPECT_EQ(ContentStorage::FContentMountPaths::VirtualToPhysical("/ContentBrowserTests/A/"), Child);
	EXPECT_EQ(ContentStorage::FContentMountPaths::PhysicalToVirtualDirectory(Child), "/ContentBrowserTests/A/");
	EXPECT_TRUE(ContentStorage::FContentMountPaths::VirtualToPhysical("/ContentBrowserTests/../Escape").empty());
	EXPECT_TRUE(ContentStorage::FContentMountPaths::VirtualToPhysical("/ContentBrowserTests//A").empty());
	EXPECT_TRUE(ContentStorage::FContentMountPaths::VirtualToPhysical("/Unknown/A").empty());
	EXPECT_EQ(ContentStorage::FContentMountPaths::VirtualToPhysical("/ContentBrowserTests/Missing/"),
		(Root / "Content/Missing").generic_string());
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.NavigateToPhysical((Root / "Excluded").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_TRUE(Model.PhysicalToVirtualDirectory(
		(Root / "Excluded").generic_string()).empty());
	EXPECT_EQ(
		Model.GetCurrentPhysicalPath(),
		std::filesystem::absolute(Root / "Content")
			.lexically_normal()
			.generic_string());

	FProviderFolderFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	FContentBrowserItem CreateExcludedItem;
	const bool CreateExcluded =
		Operations.CreateFolder((Root / "Excluded").generic_string(), "New Folder", CreateExcludedItem);
	EXPECT_FALSE(CreateExcluded);
	EXPECT_TRUE(OperationError.find("automatically scanned")
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_TRUE(Model.NavigateToPhysical((Root / "Replacement").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	ASSERT_EQ(Model.GetMounts().size(), 2u);
	EXPECT_EQ(Model.GetMounts()[0].VirtualRoot, "/Game/");
	EXPECT_EQ(Model.GetMounts()[1].VirtualRoot, "/Engine/");
}

TEST_F(FContentBrowserModelTests, RefreshesMountSnapshotAfterRegistryReplacementAndRestoration)
{
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	ASSERT_EQ(Model.GetMounts().size(), 1u);
	const std::string OriginalRoot = Model.GetMounts()[0].PhysicalRoot;
	const auto* OriginalSnapshot = Model.GetMounts().data();
	Model.RefreshMountSnapshot();
	EXPECT_EQ(Model.GetMounts().data(), OriginalSnapshot);

	// Same virtual mount, different physical root and write policy must invalidate the cache.
	const std::array Definitions{FMountPoint{
		.VirtualRoot = "/ContentBrowserTests/", .Owner = EMountOwner::Test,
		.Root = Root / "Content/A", .bAutoScan = true, .bContentWritable = false}};
	{
		Testing::FScopedMountRegistryFixture Replacement(Definitions);
		ASSERT_TRUE(Replacement.IsValid()) << Replacement.GetError();
		Model.RefreshMountSnapshot();
		ASSERT_EQ(Model.GetMounts().size(), 1u);
		EXPECT_NE(Model.GetMounts()[0].PhysicalRoot, OriginalRoot);
		EXPECT_FALSE(Model.GetMounts()[0].bContentWritable);
	}
	Model.RefreshMountSnapshot();
	ASSERT_EQ(Model.GetMounts().size(), 1u);
	EXPECT_EQ(Model.GetMounts()[0].PhysicalRoot, OriginalRoot);
	EXPECT_TRUE(Model.GetMounts()[0].bContentWritable);
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

	const auto TargetDirectory = DestinationPath.ToString().substr(0, DestinationPath.ToString().find_last_of('/'));
	const FAssetRenameData Mapping{TWeakObjectPtr<DObject>(Material), TargetDirectory, DestinationPath.GetPackageName()};
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Mapping, 1}));
	EXPECT_TRUE(Transactions->GetUndoContext(false).Description.empty());
	EXPECT_EQ(ResolvePackagePathForOperation(SourcePath).FinalPath,
		DestinationPath);

	EXPECT_FALSE(Transactions->Undo());
	Transactions->Reset();
}

TEST_F(FContentBrowserModelTests, SearchesRecursivelyButBrowsesImmediateChildren)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::SetSnapshot(Model,
		RootPath,
		Durin::ContentBrowser::Private::ItemDescriptions({
			{.Kind = EContentBrowserItemKind::Folder,
				.Name = "Textures",
				.PhysicalPath = RootPath + "/Textures"},
			{.Kind = EContentBrowserItemKind::Asset,
				.Name = "Stone",
				.VirtualPath = "/ContentBrowserTests/Textures/Stone",
				.PhysicalPath = RootPath + "/Textures/Stone.dasset",
				.AssetClassName = "Durin::DTexture2D"},
		}));

	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Textures");
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Search(Model, "stone");
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Stone");
}

TEST_F(FContentBrowserModelTests, AssetLifecycleNotificationsRefreshCreationSaveRenameAndDiscard)
{
	InitializeDObjectSystem();
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	FPackagePath Path, DiscardPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Live", Path));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Discard", DiscardPath));
	(void)CaptureAssetQuerySnapshot();
	FContentChangeBatch PendingChanges;
	const auto Changed = OnAssetQueryChanged().AddLambda([&](const FContentChangeBatch& Batch) { PendingChanges.bFullRefresh |= Batch.bFullRefresh; PendingChanges.Changes.insert(PendingChanges.Changes.end(), Batch.Changes.begin(), Batch.Changes.end()); });
	struct FUnsubscribe { FDelegateHandle Handle; ~FUnsubscribe() { OnAssetQueryChanged().Remove(Handle); } } Unsubscribe{Changed};
	auto FlushEvents = [&] { (void)CaptureAssetQuerySnapshot(); EXPECT_TRUE(Model.ApplyContentChanges(std::exchange(PendingChanges, {})));
	FContentBrowserModelFixtureAccess::Wait(Model); };

	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	FProviderAssetActionFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	EXPECT_TRUE(Operations.CanDuplicateItem(AssetItem(Path)));
	FlushEvents();
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(TestFileSize(Model.GetItems().front()), 0u);

	Material->Rename("Renamed");
	FlushEvents();
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_TRUE(TestVirtualPath(Model.GetItems().front()).ends_with(".Renamed"));

	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FlushEvents();
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_GT(TestFileSize(Model.GetItems().front()), 0u);

	ASSERT_TRUE(UnloadPackage(Path));
	FlushEvents();
	EXPECT_EQ(Model.GetItems().size(), 1u);

	DMaterial* Unsaved = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(DiscardPath, Unsaved));
	FlushEvents();
	ASSERT_EQ(Model.GetItems().size(), 2u);

	ASSERT_TRUE(UnloadPackage(DiscardPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	FlushEvents();
	EXPECT_EQ(Model.GetItems().size(), 1u);
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(Path));
}

TEST_F(FContentBrowserModelTests, ShowsAndRevealsUnsavedAssetsWithoutCreatingDirectories)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Unsaved/Nested/Material", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	const auto Directory = Root / "Content/Unsaved/Nested";
	EXPECT_FALSE(std::filesystem::exists(Directory));
	EXPECT_FALSE(FindPackageExact(Path));
	{
		FProviderQueryFixture ModelSource;
		FContentBrowserModel Model(ModelSource);
		ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
		FContentBrowserModelFixtureAccess::Wait(Model);
		EXPECT_TRUE(std::ranges::any_of(Model.GetItems(), [](const auto& Item) {
			return TestKind(Item) == EContentBrowserItemKind::Folder && TestName(Item) == "Unsaved";
		}));
		EXPECT_FALSE(!Model.RevealAsset(Testing::MakePackageLeafTopLevelAssetPathForTests(Path).ToString()).IsValid());
		FContentBrowserModelFixtureAccess::Wait(Model);
		ASSERT_EQ(Model.GetItems().size(), 1u);
		EXPECT_EQ(TestPackagePath(Model.GetItems()[0]), Path);
		EXPECT_EQ(TestAssetClassName(Model.GetItems()[0]), DMaterial::StaticClass()->GetQualifiedName().ToString());
		Model.RequestDirectoryChildrenSnapshot((Root / "Content/Unsaved").generic_string());
		FContentBrowserModelFixtureAccess::Wait(Model);
		const auto Children = Model.GetDirectorySnapshot((Root / "Content/Unsaved").generic_string());
		ASSERT_TRUE(Children);
		EXPECT_EQ(Children->Children, std::vector<std::filesystem::path>{Directory});
		EXPECT_TRUE(Model.GetEnumerationDiagnostics().empty());
		EXPECT_FALSE(std::filesystem::exists(Directory));
	}
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(Directory.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetItems().size(), 1u);
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(Path));
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetSearch("does-not-match");
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.GetItems().empty());

	const auto Revealed = Model.RevealAsset(
		Testing::MakePackageLeafTopLevelAssetPathForTests(AssetPath).ToString());
	FContentBrowserModelFixtureAccess::Wait(Model);

	EXPECT_FALSE(!Revealed.IsValid());
	EXPECT_TRUE(Model.GetSearch().empty());
	EXPECT_EQ(Model.GetTypeFilter(), EContentBrowserTypeFilter::All);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[&](const auto& Item) {
			return Item.GetItemKey() == Revealed;
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
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Material), "/ContentBrowserTests", DestinationPath.GetPackageName()};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const FTopLevelAssetPath AssetPath =
		Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath);
	FTopLevelAssetPath RelocatedAssetPath;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(
		DestinationPath, DestinationPath.GetPackageName(), RelocatedAssetPath));
	const auto Revealed = Model.RevealAsset(RelocatedAssetPath.ToString());
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_FALSE(!Revealed.IsValid());
	const auto It = std::ranges::find_if(
		Model.GetItems(), [&](const auto& Item) {
			return Item.GetItemKey() == Revealed;
		});
	ASSERT_NE(It, Model.GetItems().end());
	EXPECT_EQ(TestName(*(It)), "DisplayNameDestination");
	EXPECT_EQ(TestVirtualPath(*(It)), RelocatedAssetPath.ToString());
	EXPECT_TRUE(Testing::RemoveAssetPackageForTests(SourcePath));
	EXPECT_TRUE(Testing::RemoveAssetPackageForTests(DestinationPath));
}

TEST_F(FContentBrowserModelTests, RevealPhysicalItemPublishesNewFolderHiddenByFilters)
{
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetSearch("does-not-match");
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.GetItems().empty());

	FProviderFolderFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	FContentBrowserItem CreateResultItem;
	const bool CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string(), "New Folder", CreateResultItem);
	ASSERT_TRUE(CreateResult) << OperationError;

	const auto Revealed =
		Model.RevealPhysicalItem(TestPhysicalPath(CreateResultItem));
		FContentBrowserModelFixtureAccess::Wait(Model);

	EXPECT_EQ(Revealed, Durin::ContentBrowser::FContentBrowserItemKey(Durin::ContentBrowser::EContentBrowserItemFlags::Type_Folder, Model.PhysicalToVirtualDirectory(TestPhysicalPath(CreateResultItem)), {}));
	EXPECT_TRUE(Model.GetSearch().empty());
	EXPECT_EQ(Model.GetTypeFilter(), EContentBrowserTypeFilter::All);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[&](const auto& Item) {
			return TestKind(Item) == EContentBrowserItemKind::Folder
				&& Item.GetItemKey() == Revealed;
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	bool bObservedNestedFile = false;
	Model.SetEntryStatusQueryForTesting(
		[&](const std::filesystem::directory_entry& Entry,
			std::error_code& Error) {
			if (Entry.path().filename() == "Needle.txt")
				bObservedNestedFile = true;
			return Entry.symlink_status(Error);
		});

	ASSERT_TRUE(Model.NavigateToPhysical(ScanRoot.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(bObservedNestedFile);
	EXPECT_EQ(std::ranges::count_if(
		Model.GetItems(),
		[](const auto& Item) { return TestName(Item) == "Needle.txt"; }), 0);

	Model.SetSearch("needle");
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_TRUE(bObservedNestedFile);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Needle.txt");

	bObservedNestedFile = false;
	Model.SetSearch(std::string_view{});
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(bObservedNestedFile);
}

TEST_F(FContentBrowserModelTests, ShowsOrdinaryFilesByDefaultAndHidesAssetPackages)
{
	{
		std::ofstream RawSource(Root / "Content/Raw.png");
		RawSource << "raw";
		std::ofstream AssetPackage(Root / "Content/Raw.dasset");
		AssetPackage << "package";
		std::ofstream(Root / "Content/Raw.dbulk") << "bulk";
		std::ofstream(Root / "Content/Orphan.DBULK") << "orphan";
	}
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto File = std::ranges::find(Model.GetItems(), "Raw.png", [](const auto& Item) { return Item.GetItemName(); });
	ASSERT_NE(File, Model.GetItems().end());
	EXPECT_EQ(TestKind(*(File)), EContentBrowserItemKind::File);
	EXPECT_EQ(TestExtension(*(File)), ".png");
	EXPECT_EQ(TestFileSize(*(File)), 3);
	EXPECT_TRUE(std::ranges::none_of(
		Model.GetItems(),
		[](const auto& Item) {
			return TestName(Item) == "Raw.dasset" || TestName(Item) == "Raw.dbulk" || TestName(Item) == "Orphan.DBULK";
		}));
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
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetItems().size(), 2);
	ASSERT_EQ(Model.GetEnumerationDiagnostics().size(), 1);
	EXPECT_EQ(
		TestKind(Model.GetEnumerationDiagnostics().front()),
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
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.GetDirectorySnapshot(TreeRoot.generic_string()));
	EXPECT_EQ(Model.GetDirectorySnapshot(TreeRoot.generic_string())->Children.size(), 2);
	ASSERT_EQ(Model.GetEnumerationDiagnostics().size(), 1);
	EXPECT_EQ(
		TestKind(Model.GetEnumerationDiagnostics().front()),
		FContentBrowserModel::EEnumerationDiagnosticKind::Entry);
	EXPECT_FALSE(Model.GetDirectorySnapshot(
		(Root / "MissingDirectory").generic_string()));
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.RequestDirectoryChildrenSnapshot(TreeRoot.generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto PublishedChildrenSnapshot = Model.GetDirectorySnapshot(TreeRoot.generic_string());
	ASSERT_TRUE(PublishedChildrenSnapshot);
	const auto& PublishedChildren = PublishedChildrenSnapshot->Children;
	ASSERT_EQ(PublishedChildren.size(), BranchCount);
	const std::filesystem::path* PublishedStorage = PublishedChildren.data();
	const std::vector<std::filesystem::path> ExpectedChildren(
		PublishedChildren.begin(), PublishedChildren.end());

	for (const std::filesystem::path& Branch : Branches)
		Model.RequestDirectoryChildrenSnapshot(Branch.generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);

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
		FProviderQueryFixture ModelSource;
		FContentBrowserModel Model(ModelSource);
		ASSERT_TRUE(Model.NavigateToPhysical(Directory));
		Model.RequestDirectoryChildrenSnapshot(Directory);
		FContentBrowserModelFixtureAccess::Wait(Model);
		RetainedItems = Model.GetItems();
		RetainedTree = Model.GetDirectorySnapshot(Directory);
		ASSERT_EQ(RetainedItems.size(), 1);
		ASSERT_TRUE(RetainedTree);
		ASSERT_EQ(RetainedTree->Children.size(), 1);
		EXPECT_FALSE(Model.PumpPendingSnapshots());

		std::filesystem::create_directories(std::filesystem::path(Directory) / "Next");
		Model.RefreshItemsSnapshot();
		Model.RequestDirectoryChildrenSnapshot(Directory);
		FContentBrowserModelFixtureAccess::Wait(Model);
		EXPECT_EQ(Model.GetItems().size(), 2);
		const auto NextTree = Model.GetDirectorySnapshot(Directory);
		ASSERT_TRUE(NextTree);
		EXPECT_EQ(NextTree->Children.size(), 2);
		EXPECT_NE(NextTree, RetainedTree);
	}
	ASSERT_EQ(RetainedItems.size(), 1);
	EXPECT_EQ(TestName(RetainedItems.front()), "Original");
	EXPECT_EQ(TestVirtualPath(RetainedItems.front()), "/ContentBrowserTests/Owned/Original");
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
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_FALSE(Model.IsLoading());
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Latest");
	EXPECT_EQ(SkippedVisits->load(), 0);
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), C.generic_string());
}

TEST_F(FContentBrowserModelTests, AsyncSearchPublishesUsingLatestQueryWithoutRestartingCapture)
{
	InitializeDObjectSystem();
	const auto A = Root / "Content/A";
	std::filesystem::create_directories(A / "Nested/Needle");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	Model.SetSearch("unmatched");
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Requests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	Model.SetSearch("Needle");
	Model.SetSort(EContentBrowserSortColumn::Name, false);
	EXPECT_EQ(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
	Gate->Release();
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Needle");
}

TEST_F(FContentBrowserModelTests, AsyncTreeDiscardsResultsInvalidatedDuringEnumeration)
{
	InitializeDObjectSystem();
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Old");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	Model.SetEntryStatusQueryForTesting({});
	std::filesystem::create_directory(A / "New");
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
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
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.SetEntryStatusQueryForTesting([](const auto&, auto&) -> std::filesystem::file_status {
		throw std::runtime_error("injected capture failure");
	});
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	ASSERT_FALSE(Model.GetEnumerationDiagnostics().empty());
	EXPECT_NE(Model.GetEnumerationDiagnostics().front().Message.find("injected capture failure"), std::string::npos);
	Model.SetEntryStatusQueryForTesting({});
	Model.RefreshItemsSnapshot();
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_TRUE(Model.GetEnumerationDiagnostics().empty());
	ASSERT_EQ(Model.GetItems().size(), 1);
}

TEST_F(FContentBrowserModelTests, AsyncRevealReturnsIdentityBeforeDirectoryCaptureCompletes)
{
	InitializeDObjectSystem();
	const auto Folder = Root / "Content/A/Revealed";
	std::filesystem::create_directory(Folder);
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	const auto Revealed = Model.RevealPhysicalItem(Folder.generic_string());
	ASSERT_TRUE(Revealed.IsValid());
	EXPECT_EQ(Revealed.GetVirtualPath(), "/ContentBrowserTests/A/Revealed");
	// Reveal can replace a capture whose cancellation has not yet retired.
	// Keep the normal owner pump progressing so the replacement can be submitted.
	ASSERT_TRUE(Gate->WaitUntilStarted([&] { (void)Model.PumpPendingSnapshots(); }));
	EXPECT_TRUE(Model.IsLoading());
	Gate->Release();
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().GetItemKey(), Revealed);
}

TEST_F(FContentBrowserModelTests, RetainedDirectorySnapshotSurvivesInvalidationAndModelDestruction)
{
	std::shared_ptr<const FContentBrowserDirectorySnapshot> Published;
	const std::string TreeRoot = (Root / "Content").generic_string();
	{
		FProviderQueryFixture ModelSource;
		FContentBrowserModel Model(ModelSource);
		ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot));
		FContentBrowserModelFixtureAccess::Wait(Model);
		Model.RequestDirectoryChildrenSnapshot(TreeRoot);
		FContentBrowserModelFixtureAccess::Wait(Model);
		Published = Model.GetDirectorySnapshot(TreeRoot);
		ASSERT_NE(Published, nullptr);
		ASSERT_EQ(Published->Children.size(), 2);
		std::filesystem::create_directory(Root / "Content/C");
		Model.RefreshItemsSnapshot();
		FContentBrowserModelFixtureAccess::Wait(Model);
		EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
		Model.RequestDirectoryChildrenSnapshot(TreeRoot);
		FContentBrowserModelFixtureAccess::Wait(Model);
		const auto Next = Model.GetDirectorySnapshot(TreeRoot);
		ASSERT_NE(Next, nullptr);
		EXPECT_NE(Next, Published);
		EXPECT_EQ(Next->Children.size(), 3);
	}
	EXPECT_EQ(Published->Children.size(), 2);
	EXPECT_EQ(Published->Children.front().filename(), "A");
}

TEST_F(FContentBrowserModelTests, SearchEditsAndViewSettingsReuseCapturedData)
{
	std::filesystem::create_directories(Root / "Content/A/Nested");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto DirectoryRequests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	Model.SetSearch("Nested");
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto RecursiveRequests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	EXPECT_NE(RecursiveRequests, DirectoryRequests);
	ASSERT_EQ(Model.GetItems().size(), 1);
	Model.SetSearch("Nest");
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetSort(EContentBrowserSortColumn::Modified, false);
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetShowHiddenFiles(true);
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), RecursiveRequests);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Nested");
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto RootChildrenSnapshot = Model.GetDirectorySnapshot(TreeRoot.generic_string());
	ASSERT_TRUE(RootChildrenSnapshot);
	const auto& RootChildren = RootChildrenSnapshot->Children;
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

		FContentBrowserModelFixtureAccess::Wait(Model);
		EXPECT_TRUE(Model.HasDirectoryChildrenSnapshot(Directory.generic_string()));
		EXPECT_EQ(RootChildren.data(), RootStorage);
		EXPECT_TRUE(std::ranges::equal(RootChildren, ExpectedRootChildren));
	}
}

TEST_F(FContentBrowserModelTests, FiltersFilesSeparatelyAndKeepsHiddenContentOptIn)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::SetSnapshot(Model,
		RootPath,
		Durin::ContentBrowser::Private::ItemDescriptions({
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
		}));

	ASSERT_EQ(Model.GetItems().size(), 2);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Stone.png");
	Model.SetShowHiddenFiles(true);
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 2);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[](const auto& Item) { return TestName(Item) == ".internal"; }));
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Search(Model, "notes");
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "notes.txt");
}

TEST_F(FContentBrowserModelTests, HidesRedirectorsButPreservesFoldersAndSupportsExplicitFilter)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FSoftObjectPath Destination;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(
		"/ContentBrowserTests/Final/Stone.Stone", Destination));
	FPackagePath RedirectorPackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/OldStone", RedirectorPackagePath));
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::SetSnapshot(Model,
		RootPath,
		Durin::ContentBrowser::Private::ItemDescriptions({
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
		}));

	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(TestKind(Model.GetItems().front()), EContentBrowserItemKind::Folder);
	Model.SetShowRedirectors(true);
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(), [](const auto& Item) {
			return TestKind(Item) == EContentBrowserItemKind::Redirector;
		}));
	Model.SetShowRedirectors(false);
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Redirectors);
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(), [](const auto& Item) {
			return TestKind(Item) == EContentBrowserItemKind::Redirector
				&& TestName(Item) == "OldStone";
		}));
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Search(Model, "Final/Stone");
	ASSERT_EQ(Model.GetItems().size(), 1u);
}

TEST_F(FContentBrowserModelTests, KeepsFoldersFirstAndSortsEqualKeysStably)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::SetSnapshot(Model,
		RootPath,
		Durin::ContentBrowser::Private::ItemDescriptions({
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
		}));
	Model.SetSort(EContentBrowserSortColumn::Size, true);
	FContentBrowserModelFixtureAccess::Wait(Model);

	ASSERT_EQ(Model.GetItems().size(), 3);
	EXPECT_EQ(TestKind(Model.GetItems()[0]), EContentBrowserItemKind::Folder);
	EXPECT_TRUE(TestPhysicalPath(Model.GetItems()[1]).ends_with("second.dasset"));
	EXPECT_TRUE(TestPhysicalPath(Model.GetItems()[2]).ends_with("first.dasset"));
}

TEST_F(FContentBrowserModelTests, OperationsRejectCollisionsAndRenameOrdinaryFolders)
{
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);

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
	FContentBrowserItem CollisionResultItem;
	const bool CollisionResult =
		Operations.RenameItem(SourceItem, "taken.txt", CollisionResultItem);
	EXPECT_FALSE(CollisionResult);
	EXPECT_NE(OperationError.find("occupied"), std::string::npos);

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
	FContentBrowserItem FolderResultItem;
	const bool FolderResult =
		Operations.RenameItem(FolderItem, "Renamed", FolderResultItem);
	ASSERT_TRUE(FolderResult) << OperationError;
	EXPECT_FALSE(std::filesystem::exists(Folder));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/Renamed/notes.txt"));
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
		"/ContentBrowserTests/Original2", DuplicatePath));
	ASSERT_TRUE(FPackagePath::TryCreate(
		"/ContentBrowserTests/A/Original", PastedPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Material));
	FMaterialStaticProperties Properties = Material->GetStaticProperties();
	Properties.bTwoSided = true;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	{
		std::ofstream Occupied(Root / "Content/Original1.dasset");
		Occupied << "unregistered collision";
	}

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FProviderAssetActionFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "Original",
		.VirtualPath = Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath).ToString(),
		.PackagePath = SourcePath,
		.PhysicalPath = (Root / "Content/Original.dasset").generic_string(),
		.AssetClassName = DMaterial::StaticClass()
			->GetQualifiedName().ToString()};

	FContentBrowserItem ResultItem;
	const bool Result = Operations.DuplicateItem(Item, ResultItem);

	ASSERT_TRUE(Result) << OperationError;
	EXPECT_EQ(TestName(ResultItem), TestPackagePath(ResultItem).GetPackageName());
	EXPECT_EQ(TestVirtualPath(ResultItem),
		Testing::MakePackageLeafTopLevelAssetPathForTests(DuplicatePath).ToString());
	EXPECT_EQ(TestPhysicalPath(ResultItem),
		std::filesystem::absolute(Root / "Content/Original2.dasset")
			.lexically_normal().generic_string());
	DMaterial* Duplicate = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(DuplicatePath), nullptr, &LoadedValueError);
		Duplicate = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_NE(Duplicate, Material);
	EXPECT_EQ(Duplicate->GetStaticProperties(), Properties);
	EXPECT_TRUE(Duplicate->GetPackage()->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(ResultItem)));
	FContentBrowserItem PastedResultItem;
	const bool PastedResult = Operations.CopyToFolder(
		AssetItem(SourcePath),
		(Root / "Content/A").generic_string(), PastedResultItem);
	ASSERT_TRUE(PastedResult) << OperationError;
	EXPECT_EQ(TestVirtualPath(PastedResultItem),
		Testing::MakePackageLeafTopLevelAssetPathForTests(PastedPath).ToString());
	DMaterial* Pasted = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(PastedPath), nullptr, &LoadedValueError);
		Pasted = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Pasted, nullptr);
	EXPECT_EQ(Pasted->GetStaticProperties(), Properties);
	ASSERT_TRUE(SavePackage(Pasted->GetPackage()));
	ASSERT_TRUE(SavePackage(Duplicate->GetPackage()));
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem FileItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "Independent.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	FContentBrowserItem ResultItem;
	const bool Result =
		Operations.RenameItem(FileItem, "Renamed.txt", ResultItem);
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/Renamed.txt"));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FContentBrowserModelTests, OwnedCompanionCannotMoveOrRenameIndependently)
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
		[AssetPath, Companion](const FPackageData& Data,
			const FAssetPackageInspection&,
			FAssetDeleteContribution& Contribution, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			if (TestPackagePath(Data) == AssetPath)
				Contribution.Files.push_back(Companion);
			return true;
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};

	FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(QueryAssetCompanionOwnership(Companion, Ownership));
	ASSERT_EQ(Ownership.State, EAssetCompanionOwnershipState::Owned);
	ASSERT_EQ(Ownership.Owners.size(), 1);
	EXPECT_EQ(Ownership.Owners.front(), AssetPath);
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem CompanionItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "Owned.meta",
		.PhysicalPath = Companion.generic_string(),
		.Extension = ".meta"};
	FProviderQueryFixture Query;
	const auto Bound = Query.BindContributionsForTesting(CompanionItem, {});
	ASSERT_EQ(1u, Bound.GetInternalItems().size());
	const auto& Selected = Bound.GetInternalItems().front();
	auto* FileSource = Selected.GetOwnerDataSource(); ASSERT_NE(nullptr, FileSource);
	EXPECT_FALSE(FileSource->CanRenameItem(Selected));
	EXPECT_FALSE(FileSource->CanCopyItem(Selected, "/ContentBrowserTests/B"));
	EXPECT_FALSE(FileSource->CanMoveItem(Selected, "/ContentBrowserTests/B"));
	FContentBrowserItem RenameResultItem;
	const bool RenameResult =
		Operations.RenameItem(CompanionItem, "Other.meta", RenameResultItem);
	EXPECT_FALSE(RenameResult);
	EXPECT_TRUE(OperationError.find(AssetPath.ToString())
		!= std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Companion));
	EXPECT_FALSE(Operations.BulkMoveItems(std::span{&CompanionItem, 1}, (Root / "Content/B").generic_string()));
	EXPECT_FALSE(Operations.BulkCopyItems(std::span{&CompanionItem, 1}, (Root / "Content/B").generic_string()));
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(*(FindPackageExact(AssetPath)))));

}

TEST_F(FContentBrowserModelTests, FolderRenameRetainsSourceRedirectorAndWarnsOnCleanup)
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "CleanupSource",
		.PhysicalPath = Folder.generic_string()};

	FContentBrowserItem ResultItem;
	const bool Result =
		Operations.RenameItem(FolderItem, "CleanupDestination", ResultItem);

	ASSERT_TRUE(Result) << OperationError;
	EXPECT_TRUE(OperationWarning.find("cleanup failed") != std::string::npos
		|| OperationWarning.find("could not be removed") != std::string::npos);
	EXPECT_EQ(TestPhysicalPath(ResultItem),
		std::filesystem::absolute(Root / "Content/CleanupDestination")
			.lexically_normal().generic_string());
	EXPECT_EQ(ResolvePackagePathForOperation(SourcePath).FinalPath, DestinationPath);
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
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);

	FContentBrowserItem CreateResultItem;
	const bool CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string(), "New Folder", CreateResultItem);
	EXPECT_FALSE(CreateResult);

	EXPECT_FALSE(OperationError.empty());
	EXPECT_TRUE(OperationError.find("read-only") != std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/New Folder"));

	const FContentBrowserItem FileItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "notes.txt",
		.PhysicalPath = File.generic_string(),
		.Extension = ".txt"};
	FContentBrowserItem FileResultItem;
	const bool FileResult =
		Operations.RenameItem(FileItem, "renamed.txt", FileResultItem);
	EXPECT_FALSE(FileResult) << OperationError;

	EXPECT_FALSE(OperationError.empty());
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/renamed.txt"));
	EXPECT_FALSE(Operations.BulkMoveItems(std::span{&FileItem, 1}, (Root / "Content/B").generic_string()));

	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "WritableOrdinaryFolder",
		.PhysicalPath = Folder.generic_string()};
	FContentBrowserItem FolderResultItem;
	const bool FolderResult =
		Operations.RenameItem(FolderItem, "RenamedFolder", FolderResultItem);
	EXPECT_FALSE(FolderResult);

	EXPECT_FALSE(OperationError.empty());
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
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);

	FContentBrowserItem CreateResultItem;
	const bool CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string(), "New Folder", CreateResultItem);
	ASSERT_TRUE(CreateResult) << OperationError;
	EXPECT_TRUE(std::filesystem::is_directory(TestPhysicalPath(CreateResultItem)));

	const FContentBrowserItem FileItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "notes.txt",
		.PhysicalPath = File.generic_string(),
		.Extension = ".txt"};
	FContentBrowserItem FileResultItem;
	const bool FileResult =
		Operations.RenameItem(FileItem, "renamed.txt", FileResultItem);
	ASSERT_TRUE(FileResult) << OperationError;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/renamed.txt"));

	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "WritableOrdinaryFolder",
		.PhysicalPath = Folder.generic_string()};
	FContentBrowserItem FolderResultItem;
	const bool FolderResult =
		Operations.RenameItem(FolderItem, "RenamedFolder", FolderResultItem);
	ASSERT_TRUE(FolderResult) << OperationError;
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/RenamedFolder"));
}

TEST_F(FContentBrowserModelTests, AssetAndMixedMovesShareRenameIdentityAndNotifications)
{
	InitializeDObjectSystem();
	FPackagePath Source;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/SharedMove", Source));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Source, Material));
	Material->Rename("AssetObject");
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto Notes = Root / "Content/A/notes.txt";
	std::ofstream(Notes) << "notes";
	// The mixed payload supplies file paths, while a single-asset payload supplies identity.
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = TestPhysicalPath(*(FindPackageExact(Source)))},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = Notes.generic_string()}};
	int Publications = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); ++Publications; }});
	Observe(Service);
	ASSERT_TRUE(Service.CanMoveItem(Items.front(), (Root / "Content/B").generic_string()));
	ASSERT_TRUE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string())) << OperationError;
	EXPECT_EQ(Publications, 1);
	EXPECT_EQ(ObservedChanges.Changes.size(), 2u);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/notes.txt"));
	FPackagePath Moved;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/AssetObject", Moved));
	ASSERT_NE(FindPackageExact(Moved), nullptr);
	const FContentBrowserItem Asset{.Kind = EContentBrowserItemKind::Asset,
		.VirtualPath = "/ContentBrowserTests/B/AssetObject.AssetObject", .PackagePath = Moved};
	ASSERT_TRUE(Service.CanRenameItem(Asset, "RenamedMove"));
	FContentBrowserItem Renamed;
	ASSERT_TRUE(Service.RenameItem(Asset, "RenamedMove", Renamed)) << OperationError;
	EXPECT_EQ(TestVirtualPath(Renamed), "/ContentBrowserTests/B/RenamedMove.RenamedMove");
	EXPECT_EQ(TestName(Renamed), "RenamedMove");
	EXPECT_EQ(TestPhysicalPath(Renamed), (Root / "Content/B/RenamedMove.dasset").generic_string());
	EXPECT_EQ(Publications, 2);
	ASSERT_EQ(ObservedChanges.Changes.size(), 1u);
	EXPECT_EQ(ObservedChanges.Changes.front().NewAssetPath, TestVirtualPath(Renamed));
	ASSERT_TRUE(Service.BulkMoveItems(std::span{&Renamed, 1}, (Root / "Content").generic_string())) << OperationError;
	EXPECT_EQ(Publications, 3);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/RenamedMove.dasset"));
}

TEST_F(FContentBrowserModelTests, AssetMoveRetainsCrossMountSupportWithoutMovingOrdinaryFilesAcrossMounts)
{
	Registry.reset();
	const auto OtherRoot = Root / "OtherContent";
	std::filesystem::create_directories(OtherRoot);
	const std::array Definitions{
		FMountPoint{.VirtualRoot = "/ContentBrowserTests/", .Owner = EMountOwner::Test,
			.Root = Root / "Content", .bAutoScan = true, .bContentWritable = true},
		FMountPoint{.VirtualRoot = "/ContentBrowserOther/", .Owner = EMountOwner::Test,
			.Root = OtherRoot, .bAutoScan = true, .bContentWritable = true}};
	Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(Definitions);
	ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
	InitializeDObjectSystem();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Source, Destination;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/CrossMount", Source));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserOther/CrossMount", Destination));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Source, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FProviderTransferFixture Service;
	Observe(Service);
	const FContentBrowserItem Asset{.Kind = EContentBrowserItemKind::Asset, .PackagePath = Source};
	ASSERT_TRUE(Service.CanMoveItem(Asset, OtherRoot.generic_string()));
	ASSERT_TRUE(Service.BulkMoveItems(std::span{&Asset, 1}, OtherRoot.generic_string())) << OperationError;
	ASSERT_NE(FindPackageExact(Destination), nullptr);
	EXPECT_TRUE(std::filesystem::exists(OtherRoot / "CrossMount.dasset"));
	const auto File = Root / "Content/notes.txt";
	std::ofstream(File) << "notes";
	const FContentBrowserItem Ordinary{.Kind = EContentBrowserItemKind::File, .PhysicalPath = File.generic_string()};
	EXPECT_FALSE(Service.CanMoveItem(Ordinary, OtherRoot.generic_string()));
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Ordinary, 1}, OtherRoot.generic_string()));
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_FALSE(std::filesystem::exists(OtherRoot / "notes.txt"));
}

TEST_F(FContentBrowserModelTests, RenameRechecksDestinationAndPreservesOrdinaryExtension)
{
	const auto Source = Root / "Content/A/source.txt";
	const auto Target = Root / "Content/A/renamed.txt";
	std::ofstream(Source) << "source";
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File, .PhysicalPath = Source.generic_string()};
	int Publications = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch) { ++Publications; }});
	Observe(Service);
	ASSERT_TRUE(Service.CanRenameItem(Item, "renamed"));
	std::ofstream(Target) << "collision";
	EXPECT_FALSE(Service.CanRenameItem(Item, "renamed"));
	FContentBrowserItem Renamed;
	EXPECT_FALSE(Service.RenameItem(Item, "renamed", Renamed));
	EXPECT_EQ(Publications, 0);
	EXPECT_TRUE(std::filesystem::exists(Source));
	ASSERT_TRUE(std::filesystem::remove(Target));
	ASSERT_TRUE(Service.RenameItem(Item, "renamed", Renamed));
	EXPECT_EQ(TestPhysicalPath(Renamed), Target.generic_string());
	EXPECT_EQ(TestName(Renamed), "renamed");
	EXPECT_EQ(TestExtension(Renamed), ".txt");
	EXPECT_EQ(Publications, 1);
}

TEST_F(FContentBrowserModelTests, RejectedMixedMoveDoesNotPublishPlannedChanges)
{
	const auto First = Root / "Content/First";
	const auto Second = Root / "Content/Second";
	std::filesystem::create_directory(First);
	std::filesystem::create_directory(Second);
	std::ofstream(First / "notes.txt") << "notes";
	std::ofstream(Second / "unknown.dasset") << "unregistered";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::Folder, .PhysicalPath = First.generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::Folder, .PhysicalPath = Second.generic_string()}};
	int Publications = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch) { ++Publications; }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_EQ(Publications, 0);
	EXPECT_TRUE(std::filesystem::exists(First / "notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Second / "unknown.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/First"));
}

TEST_F(FContentBrowserModelTests, OperationsRenameDirtyAssetsAndUseRecursiveDeletionPreflight)
{
	InitializeDObjectSystem();
	FPackagePath SourcePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Old", SourcePath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	Material->GetPackage()->MarkDirty();
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
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
	FContentBrowserItem MoveResultItem;
	const bool MoveResult =
		Operations.RenameItem(AssetItem, "New", MoveResultItem);
	EXPECT_TRUE(MoveResult) << OperationError;
	EXPECT_TRUE(OperationError.empty());
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(AssetItem)));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/New.dasset"));
	EXPECT_EQ(Material->GetObjectPath(), "/ContentBrowserTests/New.New");

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
	FDeletionRequestFixture Deletion;
	const FFileDeletionPlanPtr Plan =
		Deletion.PrepareDeleteItems(Items);
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[&](const FFileDeletionFingerprint& Entry) {
			return TestKind(Entry) == EFileDeletionEntryKind::OrdinaryFile
				&& TestPhysicalPath(Entry)
					== std::filesystem::absolute(Folder / "child.txt")
						.lexically_normal()
						.generic_string();
		}));
	EXPECT_TRUE(std::filesystem::exists(Folder));
	EXPECT_TRUE(std::filesystem::exists(Folder / "child.txt"));

}

TEST_F(FContentBrowserModelTests, AssetRenameReturnsTheNewPackageAndObjectIdentity)
{
	FPackagePath OldPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/OldPackage", OldPath));
	InitializeDObjectSystem();
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(OldPath, Material));
	Material->Rename("AssetObject");
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const FContentBrowserItem AssetItem{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "OldPackage",
		.VirtualPath = "/ContentBrowserTests/OldPackage.AssetObject",
		.PackagePath = OldPath,
		.PhysicalPath = (Root / "Content/OldPackage.dasset").generic_string()};
	FProviderTransferFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);

	FContentBrowserItem ResultItem;
	const bool Result =
		Operations.RenameItem(AssetItem, "NewPackage", ResultItem);

	ASSERT_TRUE(Result) << OperationError;
	EXPECT_EQ(TestVirtualPath(ResultItem),
		"/ContentBrowserTests/NewPackage.NewPackage");
}

TEST_F(FContentBrowserModelTests, RefreshesSnapshotAfterFolderMutation)
{
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.RequestDirectoryChildrenSnapshot((Root / "Content").generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto BeforeSnapshot = Model.GetDirectorySnapshot((Root / "Content").generic_string());
	ASSERT_TRUE(BeforeSnapshot);
	const size_t ChildrenBefore = BeforeSnapshot->Children.size();
	FProviderFolderFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);

	FContentBrowserItem CreateResultItem;
	const bool CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string(), "New Folder", CreateResultItem);
	ASSERT_TRUE(CreateResult) << OperationError;
	Model.RefreshItemsSnapshot();
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(
		(Root / "Content").generic_string()));
	Model.RequestDirectoryChildrenSnapshot((Root / "Content").generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.GetDirectorySnapshot((Root / "Content").generic_string()));
	EXPECT_EQ(
		Model.GetDirectorySnapshot((Root / "Content").generic_string())->Children.size(),
		ChildrenBefore + 1);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[&](const auto& Item) {
			return TestPhysicalPath(Item) == TestPhysicalPath(CreateResultItem);
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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem RootItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Tree",
		.PhysicalPath = Tree.generic_string()};
	const FContentBrowserItem NestedItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Nested",
		.PhysicalPath = Nested.generic_string()};
	const std::array Items{RootItem, NestedItem};

	const FFileDeletionPlanPtr Plan =
		Operations.PrepareDeleteItems(Items);
	ASSERT_NE(Plan, nullptr);
	EXPECT_TRUE(Plan->CanExecute());
	ASSERT_EQ(Plan->MaximalRoots.size(), 1);
	EXPECT_EQ(Plan->Summary.FolderCount, 3);
	EXPECT_EQ(Plan->Summary.FileCount, 2);
	EXPECT_EQ(Plan->Summary.AssetCount, 0);
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[&](const FFileDeletionFingerprint& Entry) {
			return TestPhysicalPath(Entry).ends_with("/.hidden/secret.txt");
		}));
	EXPECT_TRUE(std::filesystem::exists(Hidden / "secret.txt"));
	{
		std::ofstream HiddenFile(Hidden / "secret.txt", std::ios::app);
		HiddenFile << " changed";
	}
	auto Confirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(Confirmation).IsComplete());
	EXPECT_TRUE(std::filesystem::exists(Hidden / "secret.txt"));
	EXPECT_TRUE(std::filesystem::exists(Tree / "visible.txt"));
	ASSERT_NE(Confirmation, nullptr);
	EXPECT_NE(Confirmation, Plan);
	ASSERT_TRUE(Confirmation->CanExecute());
	ASSERT_EQ(Confirmation->MaximalRoots.size(), 1);
	auto RetiredConfirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(RetiredConfirmation).IsComplete());
	EXPECT_TRUE(std::filesystem::exists(Tree));
	EXPECT_TRUE(Operations.DeleteItems(Confirmation).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(Tree));
}

TEST_F(FContentBrowserModelTests, BlocksUnknownPackagesWithoutOmittingThem)
{
	const std::filesystem::path Folder = Root / "Content/UnknownPackage";
	std::filesystem::create_directories(Folder);
	{
		std::ofstream Package(Folder / "Unregistered.dasset");
		Package << "not a registered package";
	}
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "UnknownPackage",
		.PhysicalPath = Folder.generic_string()};
	const FFileDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&FolderItem, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FFileDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EFileDeletionBlocker::UnknownPackage;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[](const FFileDeletionFingerprint& Entry) {
			return TestKind(Entry) == EFileDeletionEntryKind::UnknownPackage;
		}));
}

TEST(FFileDeletionAnalysisTests, RejectsReadOnlyMountBeforeMutation)
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
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations;
	const FContentBrowserItem Folder{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Folder",
		.PhysicalPath = (Root / "Content/Folder").generic_string()};
	const FFileDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&Folder, 1});
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FFileDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EFileDeletionBlocker::ReadOnlyMount;
		}));
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/Folder"));

	const FContentBrowserItem MountRoot{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Content",
		.PhysicalPath = (Root / "Content").generic_string()};
	const FFileDeletionPlanPtr MountRootPlan = Operations.PrepareDeleteItems(
		std::span{&MountRoot, 1});
	EXPECT_TRUE(std::ranges::any_of(
		MountRootPlan->Blockers,
		[](const FFileDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EFileDeletionBlocker::MountRoot;
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
	EXPECT_FALSE(PrepareAssetDeletionWithEditorPolicy(
		{.AssetPaths = {PartialPaths.begin(), PartialPaths.end()}}, Job));
	EXPECT_TRUE(std::ranges::any_of(
		Job.GetBlockers(),
		[&](const FAssetDeletionBlocker& Blocker) {
			return Blocker.AssetPath == BasePath
				&& Blocker.RelatedAssetPath == ExternalPath;
		}));

	const std::array CompletePaths{BasePath, InternalPath, ExternalPath};
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy(
		{.AssetPaths = {CompletePaths.begin(), CompletePaths.end()}}, Job));
	EXPECT_TRUE(Job.GetBlockers().empty());
	EXPECT_EQ(Job.GetEntries().size(), 3);

	// Unloading runs GC and can also collect the other unreferenced saved packages.
	if (FindResidentPackage(ExternalPath)) ASSERT_TRUE(UnloadPackage(ExternalPath));
	ASSERT_EQ(FindResidentPackage(ExternalPath), nullptr);
	if (FindResidentPackage(InternalPath)) ASSERT_TRUE(UnloadPackage(InternalPath));
	ASSERT_EQ(FindResidentPackage(InternalPath), nullptr);
	if (FindResidentPackage(BasePath)) ASSERT_TRUE(UnloadPackage(BasePath));
	ASSERT_EQ(FindResidentPackage(BasePath), nullptr);
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
		[SharedCompanion](const FPackageData& Data,
			const FAssetPackageInspection&,
			FAssetDeleteContribution& Contribution, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			if (TestPackagePath(Data).GetView().starts_with(
					"/ContentBrowserTests/Companion"))
				Contribution.Files.push_back(SharedCompanion);
			return true;
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
	EXPECT_FALSE(PrepareAssetDeletionWithEditorPolicy(
		{.AssetPaths = {Paths.begin(), Paths.end()}}, Job));
	EXPECT_TRUE(std::ranges::any_of(
		Job.GetBlockers(),
		[](const FAssetDeletionBlocker& Blocker) {
			return TestKind(Blocker)
				== EAssetDeletionBlocker::CompanionOwnershipConflict;
		}));

	// Unloading runs GC and can also collect the other unreferenced saved packages.
	if (FindResidentPackage(SecondPath)) ASSERT_TRUE(UnloadPackage(SecondPath));
	ASSERT_EQ(FindResidentPackage(SecondPath), nullptr);
	if (FindResidentPackage(FirstPath)) ASSERT_TRUE(UnloadPackage(FirstPath));
	ASSERT_EQ(FindResidentPackage(FirstPath), nullptr);
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
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {BasePath}}, Job));
	ASSERT_TRUE(Job.GetBlockers().empty());

	DMaterialInstance* External = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(ExternalPath, External));
	ASSERT_TRUE(External->SetParent(Base));
	ASSERT_TRUE(SavePackage(External->GetPackage()));
	const auto Commit = Job.Delete({
		.Delete = [](auto&) { return FAssetWriteResult{}; },
	});
	EXPECT_FALSE(Commit);
	EXPECT_FALSE(Job.GetWriteResult());

	// Unloading runs GC and can also collect the other unreferenced saved packages.
	if (FindResidentPackage(ExternalPath)) ASSERT_TRUE(UnloadPackage(ExternalPath));
	ASSERT_EQ(FindResidentPackage(ExternalPath), nullptr);
	if (FindResidentPackage(BasePath)) ASSERT_TRUE(UnloadPackage(BasePath));
	ASSERT_EQ(FindResidentPackage(BasePath), nullptr);
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
	const std::filesystem::path File = TestPhysicalPath(*(FindPackageExact(Path)));

	uint32 Calls = 0;
	const FAssetDeletionCommit Commit{
		.Delete = [&](auto&) -> FAssetWriteResult {
			++Calls;
			std::error_code Ec;
			std::filesystem::remove(File, Ec);
			return Ec ? FAssetWriteResult{.Error = Durin::UnifiedError::FError{AssetWriteErrors::IoError, Ec.message()}} : FAssetWriteResult{};
		}};
	FAssetDeletionOperation Original;
	EXPECT_FALSE(Original.Delete(Commit));
	EXPECT_EQ(Calls, 0);
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Original));
	FAssetDeletionOperation Owner = std::move(Original);
	EXPECT_FALSE(Original.Delete(Commit));
	EXPECT_EQ(Calls, 0);
	ASSERT_TRUE(Owner.Delete(Commit));
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	EXPECT_EQ(FindPackageExact(Path), nullptr);
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
	const std::filesystem::path File = TestPhysicalPath(*(FindPackageExact(Path)));
	{
		std::ofstream Corrupt(File, std::ios::binary | std::ios::trunc);
		Corrupt << "invalid package";
	}
	FAssetDeletionOperation Operation;
	EXPECT_FALSE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	EXPECT_TRUE(std::ranges::any_of(Operation.GetBlockers(), [](const auto& Blocker) {
		return TestKind(Blocker) == EAssetDeletionBlocker::CompanionInspectionFailed;
	}));
	bool bCalled = false;
	EXPECT_FALSE(Operation.Delete({.Delete = [&](auto&) -> FAssetWriteResult {
		bCalled = true;
		return {};
	}}));
	EXPECT_FALSE(bCalled);
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_NE(FindPackageExact(Path), nullptr);
	// Later ownership queries also inspect catalog entries from earlier fixtures.
	ASSERT_TRUE(std::filesystem::remove(File));
}

TEST_F(FContentBrowserModelTests, StandardCompanionOwnershipUsesMetadataWithoutValidatingPayload)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/MetadataCompanion", Path));
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
	Image::FImage Image;
	auto ImageResult1 = Image::FImage::TryCreate({.Width = 512, .Height = 512,
		.Format = Image::ERawImageFormat::RGBA8, .GammaSpace = Image::EImageGammaSpace::SRGB}, FByteBuffer(512 * 512 * 4, std::byte{71}));
	ASSERT_TRUE(ImageResult1);
	Image = std::move(*ImageResult1);
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 4, 0, ETextureSourceCompression::Raw));
	Texture->SetSource(Source);
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
	const auto Data = FindPackageExact(Path);
	ASSERT_NE(Data, nullptr);
	ASSERT_GT(Data->BulkSegmentExtent, 0u);
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto Item = std::ranges::find_if(Model.GetItems(), [&](const auto& Entry) {
		return TestPackagePath(Entry) == Path;
	});
	ASSERT_NE(Item, Model.GetItems().end());
	EXPECT_EQ(TestPackageFileSize(*(Item)), TestFileSize(*(Data)));
	EXPECT_EQ(TestBulkFileSize(*(Item)), Data->BulkSegmentExtent);
	EXPECT_EQ(TestFileSize(*(Item)), TestFileSize(*(Data)) + Data->BulkSegmentExtent);
	std::filesystem::path BulkPath = TestPhysicalPath(*(Data));
	BulkPath.replace_extension(".dbulk");
	{
		// Same-size payload damage changes integrity, but not package ownership.
		std::fstream Bulk(BulkPath, std::ios::binary | std::ios::in | std::ios::out);
		ASSERT_TRUE(Bulk.is_open());
		char Byte = 0;
		Bulk.read(&Byte, 1);
		ASSERT_TRUE(Bulk.good());
		Byte ^= 1;
		Bulk.seekp(0);
		Bulk.write(&Byte, 1);
		ASSERT_TRUE(Bulk.good());
	}
	FAssetPackageInspection Inspection;
	EXPECT_FALSE(InspectAssetPackage(TestPhysicalPath(*(Data)), Path, Inspection));
	FAssetCompanionOwnership Ownership;
	const auto OwnershipResult = QueryAssetCompanionOwnership(BulkPath, Ownership);
	ASSERT_TRUE(OwnershipResult);
	EXPECT_EQ(Ownership.State, EAssetCompanionOwnershipState::Owned);
	ASSERT_EQ(Ownership.Owners.size(), 1u);
	EXPECT_EQ(Ownership.Owners.front(), Path);
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	ASSERT_EQ(Operation.GetEntries().size(), 1u);
	ASSERT_EQ(Operation.GetEntries().front().CompanionFiles.size(), 1u);
	EXPECT_EQ(Operation.GetEntries().front().CompanionFiles.front(), BulkPath);

	// Fresh metadata inspection must still reject a missing/truncated segment,
	// even though the catalog has not been refreshed.
	std::filesystem::resize_file(BulkPath, Data->BulkSegmentExtent - 1);
	EXPECT_FALSE(QueryAssetCompanionOwnership(BulkPath, Ownership));
	bool bCalled = false;
	EXPECT_FALSE(Operation.Delete({.Delete = [&](auto&) -> FAssetWriteResult {
		bCalled = true;
		return {};
	}}));
	EXPECT_FALSE(bCalled);
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(*(Data))));
	ASSERT_TRUE(std::filesystem::remove(TestPhysicalPath(*(Data))));
	ASSERT_TRUE(std::filesystem::remove(BulkPath));
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
	// Unloading runs GC and can also collect the other unreferenced saved packages.
	if (FindResidentPackage(OwnerPath)) ASSERT_TRUE(UnloadPackage(OwnerPath));
	ASSERT_EQ(FindResidentPackage(OwnerPath), nullptr);
	if (FindResidentPackage(BasePath)) ASSERT_TRUE(UnloadPackage(BasePath));
	ASSERT_EQ(FindResidentPackage(BasePath), nullptr);
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
		[&](const FPackageData& Data, const FAssetPackageInspection& Inspection,
			FAssetDeleteContribution& Contribution, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			if (TestPackagePath(Data) == OwnerPath)
			{
				EXPECT_NE(Inspection.FindField("Parent"), nullptr);
				Contribution.Files.push_back(Companion);
			}
			return true;
		})};
	ASSERT_NE(Reset.Handle, 0);
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {OwnerPath}}, Operation));
	ASSERT_EQ(Operation.GetEntries().size(), 1);
	EXPECT_EQ(Operation.GetEntries().front().CompanionFiles, std::vector{Companion});
	EXPECT_EQ(FindResidentPackage(BasePath), nullptr);
	EXPECT_EQ(FindResidentPackage(OwnerPath), nullptr);
	const auto PackageFile = TestPhysicalPath(Operation.GetEntries().front().RegistryEntry);
	ASSERT_TRUE(Operation.Delete({.Delete = [&](auto&) -> FAssetWriteResult {
		std::error_code Ec;
		std::filesystem::remove(PackageFile, Ec);
		if (Ec) return {.Error = Durin::UnifiedError::FError{AssetWriteErrors::IoError, Ec.message()}};
		std::filesystem::remove(Companion, Ec);
		return Ec ? FAssetWriteResult{.Error = Durin::UnifiedError::FError{AssetWriteErrors::IoError, Ec.message()}} : FAssetWriteResult{};
	}}));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	EXPECT_NE(FindPackageExact(BasePath), nullptr);
}

TEST_F(FContentBrowserModelTests, DeletionReusesConfirmationAndInspectsParticipantsOncePerPhase)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/SingleDeletionPlan", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto File = TestPhysicalPath(*(FindPackageExact(Path)));
	uint32 Inspections = 0;
	struct FContributorReset
	{
		FAssetDeleteContributorHandle Handle;
		~FContributorReset() { UnregisterAssetDeleteContributor(Handle); }
	} Reset{RegisterAssetDeleteContributor(DMaterial::StaticClass(),
		[&](const FPackageData& Data, const FAssetPackageInspection&,
			FAssetDeleteContribution&, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			if (TestPackagePath(Data) == Path) ++Inspections;
			return true;
		})};
	ASSERT_NE(Reset.Handle, 0u);
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File,
		.Name = "SingleDeletionPlan", .PhysicalPath = File};
	FDeletionRequestFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Inspections, 1u);
	ASSERT_TRUE(Service.DeleteItems(Plan).IsComplete());
	EXPECT_EQ(Inspections, 2u);
	EXPECT_FALSE(std::filesystem::exists(File));
}

TEST_F(FContentBrowserModelTests, DeletionRejectsChangedContributorRegistration)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ContributorRegistration", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	const auto Handle = RegisterAssetDeleteContributor(DMaterial::StaticClass(),
		[](const FPackageData&, const FAssetPackageInspection&, FAssetDeleteContribution&, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			return true;
		});
	ASSERT_NE(Handle, 0u);
	UnregisterAssetDeleteContributor(Handle);
	bool bCalled = false;
	EXPECT_FALSE(Operation.Delete({.Delete = [&](auto&) -> FAssetWriteResult { bCalled = true; return {}; }}));
	EXPECT_FALSE(bCalled);
	ASSERT_TRUE(UnloadPackage(Path));
	ASSERT_TRUE(Testing::RemoveAssetPackageForTests(Path));
}

TEST_F(FContentBrowserModelTests, UnrelatedStandardPackageDamageDoesNotInvalidateDeletion)
{
	InitializeDObjectSystem();
	FPackagePath SelectedPath, OutsidePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ScopedDelete", SelectedPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/UnrelatedDelete", OutsidePath));
	DMaterial* Selected = nullptr;
	DMaterial* Outside = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SelectedPath, Selected));
	ASSERT_TRUE(SavePackage(Selected->GetPackage()));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(OutsidePath, Outside));
	ASSERT_TRUE(SavePackage(Outside->GetPackage()));
	ASSERT_TRUE(UnloadPackage(OutsidePath));
	const auto SelectedFile = TestPhysicalPath(*(FindPackageExact(SelectedPath)));
	const auto OutsideFile = TestPhysicalPath(*(FindPackageExact(OutsidePath)));
	FDeletionRequestFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File,
		.Name = "ScopedDelete", .PhysicalPath = SelectedFile};
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	std::ofstream(OutsideFile, std::ios::binary | std::ios::trunc) << "unrelated corrupt package";
	FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(QueryAssetCompanionOwnership(Root / "Content/unclaimed.txt", Ownership));
	EXPECT_EQ(Ownership.State, EAssetCompanionOwnershipState::Unclaimed);
	ASSERT_TRUE(Service.DeleteItems(Plan).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(SelectedFile));
	EXPECT_TRUE(std::filesystem::exists(OutsideFile));
	ASSERT_TRUE(std::filesystem::remove(OutsideFile));
}

TEST_F(FContentBrowserModelTests, DeletionRejectsOpenLevelAndAllowsDeletionAfterClose)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/OpenLevelDeletion", Path));
	DLevel* Level = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Level));
	ASSERT_TRUE(SavePackage(Level->GetPackage()));
	const std::filesystem::path File = TestPhysicalPath(*(FindPackageExact(Path)));
	TStrongObjectPtr<DWorld> World(NewObject<DWorld>(nullptr, "DeletionWorld"));
	ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	FAssetDeletionOperation Operation;
	EXPECT_FALSE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	EXPECT_TRUE(std::ranges::any_of(Operation.GetBlockers(), [&](const auto& Blocker) {
		return TestKind(Blocker) == EAssetDeletionBlocker::ExternalLoadedReference
			&& Blocker.AssetPath == Path && Blocker.Details.find("Close the level") != std::string::npos;
	}));
	EXPECT_EQ(World->GetCurrentLevel(), Level);
	EXPECT_FALSE(Level->IsGarbage());
	EXPECT_TRUE(std::filesystem::exists(File));
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->CleanupWorld();
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	ASSERT_TRUE(Operation.Delete({.Delete = [&](auto& Removed) -> FAssetWriteResult {
		std::error_code Error;
		if (!std::filesystem::remove(File, Error)) return {.Error = Durin::UnifiedError::FError{AssetWriteErrors::IoError, Error.message()}};
		Removed.push_back(File);
		return {};
	}}));
	EXPECT_FALSE(std::filesystem::exists(File));
	EXPECT_EQ(FindPackageExact(Path), nullptr);
}

TEST_F(FContentBrowserModelTests, DeletionRechecksLevelOpenedAfterConfirmationAndDuringFileValidation)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/LateOpenLevelDeletion", Path));
	DLevel* Level = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Level));
	ASSERT_TRUE(SavePackage(Level->GetPackage()));
	DPackage* Package = Level->GetPackage();
	const std::filesystem::path File = TestPhysicalPath(*(FindPackageExact(Path)));
	TStrongObjectPtr<DWorld> World(NewObject<DWorld>(nullptr, "LateDeletionWorld"));
	ASSERT_NO_THROW(World->InitializeSubsystems());
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	{ std::optional<UnifiedError::FError> Error; EXPECT_FALSE(Operation.Validate(&Error)); EXPECT_TRUE(Error && Error->Is(AssetWriteErrors::InUse)); }
	uint32 Deletes = 0;
	const auto Delete = [&](auto&) -> FAssetWriteResult { ++Deletes; return {}; };
	EXPECT_FALSE(Operation.Delete({.Delete = Delete}));
	EXPECT_EQ(Deletes, 0u);
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	EXPECT_TRUE(Operation.Validate());
	EXPECT_FALSE(Operation.Delete({
		.Delete = Delete,
		.ValidateFiles = [&](FAssetDeletionFileIdentities& Identities, std::optional<UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			if (!World->SetCurrentLevel(Level)) { if (OutError) *OutError = Durin::UnifiedError::FError{AssetWriteErrors::InUse, "Could not open level."}; return false; }
			const auto Identity = FFileHelper::HashFileXx128(File);
			if (!Identity) { if (OutError) *OutError = Durin::UnifiedError::FError{AssetWriteErrors::IoError, Identity.error().ToString()}; return false; }
			Identities.emplace(File.generic_string(), *Identity);
			return true;
		}}));
	EXPECT_EQ(Deletes, 0u);
	EXPECT_EQ(FindResidentPackage(Path), Package);
	EXPECT_EQ(World->GetCurrentLevel(), Level);
	EXPECT_FALSE(Level->IsGarbage());
	EXPECT_TRUE(std::filesystem::exists(File));
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->CleanupWorld();
}

TEST_F(FContentBrowserModelTests, DeletionSharesFreshHostHashesAndRequiresCompleteEvidence)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/SharedDeletionHash", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
	const std::filesystem::path File = TestPhysicalPath(*(FindPackageExact(Path)));
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	uint32 Deletes = 0;
	uint32 Hashes = 0;
	bool bOmitIdentity = true;
#ifdef _WIN32
	struct FReadGuard
	{
		HANDLE Handle = INVALID_HANDLE_VALUE;
		auto Reset() -> void { if (Handle != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(Handle, INVALID_HANDLE_VALUE)); }
		~FReadGuard() { Reset(); }
	} Guard;
#endif
	const FAssetDeletionCommit Commit{
		.Delete = [&](auto&) -> FAssetWriteResult {
#ifdef _WIN32
			Guard.Reset();
#endif
			++Deletes;
			std::error_code Error;
			std::filesystem::remove(File, Error);
			return Error ? FAssetWriteResult{.Error = Durin::UnifiedError::FError{AssetWriteErrors::IoError, Error.message()}} : FAssetWriteResult{};
		},
		.ValidateFiles = [&](FAssetDeletionFileIdentities& Identities, std::optional<UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			if (bOmitIdentity) return true;
			auto Identity = FFileHelper::HashFileXx128(File);
			if (!Identity)
				{ if (OutError) *OutError = Durin::UnifiedError::FError{AssetWriteErrors::IoError, Identity.error().ToString()}; return false; }
			++Hashes;
			Identities.emplace(File.generic_string(), *Identity);
#ifdef _WIN32
			// A duplicate AssetTools payload read would fail until the physical callback.
			Guard.Handle = CreateFileW(File.c_str(), GENERIC_READ, FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (Guard.Handle == INVALID_HANDLE_VALUE)
				{ if (OutError) *OutError = Durin::UnifiedError::FError{AssetWriteErrors::IoError, "Could not guard the verified file against duplicate reads."}; return false; }
#endif
			return true;
		}};
	EXPECT_FALSE(Operation.Delete(Commit));
	EXPECT_EQ(Deletes, 0u);
	bOmitIdentity = false;
	ASSERT_TRUE(Operation.Delete(Commit));
	EXPECT_EQ(Deletes, 1u);
	EXPECT_EQ(Hashes, 1u);
	EXPECT_FALSE(std::filesystem::exists(File));
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
		auto CaptureSnapshot(FAssetReferenceStoreSnapshot& Snapshot) -> std::expected<void, Durin::UnifiedError::FError> override
		{
			if (bFail) return std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::IoError, "Injected provider inspection failure."});
			Snapshot = {.ProviderId = "deletion-test", .Fingerprint = Fingerprint};
			return {};
		}
		auto PrepareRewrite(std::span<const FAssetReferenceRewrite>, std::string_view,
			FAssetReferenceStoreRewriteContribution&) -> std::expected<void, UnifiedError::FError> override
		{
			return std::unexpected(Durin::UnifiedError::FError{AssetWriteErrors::InUse, "Deletion must not rewrite references."});
		}
	} Store;
	struct FStoreReset
	{
		FAssetReferenceStoreHandle Handle;
		~FStoreReset() { UnregisterAssetReferenceStore(Handle); }
	} Reset{RegisterAssetReferenceStore(&Store)};
	ASSERT_NE(Reset.Handle, 0);
	FAssetDeletionOperation Operation;
	EXPECT_FALSE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	EXPECT_TRUE(std::ranges::any_of(Operation.GetBlockers(), [](const auto& Blocker) {
		return TestKind(Blocker) == EAssetDeletionBlocker::ReferenceStoreInspectionFailed;
	}));
	Store.bFail = false;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	Store.Fingerprint = "second";
	bool bCalled = false;
	EXPECT_FALSE(Operation.Delete({.Delete = [&](auto&) -> FAssetWriteResult {
		bCalled = true;
		return {};
	}}));
	EXPECT_FALSE(bCalled);
	EXPECT_TRUE(Operation.GetWriteResult().Error && Operation.GetWriteResult().Error->Is(AssetWriteErrors::StaleData));
	EXPECT_NE(FindResidentPackage(Path), nullptr);
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(*(FindPackageExact(Path)))));
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "ProviderDeletion", .PhysicalPath = TestPhysicalPath(*(FindPackageExact(Path)))};
	FDeletionRequestFixture Service;
	Observe(Service);
	auto Confirmation = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Confirmation->CanExecute());
	const auto Original = Confirmation;
	Store.bFail = true;
	EXPECT_FALSE(Service.DeleteItems(Confirmation).IsComplete());
	EXPECT_EQ(Confirmation, Original);
	EXPECT_NE(OperationError.find("Injected provider inspection failure"), std::string::npos);
	Store.bFail = false;
	Store.Fingerprint = "third";
	EXPECT_FALSE(Service.DeleteItems(Confirmation).IsComplete());
	ASSERT_TRUE(Confirmation);
	EXPECT_NE(Confirmation, Original);
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(Item)));
	EXPECT_TRUE(Service.DeleteItems(Confirmation).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(Item)));

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

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ReparseRoot",
		.PhysicalPath = Folder.generic_string()};
	const FFileDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&FolderItem, 1});
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FFileDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EFileDeletionBlocker::ReparsePoint;
		}));
}

TEST_F(FContentBrowserModelTests, EmptyFolderDeletionIsPermanent)
{
	const std::filesystem::path Folder = Root / "Content/EmptyFolder";
	std::filesystem::create_directories(Folder);
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "EmptyFolder",
		.PhysicalPath = Folder.generic_string()};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Plan->Summary.FolderCount, 1);
	EXPECT_EQ(Plan->Summary.AssetCount, 0);
	EXPECT_EQ(Plan->Summary.FileCount, 0);

	ASSERT_TRUE(Operations.DeleteItems(Plan).IsComplete());
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
			[AssetPath, OutsideFile](const FPackageData& Data,
				const FAssetPackageInspection&,
				FAssetDeleteContribution& Contribution, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
				if (TestPackagePath(Data) == AssetPath)
					Contribution.Files.push_back(OutsideFile);
				return true;
			});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "UnsafeCompanion",
		.PhysicalPath = Folder.generic_string()};
	const FFileDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&Item, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[&](const FFileDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EFileDeletionBlocker::OutsideMount
				&& TestPhysicalPath(Blocker)
					== std::filesystem::absolute(OutsideFile)
						.lexically_normal().generic_string();
		}));
	EXPECT_FALSE(std::ranges::any_of(
		Plan->MaximalRoots,
		[&](const FFileDeletionRoot& RootEntry) {
			return RootEntry.OriginalPath
				== std::filesystem::absolute(OutsideFile)
					.lexically_normal().generic_string();
		}));
	EXPECT_TRUE(std::filesystem::is_regular_file(OutsideFile));

	Operations.DismissDeletion(Plan);
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
			[AssetPath, Companion](const FPackageData& Data,
				const FAssetPackageInspection&,
				FAssetDeleteContribution& Contribution, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
				if (TestPackagePath(Data) == AssetPath)
					Contribution.Files.push_back(Companion);
				return true;
			});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ReparseCompanion",
		.PhysicalPath = Folder.generic_string()};
	const FFileDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&Item, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FFileDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EFileDeletionBlocker::ReparsePoint;
		}));
	EXPECT_TRUE(std::filesystem::is_regular_file(Target));

	Operations.DismissDeletion(Plan);
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
		[AssetPath, Companion](const FPackageData& Data,
			const FAssetPackageInspection&,
			FAssetDeleteContribution& Contribution, std::optional<Durin::UnifiedError::FError>* OutError) -> bool { if (OutError) OutError->reset();
			if (TestPackagePath(Data) == AssetPath)
				Contribution.Files.push_back(Companion);
			return true;
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Mixed",
		.PhysicalPath = Folder.generic_string()};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Plan->Summary.AssetCount, 1);
	EXPECT_EQ(Plan->Summary.FileCount, 1);
	EXPECT_EQ(Plan->Summary.CompanionCount, 1);
	EXPECT_EQ(Plan->Summary.FolderCount, 2);
	ASSERT_EQ(Plan->MaximalRoots.size(), 2);

	ASSERT_TRUE(Operations.DeleteItems(Plan).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(Folder));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	EXPECT_EQ(FindPackageExact(AssetPath), nullptr);
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
		TestPhysicalPath(*(FindPackageExact(AssetPath)));

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "DestructiveMaterial",
		.VirtualPath = AssetPath.ToString(),
		.PhysicalPath = PackagePath.generic_string(),
		.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString()};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());

	ASSERT_TRUE(Operations.DeleteItems(Plan).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(PackagePath));
	EXPECT_EQ(FindPackageExact(AssetPath), nullptr);
	EXPECT_EQ(FindResidentPackage(AssetPath), nullptr);
}

TEST_F(FContentBrowserModelTests, UnusedRedirectorCanBeSelectedAloneAndTargetRequiresIncomingAliases)
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
	ASSERT_TRUE(Testing::RenamePackageLeafAssetForTesting(OldPath, FinalPath));

	FPackageCatalogEntry AliasData =
		FindPackageExact(OldPath);
	FPackageCatalogEntry FinalData =
		FindPackageExact(FinalPath);
	ASSERT_NE(AliasData, nullptr);
	ASSERT_NE(FinalData, nullptr);
	const FContentBrowserItem AliasItem{
		.Kind = EContentBrowserItemKind::Redirector,
		.Name = "DeleteRedirectOld",
		.VirtualPath = Testing::MakePackageLeafTopLevelAssetPathForTests(OldPath).ToString(),
		.PackagePath = OldPath,
		.PhysicalPath = TestPhysicalPath(*(AliasData)),
		.AssetClassName = TestAssetClassName(AliasData->TopLevelAssets.front()),
		.RedirectDestination = TestRedirectDestination(AliasData->TopLevelAssets.front())};
	const FContentBrowserItem FinalItem{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "DeleteRedirectFinal",
		.VirtualPath = Testing::MakePackageLeafTopLevelAssetPathForTests(FinalPath).ToString(),
		.PackagePath = FinalPath,
		.PhysicalPath = TestPhysicalPath(*(FinalData)),
		.AssetClassName = TestAssetClassName(FinalData->TopLevelAssets.front())};

	FProviderQueryFixture ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FFileDeletionPlanPtr AliasOnly = Operations.PrepareDeleteItems(
		std::span{&AliasItem, 1});
	EXPECT_TRUE(AliasOnly->CanExecute());
	const auto TargetOnly = Operations.PrepareDeleteItems(std::span{&FinalItem, 1});
	EXPECT_FALSE(TargetOnly->CanExecute());
	const std::array Items{AliasItem, FinalItem};
	auto Complete = Operations.PrepareDeleteItems(Items);
	ASSERT_TRUE(Complete->CanExecute()) << [&] { std::string Text; for (const auto& Blocker : Complete->Blockers) Text += Blocker.Details + "\n"; return Text; }();
	ASSERT_TRUE(Operations.DeleteItems(Complete).IsComplete());
	EXPECT_EQ(FindPackageExact(OldPath), nullptr);
	EXPECT_EQ(FindPackageExact(FinalPath), nullptr);
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
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ConflictFolder",
		.PhysicalPath = Folder.generic_string()};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	{
		std::ofstream File(SourceFile, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(SourceFile, ConfirmedWriteTime);
	EXPECT_FALSE(Operations.DeleteItems(Plan).IsComplete());
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
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "rewrite.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	const FFileDeletionPlanPtr Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	{
		std::ofstream File(FilePath, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(FilePath, ConfirmedWriteTime);
	auto Confirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(Confirmation).IsComplete());
	EXPECT_TRUE(std::filesystem::exists(FilePath));
	ASSERT_NE(Confirmation, nullptr);
	EXPECT_NE(Confirmation, Plan);
	ASSERT_TRUE(Confirmation->CanExecute());
	auto RetiredConfirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(RetiredConfirmation).IsComplete());
	EXPECT_TRUE(std::filesystem::exists(FilePath));
	EXPECT_TRUE(Operations.DeleteItems(Confirmation).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(FilePath));
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
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "StagedRewrite",
		.PhysicalPath = Folder.generic_string()};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	const std::filesystem::path Unexpected = Folder / "unexpected.txt";
	{
		std::ofstream File(Unexpected);
		File << "new";
	}
	EXPECT_FALSE(Operations.DeleteItems(Plan).IsComplete());
	EXPECT_TRUE(std::filesystem::exists(SourceFile));
	EXPECT_TRUE(std::filesystem::exists(Unexpected));
}

TEST_F(FContentBrowserModelTests, CompletedDeletionLeavesNoRecoveryArtifact)
{
	const std::filesystem::path FilePath = Root / "Content/evicted.txt";
	{
		std::ofstream File(FilePath);
		File << "evict me";
	}
	FDeletionRequestFixture Operations({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "evicted.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Operations.DeleteItems(Plan).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(FilePath));
	EXPECT_FALSE(std::filesystem::exists(Root / "Undo"));
}

TEST_F(FContentBrowserModelTests, DeletionProjectionFailureRetainsStructuredCommittedState)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ProjectionFailure", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto Physical = TestPhysicalPath(*(FindPackageExact(Path)));
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	int Calls = 0;
	const FAssetDeletionCommit Commit{.Delete = [&](auto&) -> FAssetWriteResult {
		++Calls;
		std::filesystem::remove(Physical);
		// Reconciliation between physical removal and publication changes the revision.
		const auto Refresh = RefreshAssetRegistry(EAssetRegistryScanMode::Incremental);
		EXPECT_TRUE(Refresh);
		return {};
	}};
	const auto Result = Operation.Delete(Commit);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Operation.GetWriteResult().Effect, EAssetWriteEffect::ContentCommittedProjectionPending);
	EXPECT_NE(std::ranges::find(Operation.GetAffectedFiles(), std::filesystem::path(Physical)), Operation.GetAffectedFiles().end());
	EXPECT_FALSE(Operation.Delete(Commit));
	EXPECT_EQ(Operation.GetWriteResult().Effect, EAssetWriteEffect::ContentCommittedProjectionPending);
	EXPECT_FALSE(Operation.GetAffectedFiles().empty());
	EXPECT_EQ(Calls, 1);
}

TEST_F(FContentBrowserModelTests, ServiceRechecksPolicyAndPublishesFilesystemChangeOnce)
{
	bool bAllowed = true;
	int Publications = 0;
	FProviderFolderFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }, .CanMutateContent = [&] { return bAllowed; }});
	Observe(Service);
	ASSERT_TRUE(Service.CanModify());
	FContentBrowserItem Prepared;
	ASSERT_TRUE(Service.PrepareFolder((Root / "Content").generic_string(), Prepared));
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(Prepared)));
	EXPECT_EQ(Publications, 0);
	bAllowed = false;
	EXPECT_FALSE(Service.CreateFolder((Root / "Content").generic_string(), "New Folder", NewItem));
	EXPECT_EQ(Publications, 0);
	bAllowed = true;
	FContentBrowserItem ResultItem;
	const auto Result = Service.CreateFolder((Root / "Content").generic_string(), "New Folder", ResultItem);
	ASSERT_TRUE(Result);
	EXPECT_TRUE(std::filesystem::is_directory(TestPhysicalPath(ResultItem)));
	EXPECT_EQ(Publications, 1);
	Service.StopRequestAdmission();
	EXPECT_FALSE(Service.CreateFolder((Root / "Content").generic_string(), "New Folder", NewItem));
	EXPECT_EQ(Publications, 1);
}

TEST_F(FContentBrowserModelTests, PartialBatchSaveRefreshesSuccessfulContentOnce)
{
	FPackagePath Saved, Failed;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Saved", Saved));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Failed", Failed));
	int Publications = 0;
	InitializeDObjectSystem();
	DMaterial* SavedAsset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Saved, SavedAsset));
	SavedAsset->GetPackage()->MarkDirty();
	// The second requested package is absent; the first save must still publish.
	FProviderSaveFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }, .CanMutateContent = {}});
	Observe(Service);
	const auto Result = Service.BulkSaveItems(std::array{AssetItem(Saved), AssetItem(Failed)});
	EXPECT_TRUE(Result);
	EXPECT_TRUE((ObservedChanges.bFullRefresh || !ObservedChanges.Changes.empty()));
	EXPECT_EQ(Publications, 1);
	EXPECT_NE(OperationError.find(Failed.ToString()), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/Saved.dasset"));
	ASSERT_EQ(ObservedChanges.Changes.size(), 1u);
	EXPECT_EQ(ObservedChanges.Changes.front().NewAssetPath, Saved.ToString());
}

TEST_F(FContentBrowserModelTests, DeletionReconfirmsChangedDirtySelectionThenAllowsExplicitLoss)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DirtyConfirmation", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "DirtyConfirmation", .PhysicalPath = TestPhysicalPath(*(FindPackageExact(Path)))};
	FDeletionRequestFixture Service;
	Observe(Service);
	auto Confirmation = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Confirmation->CanExecute());
	const auto Original = Confirmation;
	Material->GetPackage()->MarkDirty();
	EXPECT_FALSE(Service.DeleteItems(Confirmation).IsComplete());
	EXPECT_NE(Confirmation, Original);
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(Item)));
	// A fresh confirmation permits explicit loss of selected unsaved changes.
	ASSERT_TRUE(Confirmation->CanExecute());
	EXPECT_TRUE(Service.DeleteItems(Confirmation).IsComplete());
	EXPECT_FALSE(Confirmation);
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(Item)));
}

#ifdef _WIN32
TEST_F(FContentBrowserModelTests, DeletionReadFailureKeepsConfirmationAndRevalidatesRetry)
{
	const auto File = Root / "Content/locked-confirmation.txt";
	std::ofstream(File) << "before";
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File,
		.Name = "locked-confirmation.txt", .PhysicalPath = File.generic_string()};
	FDeletionRequestFixture Service;
	Observe(Service);
	auto Confirmation = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Confirmation->CanExecute());
	const auto Original = Confirmation;
	{
		struct FReadGuard
		{
			HANDLE Handle;
			~FReadGuard() { if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle); }
		} Guard{CreateFileW(File.c_str(), GENERIC_READ, FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
		ASSERT_NE(Guard.Handle, INVALID_HANDLE_VALUE);
		EXPECT_FALSE(Service.DeleteItems(Confirmation).IsComplete());
		EXPECT_EQ(Confirmation, Original);
		EXPECT_FALSE(OperationError.empty());
		EXPECT_TRUE(OperationWarning.empty());
		EXPECT_TRUE(std::filesystem::exists(File));
	}
	// A retained confirmation is not permission to skip validation on retry.
	std::ofstream(File, std::ios::trunc) << "changed";
	EXPECT_FALSE(Service.DeleteItems(Confirmation).IsComplete());
	ASSERT_TRUE(Confirmation);
	EXPECT_NE(Confirmation, Original);
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_TRUE(Service.DeleteItems(Confirmation).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(File));
}
#endif

TEST_F(FContentBrowserModelTests, AssetDeletionSessionRetiresAfterPartialRemoval)
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
			.PhysicalPath = TestPhysicalPath(*(FindPackageExact(Path)))});
	}
	int Publications = 0;
	FDeletionRequestFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	auto Confirmation = Service.PrepareDeleteItems(Items);
	ASSERT_TRUE(Confirmation->CanExecute());
	int Calls = 0;
	const auto First = Service.DeleteItems(Confirmation, {.RemoveEntry = [&](const auto& Path) {
		if (++Calls == 2) return std::make_error_code(std::errc::io_error);
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Path, Error);
		return Error;
	}});
	EXPECT_FALSE(First.IsComplete());

	EXPECT_FALSE(OperationError.empty());
	EXPECT_EQ(Publications, 1);
	EXPECT_FALSE(Service.DeleteItems(Confirmation).IsComplete());
	EXPECT_EQ(Calls, 2);
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(Items.front())));
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(Items.back())));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
	auto Fresh = Service.PrepareDeleteItems(std::span{&Items.back(), 1});
	ASSERT_TRUE(Fresh->CanExecute());
	ASSERT_TRUE(Service.DeleteItems(Fresh).IsComplete());
	EXPECT_EQ(Publications, 2);
	for (const auto& Item : Items) EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(Item)));
}

TEST_F(FContentBrowserModelTests, StaleConfirmationRequiresNewHandleAndUsesCapturedRequest)
{
	const auto Folder = Root / "Content/confirmation";
	std::filesystem::create_directories(Folder);
	std::ofstream(Folder / "before.txt") << "before";
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Folder,
		.Name = "confirmation", .PhysicalPath = Folder.generic_string()};
	FDeletionRequestFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	auto OriginalPlan = Plan;
	std::ofstream(Folder / "after.txt") << "after";
	const auto Stale = Service.DeleteItems(Plan);
	EXPECT_FALSE(Stale.IsComplete());
	ASSERT_TRUE(Plan);
	EXPECT_NE(Plan, OriginalPlan);
	EXPECT_EQ(Plan->Summary.FileCount, 2u);
	EXPECT_FALSE(Service.DeleteItems(OriginalPlan).IsComplete());
	EXPECT_TRUE(std::filesystem::exists(Folder));
	Service.DismissDeletion(Plan);
	EXPECT_FALSE(Service.DeleteItems(Plan).IsComplete());
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
		.Name = "ServiceProjection", .PhysicalPath = TestPhysicalPath(*(FindPackageExact(Path)))};
	int Publications = 0;
	FDeletionRequestFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	int Calls = 0;
	const auto Result = Service.DeleteItems(Plan, {.RemoveEntry = [&](const auto& Physical) {
		++Calls;
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Physical, Error);
		EXPECT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
		return Error;
	}});

	EXPECT_TRUE(Result.HasCommittedEffects());
	EXPECT_TRUE(ObservedChanges.bFullRefresh);
	EXPECT_FALSE(Plan);
	EXPECT_TRUE(OperationError.empty());
	EXPECT_NE(OperationWarning.find("Registry reconcile is required."), std::string::npos);
	EXPECT_EQ(Publications, 1);
	EXPECT_FALSE(Service.DeleteItems(Plan).IsComplete());
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(Publications, 1);
}

TEST_F(FContentBrowserModelTests, ServiceDoesNotPublishOrChangeIdentityMoves)
{
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Identity", Path));
	InitializeDObjectSystem();
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	int Publications = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "Identity", .PackagePath = Path};
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Item, 1}, (Root / "Content").generic_string()));
	EXPECT_TRUE(Service.RenameItem(Item, "Identity", NewItem));
	EXPECT_EQ(TestPackagePath(NewItem), Path);
	EXPECT_EQ(Material->GetPackage()->GetPackagePathIdentity(), Path);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/Identity.dasset"));
	EXPECT_EQ(Publications, 0);
}

TEST_F(FContentBrowserModelTests, ScopedRefreshPreservesUnrelatedRowsAndTreeIdentity)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/AB";
	std::filesystem::create_directories(A / "Child");
	std::filesystem::create_directories(B);
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto Tree = Model.GetDirectorySnapshot(A.generic_string());
	const auto Requests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	FContentChangeBatch Changes{.Changes = {{EContentChangeKind::Modified,
		(B / "asset.dasset").generic_string(), (B / "asset.dasset").generic_string()}}};
	EXPECT_FALSE(Model.ApplyContentChanges(Changes));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string()), Tree);
	Changes.Changes[0].OldPhysicalPath = Changes.Changes[0].NewPhysicalPath = (A / "asset.dasset").generic_string();
	EXPECT_TRUE(Model.ApplyContentChanges(Changes));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_NE(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string()), Tree);
}

TEST_F(FContentBrowserModelTests, OrdinaryItemDeltasMergeWithoutCaptureAndPreserveUnrelatedRecords)
{
	FProviderQueryFixture Query;
	ASSERT_TRUE(Query.GetServiceForTesting()->DeactivateDataSource(FName("Asset")));
	ASSERT_TRUE(Query.GetServiceForTesting()->DeactivateDataSource(FName("File")));
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Name = "First", .VirtualPath = "/ContentBrowserTests/First"},
		{.Name = "Other", .VirtualPath = "/ContentBrowserTests/Other"}});
	Query.AddSource(*Source);
	Query.GetServiceForTesting()->Tick(0.0f);
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto Calls = Source->CaptureCalls->load();
	const auto Request = FContentBrowserModelFixtureAccess::Request(Model);
	const auto Original = Model.GetItems();
	auto Find = [&](std::string_view Path) -> const Durin::ContentBrowser::FContentBrowserItem* {
		const auto It = std::ranges::find_if(Model.GetItemsSnapshot(), [&](const auto& Item) { return Item.GetVirtualPath() == Path; });
		return It == Model.GetItemsSnapshot().end() ? nullptr : &*It;
	};
	const auto* Other = Find("/ContentBrowserTests/Other");
	ASSERT_NE(Other, nullptr);
	auto Description = [&](std::string Path, std::string Name) {
		return Source->BindItemData(ContentBrowser::FContentBrowserItemData(ContentBrowser::EContentBrowserItemFlags::Type_File,
			std::move(Path), std::move(Name), {}, {}));
	};
	auto Publish = [&](ContentBrowser::FContentBrowserItemDataUpdate Update) {
		Source->QueueItemDataUpdate(std::move(Update));
		Query.GetServiceForTesting()->Tick(0.0f);
		Model.PumpPendingSnapshots();
		EXPECT_EQ(Source->CaptureCalls->load(), Calls);
		EXPECT_EQ(FContentBrowserModelFixtureAccess::Request(Model), Request);
		EXPECT_EQ(Find("/ContentBrowserTests/Other"), Other);
	};
	Publish(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(Description("/ContentBrowserTests/First", "Changed")));
	ASSERT_NE(Find("/ContentBrowserTests/First"), nullptr);
	EXPECT_EQ(Find("/ContentBrowserTests/First")->GetItemName(), "Changed");
	EXPECT_TRUE(std::ranges::any_of(Original, [](const auto& Item) { return Item.GetItemName() == "First"; }));
	Publish(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemAddedUpdate(Description("/ContentBrowserTests/New", "New")));
	Publish(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemAddedUpdate(Description("/ContentBrowserTests/New", "New")));
	EXPECT_EQ(std::ranges::count_if(Model.GetItems(), [](const auto& Item) { return Item.GetVirtualPath() == "/ContentBrowserTests/New"; }), 1);
	Publish(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemMovedUpdate(Description("/ContentBrowserTests/Moved", "Moved"), "/ContentBrowserTests/New"));
	EXPECT_EQ(Find("/ContentBrowserTests/New"), nullptr);
	ASSERT_NE(Find("/ContentBrowserTests/Moved"), nullptr);
	Publish(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(Description("/ContentBrowserTests/Moved", "Moved")));
	EXPECT_EQ(Find("/ContentBrowserTests/Moved"), nullptr);
	Source->Rows[0] = ItemDescriptions({{.Name = "Changed", .VirtualPath = "/ContentBrowserTests/First"}}).front();
	Model.SetSearch("Changed");
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(Model.GetItems().front().GetItemName(), "Changed");
	const auto SearchCalls = Source->CaptureCalls->load();
	Source->QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(Description("/ContentBrowserTests/First", "Hidden")));
	Query.GetServiceForTesting()->Tick(0.0f);
	Model.PumpPendingSnapshots();
	EXPECT_TRUE(Model.GetItems().empty());
	EXPECT_EQ(Source->CaptureCalls->load(), SearchCalls);
}

TEST_F(FContentBrowserModelTests, InFlightBaselineReplaysDeltasWithoutRestartOrResurrection)
{
	FProviderQueryFixture Query;
	auto* Data = Query.GetServiceForTesting();
	ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
	ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Data, {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Name = "Old", .VirtualPath = "/ContentBrowserTests/First"},
		{.Name = "Gone", .VirtualPath = "/ContentBrowserTests/Gone"},
		{.Name = "From", .VirtualPath = "/ContentBrowserTests/From"}});
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Source->BeforeCapture = [Gate] { Gate->Pause(); };
	Query.AddSource(*Source);
	Data->Tick(0.0f);
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Request = FContentBrowserModelFixtureAccess::Request(Model);
	const auto Calls = Source->CaptureCalls->load();
	auto Description = [&](std::string Name) {
		return ContentBrowser::FContentBrowserItemData(*Source, ContentBrowser::EContentBrowserItemFlags::Type_File,
			"/ContentBrowserTests/" + Name, Name, {}, {});
	};
	using FUpdate = ContentBrowser::FContentBrowserItemDataUpdate;
	Source->QueueItemDataUpdate(FUpdate::MakeItemModifiedUpdate(Description("First")));
	Source->QueueItemDataUpdate(FUpdate::MakeItemRemovedUpdate(Description("Gone")));
	Source->QueueItemDataUpdate(FUpdate::MakeItemMovedUpdate(Description("To"), "/ContentBrowserTests/From"));
	Source->QueueItemDataUpdate(FUpdate::MakeItemAddedUpdate(Description("Transient")));
	Source->QueueItemDataUpdate(FUpdate::MakeItemRemovedUpdate(Description("Transient")));
	Source->QueueItemDataUpdate(FUpdate::MakeItemAddedUpdate(Description("Live")));
	Source->QueueItemDataUpdate(FUpdate::MakeItemModifiedUpdate(Description("First")));
	Data->Tick(0.0f);
	Model.PumpPendingSnapshots();
	EXPECT_EQ(FContentBrowserModelFixtureAccess::Request(Model), Request);
	EXPECT_EQ(Source->CaptureCalls->load(), Calls);
	Gate->Release();
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 3u);
	for (const auto Name : {"First", "To", "Live"})
		EXPECT_TRUE(std::ranges::any_of(Model.GetItems(), [&](const auto& Item) { return Item.GetItemName() == Name; }));
	for (const auto Name : {"Old", "Gone", "From", "Transient"})
		EXPECT_FALSE(std::ranges::any_of(Model.GetItems(), [&](const auto& Item) { return Item.GetItemName() == Name; }));
	EXPECT_EQ(FContentBrowserModelFixtureAccess::Request(Model), Request);
	EXPECT_EQ(Source->CaptureCalls->load(), Calls);
	EXPECT_FALSE(Model.IsLoading());
}

TEST_F(FContentBrowserModelTests, BaselineBindingEventsAreReplayedBeforeFirstPublication)
{
	FProviderQueryFixture Query;
	auto* Data = Query.GetServiceForTesting();
	ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
	ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Data, {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Name = "Keep", .VirtualPath = "/ContentBrowserTests/Keep"},
		{.Name = "Gone", .VirtualPath = "/ContentBrowserTests/Gone"}});
	Query.AddSource(*Source);
	Data->Tick(0.0f);
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const auto Request = FContentBrowserModelFixtureAccess::Request(Model);
	Source->BeforeBind = [&] {
		Source->QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(
			ContentBrowser::FContentBrowserItemData(*Source, ContentBrowser::EContentBrowserItemFlags::Type_File,
				"/ContentBrowserTests/Gone", "Gone", {}, {})));
		Data->Tick(0.0f);
		EXPECT_TRUE(Model.GetItems().empty());
	};
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(Model.GetItems().front().GetItemName(), "Keep");
	EXPECT_EQ(FContentBrowserModelFixtureAccess::Request(Model), Request);
	EXPECT_EQ(Source->CaptureCalls->load(), 1);
}

TEST_F(FContentBrowserModelTests, FolderContributionRemovalKeepsOtherOwnerWithoutRecapture)
{
	FProviderQueryFixture Query;
	ASSERT_TRUE(Query.GetServiceForTesting()->DeactivateDataSource(FName("Asset")));
	ASSERT_TRUE(Query.GetServiceForTesting()->DeactivateDataSource(FName("File")));
	auto* Left = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	auto* Right = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Right->SourceName = FName("OtherMemory");
	Left->UseCompiledCapture = Right->UseCompiledCapture = true;
	Left->Rows = Right->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Shared", .VirtualPath = "/ContentBrowserTests/Shared"}});
	Query.AddSource(*Left);
	Query.AddSource(*Right);
	Query.GetServiceForTesting()->Tick(0.0f);
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 1u);
	ASSERT_EQ(Model.GetItems().front().InternalItems.size(), 2u);
	const auto Original = Model.GetItems();
	const auto Request = FContentBrowserModelFixtureAccess::Request(Model);
	const auto LeftCalls = Left->CaptureCalls->load(), RightCalls = Right->CaptureCalls->load();
	const auto Removed = Left->BindItemData(*Left->Rows.front().GetPrimaryInternalItem());
	Left->QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(Removed));
	Query.GetServiceForTesting()->Tick(0.0f);
	Model.PumpPendingSnapshots();
	ASSERT_EQ(Model.GetItems().size(), 1u);
	ASSERT_EQ(Model.GetItems().front().InternalItems.size(), 1u);
	EXPECT_EQ(Model.GetItems().front().InternalItems.front().GetOwnerDataSource(), Right);
	EXPECT_EQ(Model.GetItems().front().GetItemKey(), Original.front().GetItemKey());
	EXPECT_EQ(Original.front().InternalItems.size(), 2u);
	EXPECT_EQ(FContentBrowserModelFixtureAccess::Request(Model), Request);
	EXPECT_EQ(Left->CaptureCalls->load(), LeftCalls);
	EXPECT_EQ(Right->CaptureCalls->load(), RightCalls);
	Right->QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemMovedUpdate(
		ContentBrowser::FContentBrowserItemData(*Right, ContentBrowser::EContentBrowserItemFlags::Type_Folder,
			"/ContentBrowserTests/Renamed", "Renamed", {}, {}), "/ContentBrowserTests/Shared/"));
	Query.GetServiceForTesting()->Tick(0.0f);
	Model.PumpPendingSnapshots();
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(Model.GetItems().front().GetVirtualPath(), "/ContentBrowserTests/Renamed");
	EXPECT_EQ(FContentBrowserModelFixtureAccess::Request(Model), Request);
	EXPECT_EQ(Left->CaptureCalls->load(), LeftCalls);
	EXPECT_EQ(Right->CaptureCalls->load(), RightCalls);
}

TEST_F(FContentBrowserModelTests, CurrentDirectoryMoveMigratesHistoryWithoutAddingNavigationEntries)
{
	FProviderQueryFixture Query;
	auto* Data = Query.GetServiceForTesting();
	ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
	ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Data, {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Name = "Leaf", .VirtualPath = "/ContentBrowserTests/Before/Nested/Leaf"}});
	Query.AddSource(*Source);
	Data->Tick(0.0f);
	const auto Before = Root / "Content/Before", After = Root / "Content/After";
	std::filesystem::create_directories(Before / "Nested");
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToPhysical((Before / "Nested").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto HistorySize = Model.GetHistory().size();
	const auto HistoryIndex = Model.GetHistoryIndex();
	std::filesystem::rename(Before, After);
	Source->Rows = ItemDescriptions({{.Name = "Leaf", .VirtualPath = "/ContentBrowserTests/After/Nested/Leaf"}});
	Source->QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemMovedUpdate(
		ContentBrowser::FContentBrowserItemData(*Source, ContentBrowser::EContentBrowserItemFlags::Type_Folder,
			"/ContentBrowserTests/After", "After", {}, {}), "/ContentBrowserTests/Before/"));
	Data->Tick(0.0f);
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), (After / "Nested").generic_string());
	EXPECT_EQ(Model.GetHistory().size(), HistorySize);
	EXPECT_EQ(Model.GetHistoryIndex(), HistoryIndex);
	EXPECT_EQ(Model.GetHistory()[HistoryIndex], (After / "Nested").generic_string());
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(Model.GetItems().front().GetVirtualPath(), "/ContentBrowserTests/After/Nested/Leaf");
}

TEST_F(FContentBrowserModelTests, ModifiedItemsRetainThumbnailDemandUntilReplacementIsPublished)
{
	FPackagePath FirstPath, SecondPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/First", FirstPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Second", SecondPath));
	DMaterial *First = nullptr, *Second = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FirstPath, First));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SecondPath, Second));
	ASSERT_TRUE(SavePackage(First->GetPackage()));
	ASSERT_TRUE(SavePackage(Second->GetPackage()));
	auto Renderer = FThumbnailManagerTestAccess::RegisterScoped(GetDefaultThumbnailManager(),
		MakeThumbnailRenderer<FRouteOnlyThumbnailRenderer>(), "Durin::DMaterial");
	ASSERT_TRUE(Renderer);
	const auto File = Root / "Content/marker.txt";
	std::ofstream(File) << "before";
	FProviderQueryFixture Query;
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto Original = Model.GetItems();
	ASSERT_EQ(std::ranges::count_if(Original, [](const auto& Item) { return Item.IsAsset() && !Item.IsFolder(); }), 2);
	FContentBrowserThumbnailReferences References;
	auto RequestFrame = [&] {
		References.BeginFrame();
		for (const auto& Item : Model.GetItems()) References.Request(Item, EAssetThumbnailPriority::Visible);
		References.EndFrame();
	};
	RequestFrame();
	ASSERT_EQ(References.GetAssetReferenceCountForTesting(), 2u);
	auto Gate = std::make_shared<FBrowserScanGate>();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	std::ofstream(File) << "after, with updated size";
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Modified,
		File.generic_string(), File.generic_string()}}}));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	for (int Frame = 0; Frame < 8; ++Frame)
	{
		RequestFrame();
		EXPECT_EQ(Model.GetItems().size(), Original.size());
		EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 2u);
		for (const auto& Item : Original)
			if (Item.IsAsset() && !Item.IsFolder()) EXPECT_EQ(References.Find(Item.GetItemKey()).State, EAssetThumbnailState::Pending);
	}
	Gate->Release();
	FContentBrowserModelFixtureAccess::Wait(Model);
	RequestFrame();
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 2u);
	const auto Updated = std::ranges::find_if(Model.GetItems(), [](const auto& Item) { return Item.GetItemName() == "marker.txt"; });
	ASSERT_NE(Updated, Model.GetItems().end());
	EXPECT_EQ(TestFileSize(*Updated), std::filesystem::file_size(File));
	// Explicit refresh/navigation still retires actionable rows immediately.
	Model.RefreshItemsSnapshot();
	EXPECT_TRUE(Model.GetItems().empty());
	FContentBrowserModelFixtureAccess::Wait(Model);
}

TEST_F(FContentBrowserModelTests, ScopedRefreshDistinguishesRecursiveSearchAndDirectoryStructure)
{
	const auto A = Root / "Content/A";
	std::filesystem::create_directories(A / "Nested");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	FContentChangeBatch Changes{.Changes = {{EContentChangeKind::Added, {},
		(A / "Nested/file.txt").generic_string()}}};
	EXPECT_FALSE(Model.ApplyContentChanges(Changes));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetSearch("file");
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_TRUE(Model.ApplyContentChanges(Changes));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.SetSearch("");
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	std::filesystem::create_directory(A / "New");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "New").generic_string(), {}, {}, true}}}));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
}

TEST_F(FContentBrowserModelTests, DirectoryRenameMigratesNavigationAndDeletionFallsBack)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/Renamed";
	std::filesystem::create_directories(A / "Nested");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.NavigateToPhysical((A / "Nested").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	std::filesystem::rename(A, B);
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Renamed,
		A.generic_string(), B.generic_string(), {}, {}, true}}}));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), (B / "Nested").generic_string());
	EXPECT_EQ(Model.GetHistory().front(), B.generic_string());
	std::filesystem::remove(B / "Nested");
	std::filesystem::remove(B);
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Removed,
		B.generic_string(), {}, {}, {}, true}}}));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), (Root / "Content").generic_string());
}

TEST_F(FContentBrowserModelTests, UnrelatedChangesPreserveInFlightCaptureAndRelatedChangesCoalesce)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/B";
	std::filesystem::create_directory(A / "Original");
	auto Gate = std::make_shared<FBrowserScanGate>();
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Requests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	EXPECT_FALSE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(B / "Other").generic_string(), {}, {}, true}}}));
	EXPECT_EQ(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
	std::filesystem::create_directory(A / "Latest");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "Latest").generic_string(), {}, {}, true}}}));
	EXPECT_NE(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
	Gate->Release();
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetItems().size(), 2u);
	EXPECT_FALSE(Model.IsLoading());
}

TEST_F(FContentBrowserModelTests, MountedReconciliationFencePreventsPrematurePublication)
{
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Child");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	FContentBrowserRefreshCoordinator Coordinator;
	Coordinator.SetPublicationSuspension([&](bool bSuspended) { Model.SetSnapshotPublicationSuspended(bSuspended); });
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Coordinator.ObserveMountedContent({.bFullRefresh = true});
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_TRUE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	ASSERT_TRUE(Coordinator.Synchronize(
		[] { return std::expected<void, Durin::UnifiedError::FError>{}; }, [](const FContentChangeBatch&) {}));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Model.GetItems().size(), 1u);
}

TEST_F(FContentBrowserModelTests, ScopedServiceNotificationsDescribeCommittedFilesystemOperations)
{
	std::vector<FContentChangeBatch> Batches;

	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { Batches.push_back(std::move(Batch)); }});
	Observe(Service);
	FContentBrowserItem CreatedItem;
	const auto Created = Service.CreateFolder((Root / "Content/A").generic_string(), "New Folder", CreatedItem);
	ASSERT_TRUE(Created);
	ASSERT_EQ(Batches.size(), 1u);
	ASSERT_FALSE(Batches[0].bFullRefresh);
	ASSERT_EQ(Batches[0].Changes.size(), 1u);
	EXPECT_EQ(TestKind(Batches[0].Changes[0]), EContentChangeKind::Added);
	EXPECT_TRUE(Batches[0].Changes[0].bDirectory);
	FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Folder, .Name = "New Folder",
		.PhysicalPath = TestPhysicalPath(CreatedItem)};
	FContentBrowserItem RenamedItem;
	const auto Renamed = Service.RenameItem(Item, "Renamed", RenamedItem);
	ASSERT_TRUE(Renamed);
	ASSERT_EQ(Batches.size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(Batches[1].Changes, [&](const auto& Change) { return Change.Kind == EContentChangeKind::Removed && Change.OldPhysicalPath == TestPhysicalPath(CreatedItem); }));
	EXPECT_EQ(Batches[1].Changes[0].NewPhysicalPath, TestPhysicalPath(RenamedItem));
	FContentBrowserItem FailedItem;
	const auto Failed = Service.RenameItem(Item, "Gone", FailedItem);
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
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ModelSource.GetServiceForTesting()->Tick(0.0f);
	Model.PumpPendingSnapshots();
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Requests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	const auto Revision = GetAssetCatalogRevision();
	auto Publication = CaptureAssetRegistryPublication();
	Publication.Assets.at(BPath).SearchableNames = {"ChangedOutsideScope"};
	ASSERT_TRUE(PublishAssetRegistryPublication(Publication));
	const auto Changes = CaptureAssetCatalogChanges(Revision);
	ASSERT_FALSE(Changes.bFullRefresh);
	ASSERT_EQ(Changes.Changes.size(), 1u);
	EXPECT_EQ(TestKind(Changes.Changes[0]), EContentChangeKind::Modified);
	EXPECT_EQ(Changes.Changes[0].NewAssetPath, BPath.ToString());
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_EQ(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
	const auto CommittedRevision = GetAssetCatalogRevision();
	EXPECT_FALSE(PublishAssetRegistryPublication(Publication));
	EXPECT_EQ(GetAssetCatalogRevision(), CommittedRevision);
	EXPECT_TRUE(CaptureAssetCatalogChanges(CommittedRevision).Changes.empty());
	Publication = CaptureAssetRegistryPublication();
	Publication.Assets.at(APath).SearchableNames = {"ChangedInsideScope"};
	ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
	(void)CaptureAssetQuerySnapshot();
	ModelSource.GetServiceForTesting()->Tick(0.0f);
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_NE(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
	Gate->Release();
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), (Root / "Content/A").generic_string());
}

TEST_F(FContentBrowserModelTests, LocalTreeInvalidationPreservesUnrelatedInFlightEnumeration)
{
	const auto A = Root / "Content/A";
	const auto B = Root / "Content/B";
	std::filesystem::create_directory(A / "Child");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(B.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
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
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string())->Children.size(), 1u);
}

TEST_F(FContentBrowserModelTests, SourceThumbnailRequestsUseFileIdentityAndInvalidateOnChange)
{
	FProviderQueryFixture Query;
	FContentBrowserThumbnailReferences References;
	const auto Image = Query.BindItemForTesting(Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::File,
		.PhysicalPath = (Root / "Content/Preview.png").generic_string(), .Extension = ".png",
		.FileSize = 128, .LastWriteTime = std::filesystem::file_time_type::clock::now()});
	References.BeginFrame();
	References.Request(Image, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.Find(Image.GetItemKey()).State, EAssetThumbnailState::Pending);
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 0u);
	References.ApplyContentChanges({.Changes = {{EContentChangeKind::Modified, TestPhysicalPath(Image), TestPhysicalPath(Image)}}});
	EXPECT_EQ(References.Find(Image.GetItemKey()).State, EAssetThumbnailState::None);
	const auto Text = Query.BindItemForTesting(FContentBrowserItem{.Kind = EContentBrowserItemKind::File,
		.PhysicalPath = (Root / "Content/Notes.txt").generic_string(), .Extension = ".txt"});
	References.Request(Text, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.Find(Text.GetItemKey()).State, EAssetThumbnailState::None);
}

TEST_F(FContentBrowserModelTests, ThumbnailReferencesReleaseCardsOutsideCurrentFrame)
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
	const auto OwnerData = FindPackageExact(OwnerPath);
	const auto OtherData = FindPackageExact(OtherPath);
	auto OwnerRenderer = Durin::FThumbnailManagerTestAccess::RegisterScoped(GetDefaultThumbnailManager(),
		Durin::MakeThumbnailRenderer<FRouteOnlyThumbnailRenderer>(), TestAssetClassName(OwnerData->TopLevelAssets.front()));
	auto OtherRenderer = Durin::FThumbnailManagerTestAccess::RegisterScoped(GetDefaultThumbnailManager(),
		Durin::MakeThumbnailRenderer<FRouteOnlyThumbnailRenderer>(), TestAssetClassName(OtherData->TopLevelAssets.front()));
	ASSERT_TRUE(OwnerRenderer);
	ASSERT_TRUE(OtherRenderer);
	FProviderQueryFixture Query;
	FContentBrowserThumbnailReferences References;
	for (const auto& Path : {OwnerPath, OtherPath})
	{
		const auto Asset = FindPackageExact(Path);
		ASSERT_TRUE(Asset);
		const auto Identity = Asset->TopLevelAssets.front().AssetPath.ToString();
		References.Request(Query.BindItemForTesting(Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Identity,
			.PackagePath = Path}), EAssetThumbnailPriority::Prefetch);
	}
	ASSERT_EQ(References.GetAssetReferenceCountForTesting(), 2u);
	const auto OwnerItem = Query.BindItemForTesting(AssetItem(OwnerPath));
	const auto OtherItem = Query.BindItemForTesting(AssetItem(OtherPath));
	FContentBrowserThumbnailReferences SecondPanel;
	SecondPanel.BeginFrame();
	SecondPanel.Request(OtherItem, EAssetThumbnailPriority::Visible);
	SecondPanel.EndFrame();
	References.BeginFrame();
	References.Request(OwnerItem, EAssetThumbnailPriority::Visible);
	References.EndFrame();
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 1u);
	EXPECT_EQ(SecondPanel.GetAssetReferenceCountForTesting(), 1u);
	EXPECT_EQ(SecondPanel.Find(OtherItem.GetItemKey()).State, EAssetThumbnailState::Pending);
	References.BeginFrame();
	References.Request(OtherItem, EAssetThumbnailPriority::Prefetch);
	References.EndFrame();
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 1u);
	References.BeginFrame();
	References.EndFrame();
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 0u);
	EXPECT_EQ(SecondPanel.Find(OtherItem.GetItemKey()).State, EAssetThumbnailState::Pending);
}

TEST_F(FContentBrowserModelTests, AssetAdditionIntroducesMissingDirectoryWithoutInvalidatingExistingChildren)
{
	const auto A = Root / "Content/A";
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	std::filesystem::create_directories(A / "New/Nested");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "New/Nested/Asset.dasset").generic_string()}}}));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(TestName(Model.GetItems().front()), "New");
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	FContentBrowserModelFixtureAccess::Wait(Model);
	const auto Tree = Model.GetDirectorySnapshot(A.generic_string());
	const auto Requests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	EXPECT_FALSE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "New/Nested/Another.dasset").generic_string()}}}));
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string()), Tree);
	EXPECT_EQ(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model), Requests);
}

TEST_F(FContentBrowserModelTests, FreshCaptureCanProceedWhileReconciliationFailureIsSuppressed)
{
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Child");
	FProviderQueryFixture ModelSource;
	FContentBrowserModel Model(ModelSource);
	FContentBrowserRefreshCoordinator Coordinator;
	Coordinator.SetPublicationSuspension([&](bool bSuspended) { Model.SetSnapshotPublicationSuspended(bSuspended); });
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Coordinator.ObserveMountedContent({.bFullRefresh = true});
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	int Attempts = 0;
	const auto Reconcile = [&] { ++Attempts; return std::expected<void, Durin::UnifiedError::FError>{std::unexpected(Durin::UnifiedError::FError{Durin::PackageReadErrors::IoError, "forced failure"})}; };
	EXPECT_FALSE(Coordinator.Synchronize( Reconcile,
		[](const FContentChangeBatch&) {}));
	// A fresh request can proceed, and a suppressed failure must not suspend it again.
	Model.RefreshItemsSnapshot(false);
	EXPECT_TRUE(Coordinator.Synchronize( Reconcile,
		[](const FContentChangeBatch&) {}));
	EXPECT_EQ(Attempts, 1);
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Model.GetItems().size(), 1u);
}

TEST_F(FContentBrowserModelTests, ContentMovePreservesEmptyFoldersAndCollapsesDescendants)
{
	std::filesystem::create_directories(Root / "Content/A/Empty/Nested");
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::Folder, .PhysicalPath = (Root / "Content/A").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/notes.txt").generic_string()}};

	std::vector<FContentChangeBatch> Batches;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](auto Batch) { Batches.push_back(std::move(Batch)); }});
	Observe(Service);
	const auto Result = Service.BulkMoveItems(Items, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/B/A/Empty/Nested"));
	ASSERT_EQ(Batches.size(), 1);
	EXPECT_EQ(1u, std::ranges::count_if(Batches[0].Changes, [&](const auto& Change) { return Change.Kind == EContentChangeKind::Renamed && Change.OldPhysicalPath == (Root / "Content/A/notes.txt").generic_string(); }));
	EXPECT_TRUE(std::ranges::any_of(Batches[0].Changes, [&](const auto& Change) { return Change.Kind == EContentChangeKind::Removed && Change.bDirectory && Change.OldPhysicalPath == (Root / "Content/A").generic_string(); }));
}

TEST_F(FContentBrowserModelTests, ContentMoveKeepsEarlierCommitAndRejectsStructuralTargets)
{
	std::ofstream(Root / "Content/A/one.txt") << "one";
	std::ofstream(Root / "Content/A/two.txt") << "two";
	std::ofstream(Root / "Content/B/two.txt") << "occupied";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/one.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/two.txt").generic_string()}};
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	EXPECT_TRUE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/one.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/one.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/two.txt"));
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/A").generic_string()));
	const FContentBrowserItem Mount{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content").generic_string()};
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Mount, 1}, (Root / "Content/B").generic_string()));
	EXPECT_FALSE(Service.BulkMoveItems(Items, Root.generic_string()));
}

TEST_F(FContentBrowserModelTests, MixedContentMoveKeepsIndependentFileCommitWhenAssetMoveFails)
{
	FPackagePath Package;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Asset", Package));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Package, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	std::filesystem::create_directories(Root / "Content/A/Empty");
	FPackagePath Occupied; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/A/Asset", Occupied));
	DMaterial* Blocker = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Occupied, Blocker)); ASSERT_TRUE(SavePackage(Blocker->GetPackage()));
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	EXPECT_TRUE(Result);
	EXPECT_NE(OperationError.find("occupied"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/Asset.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A"));
	EXPECT_FALSE(ObservedChanges.bFullRefresh);
	EXPECT_FALSE(ObservedChanges.Changes.empty());
	EXPECT_TRUE(Testing::RemoveAssetPackageForTests(Occupied));
}

TEST_F(FContentBrowserModelTests, MixedContentMoveRelocatesAssetsAndOrdinaryFilesTogether)
{
	FPackagePath Source;
	FPackagePath Destination;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Asset", Source));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/A/Asset", Destination));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Source, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/notes.txt"));
	EXPECT_EQ(Material->GetObjectPath(), "/ContentBrowserTests/B/A/Asset.Asset") << OperationError;
	EXPECT_EQ(ResolvePackagePathForOperation(Source).FinalPath.ToString(), Destination.ToString()) << OperationError;
}

TEST_F(FContentBrowserModelTests, ContentMoveRejectsUnknownPackagesAndKeepsFirstDuplicateDestination)
{
	std::ofstream(Root / "Content/A/Unknown.dasset") << "invalid package";
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string()));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/A"));
	std::ofstream(Root / "Content/A/same.txt") << "a";
	std::ofstream(Root / "Content/B/same.txt") << "b";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/same.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/B/same.txt").generic_string()}};
	EXPECT_TRUE(Service.BulkMoveItems(Items, (Root / "Content").generic_string()));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/same.txt"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/same.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/same.txt"));
	std::string Contents; std::ifstream(Root / "Content/same.txt") >> Contents; EXPECT_EQ("a", Contents);
}

TEST_F(FContentBrowserModelTests, ContentMoveReportsNativePartialPublicationAndRetainsRecoveryEvidence)
{
	FPackagePath Package;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/ProjectionAsset", Package));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Package, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	std::vector<std::string> Warnings;
	Service.SetDiagnosticSink([&](ELogLevel Level, std::string_view Message) {
		if (Level >= ELogLevel::Error) OperationError = Message;
		else if (Level == ELogLevel::Warn) Warnings.emplace_back(Message);
	});
	Testing::FScopedPackageSaveFault Fault([](auto Point, size_t) { return Point == Testing::EPackageSaveFaultPoint::PublishRegistry; });

	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	EXPECT_TRUE(Result);

	EXPECT_FALSE(OperationError.empty());
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/ProjectionAsset.dasset")) << OperationError;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/ProjectionAsset.dasset"));
	EXPECT_TRUE(ObservedChanges.bFullRefresh);
}

TEST_F(FContentBrowserModelTests, ContentMoveMovesMultipleFilesAndRejectsIdentityMoves)
{
	std::ofstream(Root / "Content/A/one.txt") << "one";
	std::ofstream(Root / "Content/A/two.txt") << "two";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/one.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/two.txt").generic_string()}};
	int Publications = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkMoveItems(Items, (Root / "Content/A").generic_string()));
	EXPECT_EQ(Publications, 0);
	ASSERT_TRUE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_EQ(Publications, 1);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/one.txt"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/two.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/one.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/two.txt"));
}

TEST_F(FContentBrowserModelTests, ContentCopyPreservesBytesAndRejectsExistingNames)
{
	std::ofstream(Root / "Content/A/notes.txt", std::ios::binary).write("hello\0world", 11);
	std::ofstream(Root / "Content/B/notes.txt") << "other";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/notes.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/B/notes.txt").generic_string()}};
	int Notifications = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](auto) { ++Notifications; }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkCopyItems(Items, (Root / "Content/A").generic_string()));
	EXPECT_FALSE(Service.BulkCopyItems(std::span{Items.data(), 1}, (Root / "Content/B").generic_string()));
	EXPECT_EQ(0, Notifications);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/notes_Copy.txt"));
	std::ifstream Existing(Root / "Content/B/notes.txt", std::ios::binary);
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(Existing), {}), "other");
	Existing.close();
	std::filesystem::remove(Root / "Content/B/notes.txt");
	ASSERT_TRUE(Service.BulkCopyItems(std::span{Items.data(), 1}, (Root / "Content/B").generic_string()));
	EXPECT_EQ(1, Notifications);
	std::ifstream Copied(Root / "Content/B/notes.txt", std::ios::binary);
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(Copied), {}), std::string("hello\0world", 11));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/notes.txt"));
}

TEST_F(FContentBrowserModelTests, RejectedTransferReportsOnceWithoutMutationNotification)
{
	std::ofstream(Root / "Content/A/one.txt") << "one";
	std::ofstream(Root / "Content/A/two.txt") << "two";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/one.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/two.txt").generic_string()}};
	int Errors = 0, Notifications = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch) { ++Notifications; }});
	Service.SetDiagnosticSink([&](ELogLevel Level, std::string_view Message) { if (Level >= ELogLevel::Error && !Message.empty()) ++Errors; });
	EXPECT_FALSE(Service.BulkCopyItems(Items, (Root / "Content/A").generic_string()));
	EXPECT_EQ(1, Errors);
	EXPECT_EQ(0, Notifications);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/one.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/two.txt"));
}

TEST_F(FContentBrowserModelTests, TransferFeedbackWithdrawalPreservesPriorSuccessAndPublishesOnce)
{
	std::filesystem::create_directories(Root / "Content/C");
	std::filesystem::create_directories(Root / "Content/D");
	std::ofstream(Root / "Content/A/same.txt") << "first";
	std::ofstream(Root / "Content/B/same.txt") << "second";
	std::ofstream(Root / "Content/C/later.txt") << "later";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/same.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/B/same.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/C/later.txt").generic_string()}};
	bool Allowed = true;
	int CopyErrors = 0, Notifications = 0;
	FContentChangeBatch Published;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ++Notifications; Published = std::move(Batch); },
		.CanMutateContent = [&] { return Allowed; }});
	Service.SetDiagnosticSink([&](ELogLevel Level, std::string_view Message) {
		if (Level >= ELogLevel::Error && Message.starts_with("Copy failed:")) { ++CopyErrors; Allowed = false; }
	});
	EXPECT_TRUE(Service.BulkCopyItems(Items, (Root / "Content/D").generic_string()));
	EXPECT_FALSE(Allowed);
	EXPECT_EQ(1, CopyErrors);
	EXPECT_EQ(1, Notifications);
	ASSERT_EQ(1u, Published.Changes.size());
	EXPECT_EQ(EContentChangeKind::Added, Published.Changes.front().Kind);
	EXPECT_EQ((Root / "Content/D/same.txt").generic_string(), Published.Changes.front().NewPhysicalPath);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/D/later.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/C/later.txt"));
	std::string Bytes; std::ifstream(Root / "Content/D/same.txt") >> Bytes;
	EXPECT_EQ("first", Bytes);
}

TEST_F(FContentBrowserModelTests, ContentCopyPreservesDirectoriesAndCollapsesSelectedDescendants)
{
	std::filesystem::create_directories(Root / "Content/A/Empty/Nested");
	std::ofstream(Root / "Content/A/.hidden") << "hidden";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::Folder, .PhysicalPath = (Root / "Content/A").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/.hidden").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::Folder, .PhysicalPath = (Root / "Content/A/Empty").generic_string()}};
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	ASSERT_TRUE(Service.BulkCopyItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/B/A/Empty/Nested"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/.hidden"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/.hidden"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/Empty"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/.hidden"));
	EXPECT_FALSE(Service.BulkCopyItems(Items, (Root / "Content").generic_string()));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A_Copy/.hidden"));
}

TEST_F(FContentBrowserModelTests, ContentCopyRejectsUnsafeScopeBeforeWriting)
{
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	EXPECT_FALSE(Service.BulkCopyItems(std::span{&Folder, 1}, (Root / "Content/A").generic_string()));
	EXPECT_FALSE(Service.BulkCopyItems(std::span{&Folder, 1}, Root.generic_string()));
	for (const auto* Name : {"Orphan.DBULK", "Unknown.DASSET", "missing.txt"})
	{
		if (std::string_view(Name) != "missing.txt") std::ofstream(Root / "Content/A" / Name) << "invalid";
		const std::array Items{
			FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/notes.txt").generic_string()},
			FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A" / Name).generic_string()}};
		EXPECT_FALSE(Service.BulkCopyItems(Items, (Root / "Content/B").generic_string()));
		EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/notes.txt"));
	}
	EXPECT_FALSE(Service.BulkCopyItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string()));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/A"));
}

TEST_F(FContentBrowserModelTests, MixedContentCopyDuplicatesAssetAndPreservesSource)
{
	FPackagePath Source, Destination;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/CopySource", Source));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/A/CopySource", Destination));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Source, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkCopyItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_TRUE(FindPackageExact(Source));
	ASSERT_NE(FindResidentPackage(Destination), nullptr) << OperationError;
	EXPECT_TRUE(FindResidentPackage(Destination)->IsDirty());
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/CopySource.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/A/CopySource.dasset"));
	ASSERT_TRUE(SavePackage(FindResidentPackage(Destination)));
	EXPECT_TRUE(FindPackageExact(Destination));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
}

TEST_F(FContentBrowserModelTests, PasteKeepsDirtyCopyAndExplicitSaveFailureAllowsRetry)
{
	std::optional<Durin::UnifiedError::FError> LoadedCopyError;
	FPackagePath Source;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/PasteSaveFailure", Source));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Source, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto AssetPath = Testing::MakePackageLeafTopLevelAssetPathForTests(Source);
	FProviderAssetActionFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	bool Result = false;
	FContentBrowserItem ResultItem;
	Result = Service.CopyToFolder(AssetItem(AssetPath), (Root / "Content/B").generic_string(), ResultItem);
	EXPECT_TRUE(Result);
	EXPECT_TRUE((ObservedChanges.bFullRefresh || !ObservedChanges.Changes.empty()));
	EXPECT_TRUE(OperationWarning.empty());
	FSoftObjectPath CopyPath;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(TestVirtualPath(ResultItem), CopyPath));
	auto LoadedCopy = LoadObject<DMaterial>(CopyPath, nullptr, &LoadedCopyError);
	ASSERT_TRUE(LoadedCopy);
	auto* Copy = LoadedCopy;
	ASSERT_NE(Copy, nullptr);
	EXPECT_TRUE(Copy->GetPackage()->IsDirty());
	EXPECT_EQ(FindResidentPackage(CopyPath.GetPackagePath()), Copy->GetPackage());
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(ResultItem)));
	{
		Testing::FScopedPackageSaveFault Fault([](auto Phase, size_t) { return Phase == Testing::EPackageSaveFaultPoint::PublishPackage; });
		EXPECT_FALSE(SavePackage(Copy->GetPackage()));
	}
	EXPECT_TRUE(Copy->GetPackage()->IsDirty());
	EXPECT_TRUE(SavePackage(Copy->GetPackage()));
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(ResultItem)));
}

TEST_F(FContentBrowserModelTests, ContentCopyBulkTravelsOnlyThroughOwningAssetDuplication)
{
	FPackagePath SourcePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/CopyTexture", SourcePath));
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Texture));
	auto Image = Image::FImage::TryCreate({.Width = 512, .Height = 512,
		.Format = Image::ERawImageFormat::RGBA8, .GammaSpace = Image::EImageGammaSpace::SRGB},
		FByteBuffer(512 * 512 * 4, std::byte{71}));
	ASSERT_TRUE(Image);
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image->GetView(), 4, 0, ETextureSourceCompression::Raw));
	Texture->SetSource(Source);
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(std::filesystem::exists(Root / "Content/A/CopyTexture.dbulk"));
	const FContentBrowserItem Bulk{.Kind = EContentBrowserItemKind::File,
		.PhysicalPath = (Root / "Content/A/CopyTexture.dbulk").generic_string()};
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkCopyItems(std::span{&Bulk, 1}, (Root / "Content/B").generic_string()));
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkCopyItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/A/CopyTexture.dbulk"));
	FPackagePath CopiedPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/A/CopyTexture", CopiedPath));
	ASSERT_NE(FindResidentPackage(CopiedPath), nullptr) << OperationError;
	EXPECT_TRUE(FindResidentPackage(CopiedPath)->IsDirty());
	ASSERT_TRUE(SavePackage(FindResidentPackage(CopiedPath)));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/CopyTexture.dbulk"));
	const auto Copied = FindPackageExact(CopiedPath);
	ASSERT_NE(Copied, nullptr);
	EXPECT_GT(Copied->BulkSegmentExtent, 0u);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/CopyTexture.dbulk"));
}

TEST_F(FContentBrowserModelTests, SaveReportsCommittedFileWhenRegistryPublicationFails)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/SaveProjectionPending", Path));
	DMaterial* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	int Notifications = 0;
	FProviderSaveFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Notifications; }});
	Observe(Service);
	bool Result = false;
	FContentBrowserItem ResultItem;
	{
		Testing::FScopedPackageSaveFault Fault([](auto Phase, size_t) {
			return Phase == Testing::EPackageSaveFaultPoint::PublishRegistry;
		});
		Result = Service.BulkSaveItems(std::array{AssetItem(Path)});
	}
	EXPECT_TRUE(Result);
	EXPECT_TRUE((ObservedChanges.bFullRefresh || !ObservedChanges.Changes.empty()));
	EXPECT_EQ(Notifications, 1);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/SaveProjectionPending.dasset"));
	EXPECT_FALSE(Asset->GetPackage()->IsDirty());
	EXPECT_TRUE(IsAssetRegistryProjectionFenced(Path));
	EXPECT_FALSE(OperationError.empty());
	EXPECT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
}

TEST_F(FContentBrowserModelTests, CapabilityQueriesExplainRejectionWithoutExecutingOrReporting)
{
	const auto File = Root / "Content/A/capability.txt";
	std::ofstream(File) << "original";
	FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File, .Name = "capability.txt", .PhysicalPath = File.generic_string()};
	bool bAllowed = true;
	int Updates = 0;
	FProviderTransferFixture Service({.NotifyContentMutation = [&](FContentChangeBatch) { ++Updates; }, .CanMutateContent = [&] { return bAllowed; }});
	Observe(Service);
	std::string Reason;
	EXPECT_FALSE(Service.CanRenameItem(Item, "../invalid", &Reason));
	EXPECT_FALSE(Reason.empty());
	EXPECT_TRUE(Service.CanRenameItem(Item, "renamed.txt", &Reason));
	EXPECT_TRUE(Reason.empty());
	EXPECT_TRUE(OperationError.empty());
	EXPECT_TRUE(OperationWarning.empty());
	EXPECT_EQ(Updates, 0);
	EXPECT_TRUE(std::filesystem::exists(File));
	bAllowed = false;
	EXPECT_FALSE(Service.RenameItem(Item, "renamed.txt", NewItem));
	EXPECT_FALSE(OperationError.empty());
	EXPECT_TRUE(TestPhysicalPath(NewItem).empty());
	EXPECT_EQ(Updates, 0);
	EXPECT_TRUE(std::filesystem::exists(File));
}

TEST_F(FContentBrowserModelTests, FixUpPublishesCommittedChangesAndFallsBackForPendingProjection)
{
	int Updates = 0;
	FProviderAssetActionFixture Service({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); ++Updates; }});
	Observe(Service);
	EXPECT_TRUE(Service.FixUpRedirectorsInFolder("/ContentBrowserTests/A/"));
	EXPECT_EQ(Updates, 0);
	EXPECT_TRUE(OperationError.empty());

	FPackagePath OldPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/FixUpSource", OldPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(OldPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const FAssetRenameData Rename{TWeakObjectPtr<DObject>(Material), "/ContentBrowserTests/B", "FixUpTarget"};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&Rename, 1}));
	Material->MarkPackageDirty();
	{
		Testing::FScopedPackageSaveFault Fault([](auto Point, size_t) { return Point == Testing::EPackageSaveFaultPoint::PublishPackage; });
		// The Browser return value means the request was handled, as the UE fix-up entry is void.
		EXPECT_TRUE(Service.FixUpRedirectorsInFolder("/ContentBrowserTests/A/"));
	}
	EXPECT_FALSE(OperationError.empty());
	EXPECT_EQ(Updates, 0);
	EXPECT_NE(FindPackageExact(OldPath), nullptr);
	OperationError.clear();
	ASSERT_TRUE(Service.FixUpRedirectorsInFolder("/ContentBrowserTests/A/"));
	EXPECT_TRUE(OperationError.empty()) << OperationError;
	EXPECT_EQ(Updates, 1);
	EXPECT_FALSE(ObservedChanges.bFullRefresh);
	EXPECT_TRUE(std::ranges::any_of(ObservedChanges.Changes, [&](const auto& Change) {
		return Change.Kind == EContentChangeKind::Removed && Change.OldAssetPath == OldPath.GetView();
	}));
	EXPECT_TRUE(std::ranges::all_of(ObservedChanges.Changes, [&](const auto& Change) {
		return Change.OldAssetPath == OldPath.GetView()
			|| Change.NewAssetPath == Material->GetPackage()->GetPackagePath();
	}));
	EXPECT_EQ(FindPackageExact(OldPath), nullptr);
	EXPECT_TRUE(Service.FixUpRedirectorsInFolder("/ContentBrowserTests/A/"));
	EXPECT_EQ(Updates, 1);

	const auto IntermediatePath = Material->GetPackage()->GetPackagePathIdentity();
	const FAssetRenameData RenameAgain{TWeakObjectPtr<DObject>(Material), "/ContentBrowserTests/A", "FixUpFinal"};
	ASSERT_TRUE(IAssetTools::Get().RenameAssets(std::span{&RenameAgain, 1}));
	SetAssetRedirectorFixupFailurePointForTesting(EAssetRedirectorFixupFailurePoint::PublishRegistry);
	struct FResetFault { ~FResetFault() { SetAssetRedirectorFixupFailurePointForTesting(EAssetRedirectorFixupFailurePoint::None); } } ResetFault;
	EXPECT_TRUE(Service.FixUpRedirectorsInFolder("/ContentBrowserTests/B/"));
	EXPECT_EQ(Updates, 2);
	EXPECT_TRUE(ObservedChanges.bFullRefresh);
	EXPECT_FALSE(OperationError.empty());
	EXPECT_TRUE(IsAssetRegistryProjectionFenced(IntermediatePath));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
}

TEST_F(FContentBrowserModelTests, DataSourceShutdownCancelsPublicationAndRejectsNewCaptures)
{
	const auto Directory = Root / "Content/A";
	std::filesystem::create_directory(Directory / "Child");
	auto Gate = std::make_shared<FBrowserScanGate>();
	FProviderQueryFixture Source;
	FContentBrowserModel Model(Source);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical(Directory.generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Requests = Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model);
	Source.StopRequestAdmission();
	EXPECT_FALSE(Source.PrepareItemsCapture("/ContentBrowserTests/", false));
	Model.RefreshItemsSnapshot();
	Model.RequestDirectoryChildrenSnapshot(Directory.generic_string());
	Gate->Release();
	FContentBrowserModelFixtureAccess::Wait(Model);
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	EXPECT_EQ(nullptr, Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(Model));
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(Directory.generic_string()));
	FContentBrowserModel ClosedModel(Source);
	ClosedModel.RefreshItemsSnapshot();
	FContentBrowserModelFixtureAccess::Wait(ClosedModel);
	EXPECT_EQ(Durin::ContentBrowser::Private::FContentBrowserModelFixtureAccess::Request(ClosedModel), nullptr);
}

TEST_F(FContentBrowserModelTests, FolderValidationIsSilentAndFinalizationRechecksDestination)
{
	int Diagnostics = 0;
	int ScopedUpdates = 0;
	int FullRefreshUpdates = 0;
	FProviderFolderFixture Source({
		.NotifyContentMutation = [&](FContentChangeBatch Batch) { ScopedUpdates += !Batch.bFullRefresh; FullRefreshUpdates += Batch.bFullRefresh; }});
	Source.SetDiagnosticSink([&](ELogLevel, std::string_view) { ++Diagnostics; });
	const auto Directory = (Root / "Content/A").generic_string();
	std::string Reason;
	EXPECT_FALSE(Source.CanCreateFolder(Directory, "../Escape", &Reason));
	EXPECT_FALSE(Reason.empty());
	ASSERT_TRUE(Source.CanCreateFolder(Directory, "Chosen", &Reason));
	EXPECT_TRUE(Reason.empty());
	EXPECT_EQ(Diagnostics, 0);
	EXPECT_EQ(ScopedUpdates, 0);
	std::filesystem::create_directory(Root / "Content/A/Chosen");
	FContentBrowserItem Item;
	EXPECT_FALSE(Source.CreateFolder(Directory, "Chosen", Item));
	EXPECT_EQ(ScopedUpdates, 0);
	ASSERT_TRUE(Source.CreateFolder(Directory, "Other", Item));
	EXPECT_EQ(ScopedUpdates, 1);
	EXPECT_EQ(FullRefreshUpdates, 0);
}

TEST_F(FContentBrowserModelTests, BulkSaveDeduplicatesItemsSharingPackage)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/SharedSave", Path));
	DMaterial* First = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, First));
	auto* Second = NewObject<DMaterial>(First->GetPackage(), "Second");
	ASSERT_NE(Second, nullptr);
	First->GetPackage()->MarkDirty();
	FContentBrowserItem Other = AssetItem(Path);
	Other.VirtualPath = Second->GetObjectPath();
	int Updates = 0;
	FProviderSaveFixture Source({.NotifyContentMutation = [&](FContentChangeBatch) { ++Updates; }});
	Observe(Source);
	ASSERT_TRUE(Source.CanSaveItem(Other));
	ASSERT_TRUE(Source.BulkSaveItems(std::array{AssetItem(Path), Other, Other})) << OperationError;
	EXPECT_EQ(Updates, 1);
	EXPECT_FALSE(First->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/SharedSave.dasset"));
	EXPECT_FALSE(Source.CanSaveItem(Other));
}

TEST_F(FContentBrowserModelTests, PreparedDiscoveryOwnsInputsAfterSourceDestruction)
{
	const auto Directory = Root / "Content/A";
	std::filesystem::create_directory(Directory / "Child");
	FContentBrowserDataQuery::FItemsCapture Work;
	FContentBrowserDataQuery::FDirectoryCapture TreeWork;
	{
		FProviderQueryFixture Source;
		Work = Source.PrepareItemsCapture("/ContentBrowserTests/A/", false, {});
		TreeWork = Source.PrepareDirectoryCapture("/ContentBrowserTests/A/", {});
		Source.StopRequestAdmission();
		EXPECT_FALSE(Source.PrepareItemsCapture("/ContentBrowserTests/A/", false, {}));
		EXPECT_FALSE(Source.PrepareDirectoryCapture("/ContentBrowserTests/A/", {}));
	}
	ASSERT_TRUE(Work);
	auto Snapshot = Work();
	ASSERT_EQ(Snapshot->Items.size(), 1u);
	EXPECT_EQ(Snapshot->Items.front().GetItemName(), "Child");
	ASSERT_TRUE(TreeWork);
	EXPECT_EQ(TreeWork()->Children.size(), 1u);
}

TEST_F(FContentBrowserModelTests, DirectoryCapturesPrepareCurrentCatalogWithoutItemRefresh)
{
	InitializeDObjectSystem();
	{
		const std::string Folder = "Tree";
		const auto Physical = Root / "Content" / Folder;
		FPackagePath FirstPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/" + Folder + "/First/Material", FirstPath));
		DMaterial* First = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(FirstPath, First));
		FProviderQueryFixture Source;
		FContentBrowserModel Model(Source);
		// No navigation or item capture has prepared a catalog yet.
		Model.RequestDirectoryChildrenSnapshot(Physical.generic_string());
		FContentBrowserModelFixtureAccess::Wait(Model);
		auto Tree = Model.GetDirectorySnapshot(Physical.generic_string());
		ASSERT_NE(Tree, nullptr);
		ASSERT_EQ(Tree->Children.size(), 1u);
		EXPECT_EQ(Tree->Children.front(), Physical / "First");

		ASSERT_TRUE(Model.NavigateToPhysical(Physical.generic_string()));
		FContentBrowserModelFixtureAccess::Wait(Model);
		FPackagePath LaterPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/" + Folder + "/Later/Material", LaterPath));
		DMaterial* Later = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(LaterPath, Later));
		// The item catalog predates Later; tree preparation must refresh its owned inputs.
		Model.RequestDirectoryChildrenSnapshot(Physical.generic_string());
		FContentBrowserModelFixtureAccess::Wait(Model);
		Tree = Model.GetDirectorySnapshot(Physical.generic_string());
		ASSERT_NE(Tree, nullptr);
		EXPECT_EQ(Tree->Children.size(), 2u);
		EXPECT_TRUE(std::ranges::contains(Tree->Children, Physical / "Later"));
		EXPECT_FALSE(std::filesystem::exists(Physical));
	}
}

TEST_F(FContentBrowserModelTests, SaveCapabilitiesFollowFlagsAndCurrentMountPolicy)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/SavePolicy", Path));
	DMaterial* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->GetPackage()->MarkDirty();
	const auto Item = AssetItem(Path);
	FProviderSaveFixture Source;
	Observe(Source);
	EXPECT_TRUE(Source.CanSaveItem(Item));
	EXPECT_TRUE(Source.CanSaveItem(Item, true));
	ASSERT_TRUE(Source.BulkSaveItems(std::span(&Item, 1)));
	EXPECT_FALSE(Source.CanSaveItem(Item));
	EXPECT_TRUE(Source.CanSaveItem(Item, true));
	Asset->GetPackage()->MarkDirty();
	Registry.reset();
	const std::array Definitions{FMountPoint{.VirtualRoot = "/ContentBrowserTests/",
		.Owner = EMountOwner::Test, .Root = Root / "Content", .bAutoScan = true, .bContentWritable = false}};
	Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(Definitions);
	ASSERT_TRUE(Registry->IsValid());
	std::string Reason;
	EXPECT_FALSE(Source.CanSaveItem(Item, false, &Reason));
	EXPECT_FALSE(Reason.empty());
	EXPECT_FALSE(Source.CanSaveItem(Item, true));
	EXPECT_TRUE(OperationError.empty());
	EXPECT_FALSE(Source.BulkSaveItems(std::span(&Item, 1)));
	EXPECT_TRUE(Asset->GetPackage()->IsDirty());
}

TEST_F(FContentBrowserModelTests, NonphysicalProviderUsesNormalListTreeNavigationSearchAndCommands)
{
	FProviderQueryFixture Query;
	auto *Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
					{.Kind = EContentBrowserItemKind::Folder, .Name = "Sub", .VirtualPath = "/Memory/Sub"},
					{.Name = "Leaf", .VirtualPath = "/Memory/Sub/Leaf"}});
	Query.AddSource(*Source);
	FContentBrowserModel First(Query), Second(Query);
	ASSERT_TRUE(First.NavigateToVirtual("/Memory"));
	ASSERT_TRUE(Second.NavigateToVirtual("/Memory/Sub"));
	First.SetSearch("Leaf");
	First.CancelPendingSnapshots();
	FContentBrowserModelFixtureAccess::Wait(Second);
	ASSERT_EQ(Second.GetItems().size(), 1u);
	EXPECT_TRUE(TestPhysicalPath(Second.GetItems().front()).empty());
	EXPECT_EQ(ContentBrowserQuery::TypeLabel(Second.GetItems().front()), "Memory object");
	EXPECT_TRUE(Second.GetItems().front().Copy("/Memory/Target"));
	EXPECT_EQ(Source->Copies, 1);
	First.SetSearch("Leaf");
	First.RefreshItemsSnapshot();
	FContentBrowserModelFixtureAccess::Wait(First);
	ASSERT_EQ(First.GetItems().size(), 1u);
	EXPECT_EQ(TestVirtualPath(First.GetItems().front()), "/Memory/Sub/Leaf");
	First.RequestDirectoryChildrenSnapshot("/");
	First.RequestDirectoryChildrenSnapshot("/Memory");
	FContentBrowserModelFixtureAccess::Wait(First);
	const auto Roots = First.GetDirectorySnapshot("/");
	ASSERT_NE(Roots, nullptr);
	EXPECT_TRUE(std::ranges::contains(Roots->Children, std::filesystem::path("/Memory")));
	const auto Tree = First.GetDirectorySnapshot("/Memory");
	ASSERT_NE(Tree, nullptr);
	ASSERT_EQ(Tree->Children.size(), 1u);
	EXPECT_EQ(Tree->Children.front(), std::filesystem::path("/Memory/Sub"));
}

TEST_F(FContentBrowserModelTests, PreparedProviderCapturesRejectExplicitRefresh)
{
	FProviderQueryFixture Query;
	auto *Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
					{.Name = "Leaf", .VirtualPath = "/Memory/Leaf"}});
	Query.AddSource(*Source);
	auto Work = Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Work);
	Source->NotifyItemDataRefreshed();
	Query.GetServiceForTesting()->Tick(0.0f);
	auto Old = Query.ProjectItems(std::move(*Work()), "/Memory", false);
	EXPECT_TRUE(Old.bStale);
	EXPECT_TRUE(Old.Items.empty());
	auto TreeWork = Query.PrepareDirectoryCapture("/Memory");
	ASSERT_TRUE(TreeWork);
	Source->Rows.push_back(Durin::ContentBrowser::Private::FStorageItemFixture{.Kind = EContentBrowserItemKind::Folder, .Name = "Later", .VirtualPath = "/Memory/Later"});
	Source->NotifyItemDataRefreshed();
	Query.GetServiceForTesting()->Tick(0.0f);
	auto OldTree = Query.ProjectDirectory(std::move(*TreeWork()), "/Memory");
	EXPECT_TRUE(OldTree.bStale);
	EXPECT_TRUE(OldTree.Children.empty());
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToVirtual("/Memory"));
	Model.RequestDirectoryChildrenSnapshot("/Memory");
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(Model.GetItems().size(), 2u);
	const auto Tree = Model.GetDirectorySnapshot("/Memory");
	ASSERT_NE(Tree, nullptr);
	ASSERT_EQ(Tree->Children.size(), 1u);
}

TEST_F(FContentBrowserModelTests, MountRefreshCancelsPreparedCaptures)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->Rows = ItemDescriptions({{.Name = "Leaf", .VirtualPath = "/Memory/Leaf"}});
	Query.AddSource(*Source);
	auto Work = Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Work);
	{ Testing::FScopedMountRegistryFixture Temporary; }
	Query.GetServiceForTesting()->Tick(0.0f);
	auto Old = Query.ProjectItems(std::move(*Work()), "/Memory", false);
	EXPECT_TRUE(Old.bStale); EXPECT_TRUE(Old.Items.empty());
	auto Fresh = Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Fresh);
	EXPECT_EQ(1u, Query.ProjectItems(std::move(*Fresh()), "/Memory", false).Items.size());
}

TEST_F(FContentBrowserModelTests, TwoServiceQueriesShareProviderDataWithoutSharingAdmissionOrCancellation)
{
	FModuleManager::Get().LoadModuleChecked("ContentBrowserData");
	FModuleManager::Get().LoadModuleChecked("ContentBrowser");
	struct FEditorScope
	{
		TStrongObjectPtr<DEditorEngine> Editor;
		TStrongObjectPtr<FVirtualBrowserQuerySource> Source;
		~FEditorScope()
		{
			if (Source.Get())
				Source->Shutdown();
			if (Editor.Get())
				Editor->PreExit();
		}
	} Scope;
	Scope.Editor = TStrongObjectPtr<DEditorEngine>(NewObject<DEditorEngine>(nullptr, {}));
	FEditorEngineTestAccess::StartEditor(*Scope.Editor);
	auto *Data = Scope.Editor->GetEditorSubsystem<DContentBrowserDataSubsystem>();
	ASSERT_NE(Data, nullptr);
	Scope.Source = TStrongObjectPtr<FVirtualBrowserQuerySource>(NewObject<FVirtualBrowserQuerySource>(Scope.Editor.Get(), {}));
	Scope.Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
						  {.Name = "Leaf", .VirtualPath = "/Memory/Leaf"}});
	Scope.Source->Initialize();
	ASSERT_TRUE(Data->ActivateDataSource(FName("Memory")));
	FContentBrowserDataQuery FirstQuery(Data), SecondQuery(Data);
	FContentBrowserModel First(FirstQuery), Second(SecondQuery);
	ASSERT_TRUE(First.NavigateToVirtual("/Memory"));
	ASSERT_TRUE(Second.NavigateToVirtual("/Memory"));
	FirstQuery.StopRequestAdmission();
	FContentBrowserModelFixtureAccess::Wait(First);
	FContentBrowserModelFixtureAccess::Wait(Second);
	EXPECT_TRUE(First.GetItems().empty());
	ASSERT_EQ(Second.GetItems().size(), 1u);
	EXPECT_EQ(TestVirtualPath(Second.GetItems().front()), "/Memory/Leaf");
	EXPECT_TRUE(Second.GetItems().front().Copy("/Memory/Target"));
	EXPECT_EQ(Scope.Source->Copies, 1);
}

TEST_F(FContentBrowserModelTests, SynchronousEnumerationRejectsWithdrawalDuringPolicyBinding)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->Rows = Durin::ContentBrowser::Private::ItemDescriptions({{.Name="Leaf", .VirtualPath="/Memory/Leaf"}});
	Query.AddSource(*Source);
	// The first binding is provider enumeration; the second attaches caller policy.
	Source->BeforeBind = [&] {
		Source->BeforeBind = [&] { Source->UnregisterDataSource(); };
	};
	std::vector<Durin::ContentBrowser::FContentBrowserItem> Items;
	auto Collect = [&](Durin::ContentBrowser::FContentBrowserItem&& Item) { Items.push_back(std::move(Item)); return true; };
	Query.GetServiceForTesting()->EnumerateItemsUnderPath("/Memory", {}, Collect);
	EXPECT_TRUE(Items.empty());
	Query.AddSource(*Source);
	Query.GetServiceForTesting()->EnumerateItemsUnderPath("/Memory", {}, Collect);
	ASSERT_EQ(1u, Items.size());
	EXPECT_EQ("/Memory/Leaf", Items.front().GetVirtualPath());
}

TEST_F(FContentBrowserModelTests, OwnerBindingRejectsWithdrawalDuringPublication)
{
	FProviderQueryFixture Query;
	auto* Source=NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->Rows=Durin::ContentBrowser::Private::ItemDescriptions({{.Name="Leaf", .VirtualPath="/Memory/Leaf"}});
	Source->UseCompiledCapture=true;
	Query.AddSource(*Source);
	auto Work=Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Work);
	Source->BeforeBind=[&] { Source->UnregisterDataSource(); };
	auto Old=Query.ProjectItems(std::move(*Work()), "/Memory", false);
	EXPECT_TRUE(Old.bStale);
	EXPECT_TRUE(Old.Items.empty());
	Query.AddSource(*Source);
	auto Fresh=Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Fresh);
	EXPECT_EQ(Query.ProjectItems(std::move(*Fresh()), "/Memory", false).Items.size(), 1u);
}

TEST_F(FContentBrowserModelTests, SourceImagePreviewUsesProviderAttributesRatherThanLegacyMetadataFields)
{
	FProviderQueryFixture Query;
	auto* Source=NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->PreviewPath=(Root/"Preview.png").generic_string(); Query.AddSource(*Source);
	Durin::ContentBrowser::FContentBrowserItem Item;
	Item.InternalItems.emplace_back(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/Memory/Preview", std::string{}, std::string{}, nullptr);
	EXPECT_EQ(TestPhysicalPath(Item), Source->PreviewPath); EXPECT_EQ(TestExtension(Item), ".png");
	FContentBrowserThumbnailReferences References; References.BeginFrame();
	References.Request(Item, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.Find(Item.GetItemKey()).State, EAssetThumbnailState::Pending);
}

TEST_F(FContentBrowserModelTests, DeleteRetainsIndependentCommitsAndRetriesOnlySurvivors)
{
	const std::array Paths{Root / "Content/delete-a.txt", Root / "Content/delete-b.txt", Root / "Content/delete-c.txt"};
	std::vector<FContentBrowserItem> Items;
	for (const auto& Path : Paths) { std::ofstream(Path) << "confirmed bytes"; Items.push_back({.Kind = EContentBrowserItemKind::File, .PhysicalPath = Path.generic_string()}); }
	FDeletionRequestFixture Operation;
	auto Plan = Operation.PrepareDeleteItems(Items);
	ASSERT_TRUE(Plan && Plan->CanExecute());
	const auto Result = Operation.DeleteItems(Plan, {.RemoveEntry = [&](const auto& Path) {
		if (Path == Paths[1]) return std::make_error_code(std::errc::permission_denied);
		std::error_code Error; std::filesystem::remove(Path, Error); return Error;
	}});
	EXPECT_EQ(ContentStorage::EFileDeletionStatus::Partial, Result.GetStatus());
	EXPECT_FALSE(Result.bStopped);
	EXPECT_FALSE(Plan);
	EXPECT_FALSE(std::filesystem::exists(Paths[0]));
	EXPECT_TRUE(std::filesystem::exists(Paths[1]));
	EXPECT_FALSE(std::filesystem::exists(Paths[2]));
	EXPECT_FALSE(Operation.DeleteItems(Plan).IsComplete());
	auto Retry = Operation.PrepareDeleteItems(std::span{&Items[1], 1});
	EXPECT_TRUE(Operation.DeleteItems(Retry).IsComplete());
	EXPECT_FALSE(std::filesystem::exists(Paths[1]));
}


TEST_F(FContentBrowserModelTests, DeletionPreservesObservedEffectsAndRejectsFalseCompletion)
{
	InitializeDObjectSystem();
	for (int Case = 0; Case < 3; ++Case)
	{
		const auto Effect = Case == 2 ? EAssetWriteEffect::ContentUncertain : EAssetWriteEffect::None;
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/ContentBrowserTests/DeleteObserved{}", Case), Path));
		DMaterial* Material = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		FAssetDeletionOperation Operation;
		ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
		const auto File = TestPhysicalPath(Operation.GetEntries().front().RegistryEntry);
		EXPECT_FALSE(Operation.Delete({.Delete = [Effect, Case](auto&) -> FAssetWriteResult {
			if (Case == 1) return {};
			return {.Error = Durin::UnifiedError::FError{{"TestDeletion", "Rejected"}}, .Effect = Effect};
		}}));
		const auto& Report = Operation.GetWriteResult();
		ASSERT_TRUE(Report.Error);
		EXPECT_TRUE(Case == 1 ? Report.Error->Is(AssetWriteErrors::StaleData) : Report.Error->Is({"TestDeletion", "Rejected"}));
		EXPECT_EQ(Report.Effect, Effect);
		EXPECT_TRUE(Operation.GetAffectedFiles().empty());
		EXPECT_TRUE(std::filesystem::exists(File));
		EXPECT_EQ(IsAssetRegistryProjectionFenced(Path), Effect != EAssetWriteEffect::None);
	}
}

TEST_F(FContentBrowserModelTests, StoppedDeletionRetainsFirstNativeFailureAndUnattemptedFiles)
{
	const std::array Paths{Root / "Content/failure-a.txt", Root / "Content/failure-b.txt", Root / "Content/failure-c.txt"};
	std::vector<FContentBrowserItem> Items;
	for (const auto& Path : Paths) { std::ofstream(Path) << "confirmed"; Items.push_back({.Kind = EContentBrowserItemKind::File, .PhysicalPath = Path.generic_string()}); }
	FDeletionRequestFixture Operation;
	std::vector<std::string> Diagnostics;
	Operation.SetDiagnosticSink([&](ELogLevel, std::string_view Message) { Diagnostics.emplace_back(Message); });
	auto Plan = Operation.PrepareDeleteItems(Items);
	ASSERT_TRUE(Plan && Plan->CanExecute());
	bool Attempted = false;
	const auto Result = Operation.DeleteItems(Plan, {.RemoveEntry = [&](const auto&) {
		Attempted = true;
		return std::make_error_code(std::errc::permission_denied);
	}, .CanExecuteEffect = [&] { return !Attempted; }});
	EXPECT_TRUE(Result.bStopped);
	EXPECT_FALSE(Result.bCancelled);
	EXPECT_FALSE(Result.HasCommittedEffects());
	ASSERT_EQ(Result.Effects.size(), 3u);
	EXPECT_EQ(Result.Effects[0].Status, ContentStorage::EFileDeletionEffectStatus::Failed);
	EXPECT_EQ(Result.Effects[1].Status, ContentStorage::EFileDeletionEffectStatus::Unattempted);
	EXPECT_EQ(Result.Effects[2].Status, ContentStorage::EFileDeletionEffectStatus::Unattempted);
	EXPECT_TRUE(std::ranges::any_of(Diagnostics, [&](const auto& Text) { return Text.contains("File::IoError") && Text.contains(Paths[0].generic_string()) && Text.contains("remove"); }));
	EXPECT_FALSE(std::ranges::any_of(Diagnostics, [](const auto& Text) { return Text.contains("AssetWrite::Cancelled"); }));
	for (const auto& Path : Paths) EXPECT_TRUE(std::filesystem::exists(Path));
}

TEST_F(FContentBrowserModelTests, CancellationBeforeFirstDeletionLeavesEveryFileUnattempted)
{
	const auto Path = Root / "Content/cancel-before.txt";
	std::ofstream(Path) << "confirmed";
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File, .PhysicalPath = Path.generic_string()};
	FDeletionRequestFixture Operation;
	auto Plan = Operation.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan && Plan->CanExecute());
	int Removes = 0;
	const auto Result = Operation.DeleteItems(Plan, {.RemoveEntry = [&](const auto&) { ++Removes; return std::error_code{}; }, .ShouldCancel = [] { return true; }});
	EXPECT_TRUE(Result.bCancelled);
	EXPECT_FALSE(Result.HasCommittedEffects());
	EXPECT_EQ(Removes, 0);
	EXPECT_TRUE(std::filesystem::exists(Path));
	ASSERT_EQ(Result.Effects.size(), 1u);
	EXPECT_EQ(Result.Effects[0].Status, ContentStorage::EFileDeletionEffectStatus::Unattempted);
}
TEST_F(FContentBrowserModelTests, DeleteCancellationRetainsCommitAndMarksRemainderUnattempted)
{
	const std::array Paths{Root / "Content/cancel-a.txt", Root / "Content/cancel-b.txt", Root / "Content/cancel-c.txt"};
	std::vector<FContentBrowserItem> Items;
	for (const auto& Path : Paths) { std::ofstream(Path) << "confirmed bytes"; Items.push_back({.Kind = EContentBrowserItemKind::File, .PhysicalPath = Path.generic_string()}); }
	FDeletionRequestFixture Operation;
	auto Plan = Operation.PrepareDeleteItems(Items);
	bool Cancelled = false;
	const auto Result = Operation.DeleteItems(Plan, {.RemoveEntry = [&](const auto& Path) {
		std::error_code Error; std::filesystem::remove(Path, Error); Cancelled = true; return Error;
	}, .ShouldCancel = [&] { return Cancelled; }});
	EXPECT_TRUE(Result.bCancelled);
	EXPECT_TRUE(Result.HasCommittedEffects());
	EXPECT_EQ(ContentStorage::EFileDeletionStatus::Cancelled, Result.GetStatus());
	EXPECT_FALSE(Plan);
	EXPECT_FALSE(std::filesystem::exists(Paths[0]));
	EXPECT_TRUE(std::filesystem::exists(Paths[1]));
	EXPECT_TRUE(std::filesystem::exists(Paths[2]));
}

TEST_F(FContentBrowserModelTests, RejectedFileValidationNeedsNoDiagnosticAndNeverDeletes)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/NoDiagnosticValidation", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto File = TestPhysicalPath(*FindPackageExact(Path));
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(PrepareAssetDeletionWithEditorPolicy({.AssetPaths = {Path}}, Operation));
	uint32 Deletes = 0;
	EXPECT_FALSE(Operation.Delete({
		.Delete = [&](auto&) -> FAssetWriteResult { ++Deletes; return {}; },
		.ValidateFiles = [](FAssetDeletionFileIdentities&, std::optional<UnifiedError::FError>*) { return false; }}));
	EXPECT_EQ(Deletes, 0u);
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_TRUE(Operation.GetWriteResult().Error.has_value());
}

TEST_F(FContentBrowserModelTests, DeletionReportsEachFailureAndKeepsConfirmationPreparedFromFeedback)
{
	const std::array Paths{Root / "Content/delete-a.txt", Root / "Content/delete-b.txt", Root / "Content/delete-c.txt"};
	std::vector<FContentBrowserItem> Items;
	for (const auto& Path : Paths) { std::ofstream(Path) << "bytes"; Items.push_back({.Kind = EContentBrowserItemKind::File, .PhysicalPath = Path.generic_string()}); }
	std::vector<std::string> Errors;
	int Notifications = 0;
	FDeletionRequestFixture Operation({.NotifyContentMutation = [&](FContentChangeBatch Batch) {
		++Notifications;
		EXPECT_EQ(1u, Batch.Changes.size());
	}});
	ContentStorage::FFileDeletionPlanPtr Next;
	Operation.SetDiagnosticSink([&](ELogLevel Level, std::string_view Text) {
		if (Level < ELogLevel::Error) return;
		Errors.emplace_back(Text);
		if (!Next) Next = Operation.PrepareDeleteItems(std::span{Items.data(), 1});
	});
	auto Plan = Operation.PrepareDeleteItems(Items);
	ASSERT_TRUE(Plan && Plan->CanExecute());
	EXPECT_TRUE(Operation.DeleteItems(Plan, {.RemoveEntry = [&](const auto& Path) {
		if (Path != Paths[2]) return std::make_error_code(std::errc::permission_denied);
		std::error_code Error; std::filesystem::remove(Path, Error); return Error;
	}}).HasCommittedEffects());
	EXPECT_FALSE(Plan);
	ASSERT_EQ(3u, Errors.size());
	EXPECT_NE(std::string::npos, Errors[0].find(Paths[0].generic_string()));
	EXPECT_NE(std::string::npos, Errors[1].find(Paths[1].generic_string()));
	EXPECT_TRUE(Errors[2].contains("File::IoError"));
	EXPECT_TRUE(Errors[2].contains(Paths[0].generic_string()));
	EXPECT_TRUE(Errors[2].contains(Paths[1].generic_string()));
	EXPECT_EQ(1, Notifications);
	EXPECT_TRUE(std::filesystem::exists(Paths[0]));
	EXPECT_TRUE(std::filesystem::exists(Paths[1]));
	EXPECT_FALSE(std::filesystem::exists(Paths[2]));
	ASSERT_TRUE(Next && Next->CanExecute());
	EXPECT_TRUE(Operation.DeleteItems(Next).HasCommittedEffects());
	EXPECT_FALSE(Next);
	EXPECT_FALSE(std::filesystem::exists(Paths[0]));
	EXPECT_EQ(2, Notifications);
}

TEST_F(FContentBrowserModelTests, DuplicateDoesNotSaveAndPublishesDirtyOutput)
{
	FPackagePath Path, Copy;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DuplicateSaveFailure", Path));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DuplicateSaveFailure1", Copy));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	int Notifications = 0, Errors = 0, SaveAttempts = 0;
	FContentChangeBatch Published;
	FProviderAssetActionFixture Operation({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ++Notifications; Published = std::move(Batch); }});
	Operation.SetDiagnosticSink([&](ELogLevel Level, std::string_view Text) { if (Level >= ELogLevel::Error && !Text.empty()) ++Errors; });
	FContentBrowserItem Output;
	{
		Testing::FScopedPackageSaveFault Fault([&](auto Phase, size_t) { ++SaveAttempts; return Phase == Testing::EPackageSaveFaultPoint::PublishPackage; });
		EXPECT_TRUE(Operation.DuplicateItem(AssetItem(Path), Output));
	}
	EXPECT_EQ(Testing::MakePackageLeafTopLevelAssetPathForTests(Copy).ToString(), TestVirtualPath(Output));
	ASSERT_NE(FindResidentPackage(Copy), nullptr);
	EXPECT_TRUE(FindResidentPackage(Copy)->IsDirty());
	EXPECT_EQ(0, Errors);
	EXPECT_EQ(0, SaveAttempts);
	EXPECT_EQ(1, Notifications);
	EXPECT_FALSE(Published.bFullRefresh);
	ASSERT_EQ(1u, Published.Changes.size());
	EXPECT_EQ(TestVirtualPath(Output), Published.Changes.front().NewAssetPath);
}

TEST_F(FContentBrowserModelTests, SaveFeedbackReentryPublishesOnlyEachOperationsOwnChanges)
{
	FPackagePath A, C, Missing;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/NestedSaveA", A));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/NestedSaveC", C));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/NestedSaveZMissing", Missing));
	DMaterial *AssetA = nullptr, *AssetC = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(A, AssetA));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(C, AssetC));
	int InnerNotifications = 0, OuterNotifications = 0;
	FContentChangeBatch InnerChanges, OuterChanges;
	FProviderSaveFixture Inner({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ++InnerNotifications; InnerChanges = std::move(Batch); }});
	FProviderSaveFixture Outer({.NotifyContentMutation = [&](FContentChangeBatch Batch) { ++OuterNotifications; OuterChanges = std::move(Batch); }});
	bool Nested = false;
	Outer.SetDiagnosticSink([&](ELogLevel Level, std::string_view) {
		if (Level >= ELogLevel::Error && !Nested) { Nested = true; EXPECT_TRUE(Inner.BulkSaveItems(std::array{AssetItem(C)})); }
	});
	EXPECT_TRUE(Outer.BulkSaveItems(std::array{AssetItem(A), AssetItem(Missing)}));
	ASSERT_TRUE(Nested);
	EXPECT_EQ(1, InnerNotifications); EXPECT_EQ(1, OuterNotifications);
	ASSERT_EQ(1u, OuterChanges.Changes.size()); ASSERT_EQ(1u, InnerChanges.Changes.size());
	EXPECT_EQ(A.ToString(), OuterChanges.Changes.front().NewAssetPath);
	EXPECT_EQ(C.ToString(), InnerChanges.Changes.front().NewAssetPath);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/NestedSaveA.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/NestedSaveC.dasset"));
}

TEST_F(FContentBrowserModelTests, DuplicateRetainsCreatedOutputAcrossNotificationWithdrawal)
{
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DuplicateReactivation", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FProviderQueryFixture Query;
	auto* Source = Query.FindSourceForTesting(FName("Asset"));
	ASSERT_NE(Source, nullptr);
	auto Description = AssetItem(Path);
	Description.PhysicalPath = TestPhysicalPath(*FindPackageExact(Path));
	bool Reactivated = false;
	const auto Bound = Query.BindContributionsForTesting(Description, {.NotifyContentMutation = [&](FContentChangeBatch) {
		if (!Reactivated)
		{
			Reactivated = true;
			EXPECT_TRUE(Query.GetServiceForTesting()->DeactivateDataSource(FName("Asset")));
		}
	}});
	ASSERT_EQ(1u, Bound.InternalItems.size());
	ContentBrowser::FContentBrowserItemData Output;
	EXPECT_TRUE(Source->DuplicateItem(Bound.InternalItems.front(), Output));
	EXPECT_TRUE(Reactivated);
	EXPECT_FALSE(Source->IsActive());
	EXPECT_EQ(Source, Bound.InternalItems.front().GetOwnerDataSource());
	EXPECT_EQ(Source, Output.GetOwnerDataSource());
}

TEST_F(FContentBrowserModelTests, BindingRefreshKeepsTheReentrantTaskOwnedAndRejectsOldRows)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
		{.Name = "Old", .VirtualPath = "/Memory/Old"}});
	Query.AddSource(*Source);
	Query.GetServiceForTesting()->Tick(0.0f);
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToVirtual("/Memory"));
	bool Reentered = false;
	Source->BeforeBind = [&] {
		Reentered = true;
		Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
			{.Name = "New", .VirtualPath = "/Memory/New"}});
		Model.SetSearch("New");
	};
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Model.IsLoading() && std::chrono::steady_clock::now() < Deadline)
	{
		Model.PumpPendingSnapshots();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(Reentered);
	ASSERT_FALSE(Model.IsLoading());
	ASSERT_EQ(1u, Model.GetItems().size());
	EXPECT_EQ("/Memory/New", TestVirtualPath(Model.GetItems().front()));
}

TEST_F(FContentBrowserModelTests, BindingCancellationRejectsTheCaptureBeforePublication)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Name = "Leaf", .VirtualPath = "/Memory/Leaf"}});
	Query.AddSource(*Source);
	auto Token = std::make_shared<Tasks::FCancellationToken>();
	auto Work = Query.PrepareItemsCapture("/Memory", false, {}, Token);
	ASSERT_TRUE(Work);
	Source->BeforeBind = [Token] { Token->Cancel(); };
	auto Result = Query.ProjectItems(std::move(*Work()), "/Memory", false);
	EXPECT_TRUE(Result.bStale); EXPECT_TRUE(Result.Items.empty());
}

TEST_F(FContentBrowserModelTests, OrdinaryFolderMetadataDoesNotCancelPreparedBaseline)
{
	FProviderQueryFixture Query;
	auto* Data = Query.GetServiceForTesting();
	ASSERT_TRUE(Data->DeactivateDataSource(FName("Asset")));
	ASSERT_TRUE(Data->DeactivateDataSource(FName("File")));
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Data, {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Name = "Leaf", .VirtualPath = "/Memory/Sub/Leaf"}});
	Query.AddSource(*Source);
	Data->Tick(0.0f);
	for (const auto Path : {"/Memory/", "/"})
	{
		auto Work = Query.PrepareItemsCapture("/Memory/Sub/", false);
		ASSERT_TRUE(Work);
		Source->QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(
			ContentBrowser::FContentBrowserItemData(*Source, ContentBrowser::EContentBrowserItemFlags::Type_Folder, Path, "", "", {})));
		Data->Tick(0.0f);
		auto Result = Query.ProjectItems(std::move(*Work()), "/Memory/Sub/", false);
		EXPECT_FALSE(Result.bStale) << Path;
		EXPECT_EQ(Result.Items.size(), 1u) << Path;
	}
}

TEST_F(FContentBrowserModelTests, DeltaBindingMayDestroyTheReceivingModel)
{
	FProviderQueryFixture Query;
	ASSERT_TRUE(Query.GetServiceForTesting()->DeactivateDataSource(FName("Asset")));
	ASSERT_TRUE(Query.GetServiceForTesting()->DeactivateDataSource(FName("File")));
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Query.AddSource(*Source);
	Query.GetServiceForTesting()->Tick(0.0f);
	auto Model = std::make_unique<FContentBrowserModel>(Query);
	ASSERT_TRUE(Model->NavigateToPhysical((Root / "Content").generic_string()));
	FContentBrowserModelFixtureAccess::Wait(*Model);
	bool Destroyed = false;
	Source->BeforeBind = [&] { Model.reset(); Destroyed = true; };
	Source->QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(
		ContentBrowser::FContentBrowserItemData(*Source, ContentBrowser::EContentBrowserItemFlags::Type_File, "/ContentBrowserTests/Leaf", "Leaf", "Leaf", {})));
	Query.GetServiceForTesting()->Tick(0.0f);
	Model->PumpPendingSnapshots();
	EXPECT_TRUE(Destroyed);
	EXPECT_EQ(nullptr, Model);
}

TEST_F(FContentBrowserModelTests, TreeBindingKeepsTheReentrantTreeTaskOwned)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
		{.Kind = EContentBrowserItemKind::Folder, .Name = "Sub", .VirtualPath = "/Memory/Sub"}});
	Query.AddSource(*Source);
	Query.GetServiceForTesting()->Tick(0.0f);
	FContentBrowserModel Model(Query);
	Model.RequestDirectoryChildrenSnapshot("/Memory");
	Model.PumpPendingSnapshots();
	bool Reentered = false;
	Source->BeforeBind = [&] {
		Reentered = true;
		Model.RequestDirectoryChildrenSnapshot("/Memory/Sub");
		Model.PumpPendingSnapshots();
	};
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Model.HasDirectoryChildrenSnapshot("/Memory/Sub") && std::chrono::steady_clock::now() < Deadline)
	{
		Model.PumpPendingSnapshots();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_TRUE(Reentered);
	EXPECT_TRUE(Model.HasDirectoryChildrenSnapshot("/Memory/Sub"));
}

TEST_F(FContentBrowserModelTests, ProviderPreparationMayDestroyQueryDuringCompileOrEnumeration)
{
	FProviderQueryFixture Fixture;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Fixture.GetServiceForTesting(), {});
	Source->Rows = ItemDescriptions({{.Name = "Leaf", .VirtualPath = "/Memory/Leaf"}});
	Fixture.AddSource(*Source);
	for (bool Compiled : {false, true})
	{
		auto Query = std::make_unique<FContentBrowserDataQuery>(Fixture.GetServiceForTesting());
		Source->UseCompiledCapture = Compiled;
		if (Compiled) Source->BeforeCompile = [&] { Query.reset(); };
		else Source->BeforeBind = [&] { Query.reset(); };
		auto* Preparing = Query.get();
		auto Work = Preparing->PrepareItemsCapture("/Memory", false);
		EXPECT_EQ(nullptr, Query);
		EXPECT_FALSE(Work);
	}
}

TEST_F(FContentBrowserModelTests, BindingPolicyRemainsOwnedAfterQueryDestruction)
{
	FProviderQueryFixture Fixture;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Fixture.GetServiceForTesting(), {});
	Source->UseCompiledCapture = true;
	Source->Rows = ItemDescriptions({{.Name = "Leaf", .VirtualPath = "/Memory/Leaf"}});
	Fixture.AddSource(*Source);
	bool Inspected = false;
	auto Query = std::make_unique<FContentBrowserDataQuery>(Fixture.GetServiceForTesting(),
		ContentBrowser::FContentBrowserOperationContext{.CanMutateContent = [&] { Inspected = true; return false; }});
	auto Work = Query->PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Work);
	Source->BeforeBind = [&] { Query.reset(); };
	Source->InspectBindingPolicy = [&](const auto& Policy) {
		ASSERT_TRUE(Policy.CanMutateContent);
		EXPECT_FALSE(Policy.CanMutateContent());
	};
	auto* Publishing = Query.get();
	auto Result = Publishing->ProjectItems(std::move(*Work()), "/Memory", false);
	EXPECT_EQ(nullptr, Query);
	EXPECT_TRUE(Inspected);
	EXPECT_TRUE(Result.bStale); EXPECT_TRUE(Result.Items.empty());
}

TEST_F(FContentBrowserModelTests, ProjectionReentryKeepsTheLatestSortAndOwnedInputs)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
		{.Name = "A", .VirtualPath = "/Memory/A"}, {.Name = "B", .VirtualPath = "/Memory/B"}});
	Query.AddSource(*Source);
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToVirtual("/Memory"));
	FContentBrowserModelFixtureAccess::Wait(Model);
	ASSERT_EQ(2u, Model.GetItems().size());
	bool Reentered = false;
	Source->BeforePhysicalPath = [&] { Reentered = true; Model.SetSort(EContentBrowserSortColumn::Type, false); };
	Model.RebuildItems();
	EXPECT_TRUE(Reentered);
	ASSERT_EQ(2u, Model.GetItems().size());
	EXPECT_EQ("B", TestName(Model.GetItems().front()));
}

TEST_F(FContentBrowserModelTests, ProjectionProviderCallbackMayDestroyModel)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
		{.Name = "A", .VirtualPath = "/Memory/A"}, {.Name = "B", .VirtualPath = "/Memory/B"}});
	Query.AddSource(*Source);
	auto Model = std::make_unique<FContentBrowserModel>(Query);
	ASSERT_TRUE(Model->NavigateToVirtual("/Memory"));
	FContentBrowserModelFixtureAccess::Wait(*Model);
	Source->BeforePhysicalPath = [&] { Model.reset(); };
	auto* Projecting = Model.get();
	Projecting->RebuildItems();
	EXPECT_EQ(nullptr, Model);
}

TEST_F(FContentBrowserModelTests, SortingPrefetchesProviderAttributesBeforeComparingRows)
{
	FProviderQueryFixture Query;
	auto* Source = NewObject<FVirtualBrowserQuerySource>(Query.GetServiceForTesting(), {});
	Source->PreviewPath = "/Preview.png";
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
		{.Name = "A", .VirtualPath = "/Memory/A"}, {.Name = "B", .VirtualPath = "/Memory/B"}});
	Query.AddSource(*Source);
	FContentBrowserModel Model(Query);
	ASSERT_TRUE(Model.NavigateToVirtual("/Memory"));
	FContentBrowserModelFixtureAccess::Wait(Model);
	Source->DiskSizeReads = 0;
	Model.SetSort(EContentBrowserSortColumn::Size, true);
	EXPECT_EQ(2, Source->DiskSizeReads);
	ASSERT_EQ(2u, Model.GetItems().size());
	EXPECT_EQ("A", TestName(Model.GetItems().front()));
}

TEST_F(FContentBrowserModelTests, UnsavedAssetCapabilitiesAndRenameUseLiveIdentity)
{
	auto* Object = IAssetTools::Get().CreateAsset("UnsavedRename", "/ContentBrowserTests", DMaterial::StaticClass(), nullptr);
	ASSERT_NE(Object, nullptr);
	FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset, .Name = "UnsavedRename",
		.VirtualPath = "/ContentBrowserTests/UnsavedRename.UnsavedRename", .PackagePath = Object->GetPackage()->GetPackagePathIdentity(),
		.PhysicalPath = (Root / "Content/UnsavedRename.dasset").generic_string()};
	FProviderTransferFixture Service;
	Observe(Service);
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(Item)));
	EXPECT_TRUE(Service.CanRenameItem(Item, "LiveRenamed"));
	EXPECT_TRUE(Service.CanMoveItem(Item, (Root / "Content/B").generic_string()));
	ASSERT_TRUE(Service.RenameItem(Item, "LiveRenamed", NewItem)) << OperationError;
	EXPECT_EQ(TestVirtualPath(NewItem), "/ContentBrowserTests/LiveRenamed.LiveRenamed");
	EXPECT_EQ(Object->GetObjectPath(), TestVirtualPath(NewItem));
}

TEST_F(FContentBrowserModelTests, SelectedAssetRenamePreservesSiblingAndSupportsCaseOnlyFollowup)
{
	FPackagePath Source; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/MultiRename", Source));
	DMaterial* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Source, Object));
	auto* Sibling = NewObject<DMaterial>(Object->GetPackage(), FName("Sibling"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_NE(Sibling, nullptr); ASSERT_TRUE(SavePackage(Object->GetPackage()));
	const auto SiblingPath = Sibling->GetObjectPath();
	FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset, .Name = "MultiRename",
		.VirtualPath = Object->GetObjectPath(), .PackagePath = Source, .PhysicalPath = (Root / "Content/MultiRename.dasset").generic_string()};
	FProviderTransferFixture Service;
	Observe(Service);
	ASSERT_TRUE(Service.RenameItem(Item, "Chosen", NewItem)) << OperationError;
	EXPECT_EQ(Sibling->GetObjectPath(), SiblingPath);
	FContentBrowserItem CaseItem;
	ASSERT_TRUE(Service.RenameItem(NewItem, "chosen", CaseItem)) << OperationError;
	EXPECT_EQ(TestVirtualPath(CaseItem), "/ContentBrowserTests/chosen.chosen");
	EXPECT_TRUE(std::ranges::any_of(std::filesystem::directory_iterator(Root / "Content"), [](const auto& Entry) { return Entry.path().filename().generic_string() == "chosen.dasset"; }));
	EXPECT_EQ(Object->GetObjectPath(), TestVirtualPath(CaseItem));
	EXPECT_EQ(Sibling->GetObjectPath(), SiblingPath);
}

TEST_F(FContentBrowserModelTests, DeleteModelAdvancesStatesExcludesOwnRetentionAndNeverForcesCollection)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DeleteModel", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	const std::array Selection{Object};
	FAssetDeleteModel Model(Selection);
	std::vector<FAssetDeleteModel::EState> States;
	Model.OnStateChanged().AddLambda([&](auto State) { States.push_back(State); });
	EXPECT_FALSE(Model.CanDelete());
	EXPECT_EQ(Model.GetProgress(), 0.f);
	while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
	ASSERT_TRUE(Model.CanDelete()) << Model.GetProgressText();
	EXPECT_EQ(States, (std::vector{FAssetDeleteModel::EState::StartScanning, FAssetDeleteModel::EState::Scanning,
		FAssetDeleteModel::EState::UpdateActions, FAssetDeleteModel::EState::Finished}));
	EXPECT_EQ(Model.GetProgress(), 1.f);
	ASSERT_EQ(Model.GetPendingDeletedAssets().size(), 1u);
	EXPECT_FALSE(Model.GetPendingDeletedAssets().front()->IsReferencedByNativeOwner());
	const auto CollectionTime = GetLastGarbageCollectionStats().CollectionTime;
	EXPECT_TRUE(Model.DoDelete());
	EXPECT_EQ(Model.GetDeletedObjectCount(), 1);
	EXPECT_FALSE(Model.DoDelete());
	EXPECT_EQ(Model.GetDeletedObjectCount(), 1);
	EXPECT_EQ(GetLastGarbageCollectionStats().CollectionTime, CollectionTime);
}

TEST_F(FContentBrowserModelTests, SharedDeleteScanHonorsCancellationWithoutExecutingTheModel)
{
	InitializeDObjectSystem();
	for (const bool AllowCancel : {true, false})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(AllowCancel ? "/ContentBrowserTests/CancelScan" : "/ContentBrowserTests/RequiredScan", Path));
		DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
		FAssetDeleteModel Model(std::array{Object});
		FFeedbackContext Feedback;
		FScopedFeedbackContext Scope(Feedback);
		bool ObservedProgress = false;
		Model.OnStateChanged().AddLambda([&](auto State) {
			if (State != FAssetDeleteModel::EState::Scanning) return;
			const auto Progress = Feedback.GetVisibleTask();
			ASSERT_TRUE(Progress);
			ObservedProgress = true;
			EXPECT_EQ(Progress->bCanCancel, AllowCancel);
			EXPECT_EQ(Feedback.RequestCancel(Progress->Id), AllowCancel);
		});
		EXPECT_EQ(ObjectTools::Private::ScanDeleteModel(Model, AllowCancel
			? ObjectTools::EAllowCancelDuringDelete::AllowCancel : ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed), !AllowCancel);
		EXPECT_TRUE(ObservedProgress);
		EXPECT_TRUE(IsValid(Object));
		EXPECT_EQ(Model.GetDeletedObjectCount(), 0);
		EXPECT_EQ(Model.CanDelete(), !AllowCancel);
		EXPECT_FALSE(Feedback.GetVisibleTask());
	}
}

TEST_F(FContentBrowserModelTests, DeleteModelPreservesExternalStrongOwnersOfTheSameObject)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/NativeOwner", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	const std::array Selection{Object};
	FStrongObjectPtr External(Object);
	{
		FAssetDeleteModel Model(Selection);
		while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
		EXPECT_FALSE(Model.CanDelete());
		EXPECT_TRUE(Model.GetPendingDeletedAssets().front()->IsReferencedByNativeOwner());
		EXPECT_TRUE(Model.GetPendingDeletedAssets().front()->MemoryReferences.ExternalReferences.empty());
	}
	External.Reset();
	FAssetDeleteModel Model(Selection);
	while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
	ASSERT_TRUE(Model.CanDelete()) << Model.GetProgressText();
	External = FStrongObjectPtr(Object);
	EXPECT_FALSE(Model.DoDelete());
	EXPECT_EQ(Model.GetDeletedObjectCount(), 0);
	EXPECT_TRUE(IsValid(Object));
	EXPECT_FALSE(Model.DoDelete());
}

TEST_F(FContentBrowserModelTests, DeleteModelPreservesDirtyUnselectedSiblingAndPersistsItsPackage)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DeleteSibling", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	auto* Package = Object->GetPackage();
	auto* Sibling = NewObject<DObject>(Package, FName("Sibling"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_NE(Sibling, nullptr);
	ASSERT_TRUE(SavePackage(Package));
	Package->MarkDirty();
	const auto SiblingIdentity = Sibling->GetObjectPath();
	FStrongObjectPtr SiblingOwner(Sibling);
	const std::array Selection{Object};
	FAssetDeleteModel Model(Selection);
	while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
	ASSERT_TRUE(Model.CanDelete()) << Model.GetProgressText();
	ASSERT_TRUE(Model.DoDelete());
	EXPECT_EQ(Model.GetDeletedObjectCount(), 1);
	EXPECT_TRUE(IsValid(Sibling));
	EXPECT_EQ(Sibling->GetObjectPath(), SiblingIdentity);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_EQ(Package->GetTopLevelAssets().size(), 1u);
	EXPECT_EQ(Package->GetTopLevelAssets().front(), Sibling);
	const auto Entry = FindPackageExact(Path);
	ASSERT_TRUE(Entry);
	ASSERT_EQ(Entry->TopLevelAssets.size(), 1u);
	EXPECT_EQ(Entry->TopLevelAssets.front().AssetPath.GetAssetName(), "Sibling");
}

TEST_F(FContentBrowserModelTests, DeleteModelRejectsExportsAddedAfterAnalysis)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DeleteExpansion", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	ASSERT_TRUE(SavePackage(Object->GetPackage()));
	const std::array Selection{Object};
	FAssetDeleteModel Model(Selection);
	while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
	ASSERT_TRUE(Model.CanDelete()) << Model.GetProgressText();
	auto* Sibling = NewObject<DObject>(Object->GetPackage(), FName("LateSibling"), EObjectFlags::Public);
	ASSERT_NE(Sibling, nullptr);
	EXPECT_FALSE(Model.DoDelete());
	EXPECT_TRUE(IsValid(Object));
	EXPECT_TRUE(IsValid(Sibling));
	EXPECT_EQ(Model.GetDeletedObjectCount(), 0);
}

TEST_F(FContentBrowserModelTests, DeleteReferencerInformationReportsRealOwnersAndProperties)
{
	InitializeDObjectSystem();
	FPackagePath BasePath, RefPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ReferenceTarget", BasePath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/ReferenceOwner", RefPath));
	DMaterial* Base = nullptr; DMaterialInstance* Owner = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(BasePath, Base));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(RefPath, Owner));
	ASSERT_TRUE(Owner->SetParent(Base));
	FReferencerInformationList References;
	const std::array<DObject*, 1> Roots{Base};
	EXPECT_FALSE(FindObjectReferencers(Base, Roots, References));
	const auto Found = std::ranges::find_if(References.ExternalReferences, [&](const auto& Ref) { return Ref.Referencer.Get() == Owner; });
	ASSERT_NE(Found, References.ExternalReferences.end());
	EXPECT_GT(Found->TotalReferences, 0);
	EXPECT_FALSE(Found->ReferencingProperties.empty());
	EXPECT_EQ(Owner->GetParent(), Base);
}

TEST_F(FContentBrowserModelTests, ObjectToolsRequiresPromptHostAndSupportsExplicitHeadlessDeletion)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/HeadlessDelete", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	const std::array Selection{Object};
	ObjectTools::SetDeletePrompt({});
	EXPECT_EQ(ObjectTools::DeleteObjects(Selection), 0);
	EXPECT_TRUE(IsValid(Object));
	EXPECT_EQ(ObjectTools::DeleteObjects(Selection, false), 1);
}

TEST_F(FContentBrowserModelTests, ObjectToolsCancellationAndPromptReentryCannotExpandTheSelection)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/CancelDelete", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	const std::array Selection{Object};
	struct FReset { ~FReset() { ObjectTools::SetDeletePrompt({}); } } Reset;
	ObjectTools::SetDeletePrompt([](auto&, auto, auto) { return false; });
	EXPECT_EQ(ObjectTools::DeleteObjects(Selection), 0);
	EXPECT_TRUE(IsValid(Object));
	ObjectTools::SetDeletePrompt([&](auto&, auto, auto) {
		EXPECT_EQ(ObjectTools::DeleteObjects(Selection, false), 0);
		return true;
	});
	EXPECT_EQ(ObjectTools::DeleteObjects(Selection), 1);
}

TEST_F(FContentBrowserModelTests, DeleteAssetsSharesObjectOrchestrationAndRetainsTheExactSelection)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DeleteAssetSelection", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	auto* Sibling = NewObject<DObject>(Object->GetPackage(), FName("Sibling"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_NE(Sibling, nullptr); ASSERT_TRUE(SavePackage(Object->GetPackage()));
	FTopLevelAssetPath AssetPath;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, Object->GetName(), AssetPath));
	const auto Entry = FindAssetExact(AssetPath);
	ASSERT_TRUE(Entry);
	std::vector<FAssetData> Selection{*Entry.Asset};
	struct FReset { ~FReset() { ObjectTools::SetDeletePrompt({}); } } Reset;
	int Prompts = 0;
	ObjectTools::SetDeletePrompt([&](auto& Model, auto, auto) {
		++Prompts;
		EXPECT_EQ(Model.GetState(), FAssetDeleteModel::EState::Finished);
		EXPECT_EQ(Model.GetPendingDeletedAssets().size(), 1u);
		EXPECT_EQ(Model.GetPendingDeletedAssets().front()->GetObject(), Object);
		Selection.clear();
		EXPECT_EQ(ObjectTools::DeleteItems({.ObjectsToDelete = {Sibling}, .bShowConfirmation = false}).ObjectsDeleted, 0);
		return true;
	});
	EXPECT_EQ(ObjectTools::DeleteAssets(Selection), 1);
	EXPECT_EQ(Prompts, 1);
	EXPECT_TRUE(IsValid(Sibling));
	const auto Remaining = FindPackageExact(Path);
	ASSERT_TRUE(Remaining);
	ASSERT_EQ(Remaining->TopLevelAssets.size(), 1u);
	EXPECT_EQ(Remaining->TopLevelAssets.front().AssetPath.GetAssetName(), "Sibling");
}

TEST_F(FContentBrowserModelTests, DeleteItemsReportsObjectsAndFoldersSeparatelyForMixedFolders)
{
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModuleChecked("ContentBrowserFileDataSource");
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/MixedDelete/Asset", Path));
	DObject* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	auto* Sibling = NewObject<DObject>(Object->GetPackage(), FName("Sibling"), EObjectFlags::Public);
	ASSERT_NE(Sibling, nullptr); ASSERT_TRUE(SavePackage(Object->GetPackage()));
	std::filesystem::create_directories(Root / "Content/MixedDelete/Empty");
	std::ofstream(Root / "Content/MixedDelete/note.txt") << "ordinary";
	const auto Result = ObjectTools::DeleteItems({.FoldersToDelete = {"/ContentBrowserTests/MixedDelete"}, .bShowConfirmation = false});
	EXPECT_EQ(Result.ObjectsDeleted, 2);
	EXPECT_EQ(Result.FoldersDeleted, 2);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/MixedDelete"));
}

TEST_F(FContentBrowserModelTests, DeleteModelIgnoresPersistentReferencesToUnselectedSiblings)
{
	InitializeDObjectSystem();
	FPackagePath Path, RefPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/PartialReferences", Path));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/SiblingReferencer", RefPath));
	DMaterial* Object = nullptr; DMaterialInstance* Referencer = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	auto* Sibling = NewObject<DMaterial>(Object->GetPackage(), FName("Sibling"), EObjectFlags::Public | EObjectFlags::Standalone);
	ASSERT_NE(Sibling, nullptr);
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(RefPath, Referencer));
	ASSERT_TRUE(Referencer->SetParent(Sibling));
	ASSERT_TRUE(SavePackage(Object->GetPackage())); ASSERT_TRUE(SavePackage(Referencer->GetPackage()));
	const std::array<DObject*, 1> Selection{Object};
	FAssetDeleteModel Model(Selection);
	while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
	ASSERT_TRUE(Model.CanDelete()) << Model.GetProgressText();
	EXPECT_TRUE(Model.GetPendingDeletedAssets().front()->DiskReferences.empty());
	EXPECT_TRUE(Model.DoDelete());
	EXPECT_EQ(Model.GetDeletedObjectCount(), 1);
	EXPECT_TRUE(IsValid(Sibling));
}

TEST_F(FContentBrowserModelTests, DeleteModelReportsSamePackageReferencersByObjectGraph)
{
	InitializeDObjectSystem();
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/InternalDiskRef", Path));
	DMaterial* Object = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Object));
	auto* Sibling = NewObject<DMaterialInstance>(Object->GetPackage(), FName("Sibling"), EObjectFlags::Public);
	ASSERT_NE(Sibling, nullptr); ASSERT_TRUE(Sibling->SetParent(Object)); ASSERT_TRUE(SavePackage(Object->GetPackage()));
	const std::array<DObject*, 1> Selection{Object};
	FAssetDeleteModel Model(Selection);
	while (Model.GetState() != FAssetDeleteModel::EState::Finished) Model.Tick(0.f);
	EXPECT_FALSE(Model.CanDelete());
	ASSERT_EQ(Model.GetPendingDeletedAssets().front()->DiskReferences.size(), 1u);
	EXPECT_EQ(Model.GetPendingDeletedAssets().front()->DiskReferences.front(), Path);
}
