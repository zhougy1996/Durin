#include "Editor/EditorEngine.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Editor/ContentBrowserQueryTestSupport.h"
#include "Editor/ContentBrowserItemTestSupport.h"
#include "NativeAssetBuildTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "Actors/ActorChange.h"
#include "AssetTools/Relocation.h"
#include "AssetTools/RedirectorFixup.h"
#include "AssetTools/MutationTesting.h"
#include "Asset/RegistryOperations.h"
#include "Asset/Testing/PackageSaveTesting.h"
#include "Asset/Load.h"
#include "NativeAssetTestSupport.h"
#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserRefreshCoordinator.h"
#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Data/ContentBrowserRequestContext.h"

#include "AssetTools/Relocation.h"
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
	using FContentBrowserItem = Durin::ContentBrowser::Private::FContentBrowserNativeItem;
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
			ASSERT_TRUE(InitializeAssetCompilingManager());
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
			CollectGarbage(Durin::EObjectFlags::Standalone);
			ASSERT_TRUE(InitializeAssetRuntime());
		}

		static void TearDownTestSuite()
		{
			ShutdownAssetRuntime();
			CollectGarbage(Durin::EObjectFlags::Standalone);
			ASSERT_TRUE(InitializeAssetRuntime());
		}

		std::string OperationError, OperationWarning;
		FContentChangeBatch ObservedChanges;
		FContentBrowserItem NewItem;
		auto Observe(FContentBrowserRequestContext& Service) -> void
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
	public:
		explicit FRouteOnlyThumbnailRenderer(std::string InAssetClassName)
			: AssetClassName(std::move(InAssetClassName))
		{
		}

		auto GetRegistration() const -> FThumbnailRenderingInfo override
		{
			return {
				.AssetClassName = AssetClassName,
				.RendererName = "ContentBrowserRouteTest",
				.GeneratorSchemaVersion = 1};
		}

		auto CaptureGenerationRequest(
			const FAssetThumbnailRequest&,
			uint64) -> std::expected<FAssetThumbnailGenerationRequest, std::string> override
		{
			FAssetThumbnailGenerationRequest OutRequest;

			return std::unexpected("Route-only test renderer.");
		}

	private:
		std::string AssetClassName;
	};
} // namespace

TEST_F(FContentBrowserModelTests, MaintainsHistoryAndTruncatesForwardBranch)
{
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
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
		const Durin::ContentBrowser::FContentBrowserItem&>);
	FContentBrowserItemRange Original;
	const Durin::ContentBrowser::FContentBrowserItem* First = nullptr;
	{
		FNativeContentBrowserTestQuery ModelSource;
		FContentBrowserModel Model(ModelSource);
		const auto Directory = (Root / "Content").generic_string();
		Model.SetSnapshotForTesting(Directory, Durin::ContentBrowser::Private::ItemDescriptions({
			{.Name = "B", .PhysicalPath = Directory + "/B", .FileSize = 10},
			{.Name = "A", .PhysicalPath = Directory + "/A", .FileSize = 20}}));
		Original = Model.GetItems();
		ASSERT_EQ(Original.size(), 2);
		EXPECT_EQ(Original.GetIndices()[0], 1);
		First = &Original.front();
		EXPECT_EQ(First, &Model.GetItemsSnapshot()[1]);
		Model.SetSort(EContentBrowserSortColumn::Size, true);
		EXPECT_EQ(TestName(Model.GetItems().front()), "B");
		EXPECT_EQ(&Model.GetItems()[1], First);
		EXPECT_EQ(TestName(Original.front()), "A");
		Model.SetSearch("/ContentBrowserTests/B");
		ASSERT_EQ(Model.GetItems().size(), 1);
		EXPECT_EQ(&Model.GetItems().front(), &Model.GetItemsSnapshot()[0]);
		Model.SetSnapshotForTesting(Directory, {});
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
	FNativeContentBrowserTestQuery Provider;
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
	FNativeContentBrowserTestQuery Provider;
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	const std::string Directory =
		std::filesystem::absolute(Root / "Content/A")
			.lexically_normal()
			.generic_string();
	ASSERT_TRUE(Model.NavigateToPhysical(Directory));
	Model.SetSnapshotForTesting(Directory, Durin::ContentBrowser::Private::ItemDescriptions({
		{.Kind = EContentBrowserItemKind::File,
			.Name = "Stable.txt",
			.PhysicalPath = Directory + "/Stable.txt",
			.Extension = ".txt"},
	}));

	ASSERT_TRUE(Model.NavigateToPhysical(Directory));
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Stable.txt");
	EXPECT_EQ(Model.GetHistory().size(), 1);
}

TEST_F(FContentBrowserModelTests, DirectoryNavigationPreservesTreeChildrenSnapshots)
{
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	const std::string TreeRoot =
		std::filesystem::absolute(Root / "Content")
			.lexically_normal()
			.generic_string();
	Model.RefreshMountSnapshot();
	Model.RequestDirectoryChildrenSnapshot(TreeRoot);
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
	ASSERT_EQ(Model.GetDirectorySnapshot(TreeRoot)->Children.size(), 2);

	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(TreeRoot));
	EXPECT_EQ(Model.GetDirectorySnapshot(TreeRoot)->Children.size(), 2);
}

TEST_F(FContentBrowserModelTests, RejectsUnavailableDirectoryWithoutFilesystemException)
{
	const std::filesystem::path Unavailable = Root / "Content/B";
	FNativeContentBrowserTestQuery ModelSource;
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
	const FPackageCatalogEntry AssetData =
		FindPackageExact(AssetPath);
	ASSERT_NE(AssetData, nullptr);
	auto ThumbnailRegistration = GetDefaultThumbnailManager().RegisterScoped(
			std::make_unique<FRouteOnlyThumbnailRenderer>(TestAssetClassName(AssetData->TopLevelAssets.front())));
	ASSERT_TRUE(ThumbnailRegistration) << ThumbnailRegistration.error();

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
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
	EXPECT_EQ(FContentBrowserPaths::VirtualToPhysical("/ContentBrowserTests/"), Content);
	EXPECT_EQ(FContentBrowserPaths::VirtualToPhysical("/contentbrowsertests"), Content);
	EXPECT_EQ(FContentBrowserPaths::VirtualToPhysical("/ContentBrowserTests/A/"), Child);
	EXPECT_EQ(FContentBrowserPaths::PhysicalToVirtualDirectory(Child), "/ContentBrowserTests/A/");
	EXPECT_TRUE(FContentBrowserPaths::VirtualToPhysical("/ContentBrowserTests/../Escape").empty());
	EXPECT_TRUE(FContentBrowserPaths::VirtualToPhysical("/ContentBrowserTests//A").empty());
	EXPECT_TRUE(FContentBrowserPaths::VirtualToPhysical("/Unknown/A").empty());
	EXPECT_EQ(FContentBrowserPaths::VirtualToPhysical("/ContentBrowserTests/Missing/"),
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
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

	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	ASSERT_EQ(Model.GetMounts().size(), 2u);
	EXPECT_EQ(Model.GetMounts()[0].VirtualRoot, "/Game/");
	EXPECT_EQ(Model.GetMounts()[1].VirtualRoot, "/Engine/");
}

TEST_F(FContentBrowserModelTests, RefreshesMountSnapshotAfterRegistryReplacementAndRestoration)
{
	FNativeContentBrowserTestQuery ModelSource;
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

	const FAssetRelocation Mapping{
		SourcePath, DestinationPath};
	Durin::Tests::FTestTransactorOwner Transactions;
	ASSERT_TRUE(IAssetTools::Get().RelocateAssets({
		.Mappings = {Mapping}}));
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.SetSnapshotForTesting(
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
	Model.SetSearch("stone");
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Stone");
}

TEST_F(FContentBrowserModelTests, AssetLifecycleNotificationsRefreshCreationSaveRenameAndDiscard)
{
	InitializeDObjectSystem();
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	FPackagePath Path, DiscardPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Live", Path));
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Discard", DiscardPath));
	const auto CreatedFrom = GetAssetQueryRevision();
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	EXPECT_TRUE(Operations.CanDuplicateItem(AssetItem(Path)));
	EXPECT_TRUE(Model.ApplyContentChanges(CaptureAssetQueryChanges(CreatedFrom)));
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(TestFileSize(Model.GetItems().front()), 0u);
	const auto RenamedFrom = GetAssetQueryRevision();
	Material->Rename("Renamed");
	EXPECT_TRUE(Model.ApplyContentChanges(CaptureAssetQueryChanges(RenamedFrom)));
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_TRUE(TestVirtualPath(Model.GetItems().front()).ends_with(".Renamed"));
	const auto SavedFrom = GetAssetQueryRevision();
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	EXPECT_TRUE(Model.ApplyContentChanges(CaptureAssetQueryChanges(SavedFrom)));
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_GT(TestFileSize(Model.GetItems().front()), 0u);
	const auto UnloadedFrom = GetAssetQueryRevision();
	ASSERT_TRUE(UnloadPackage(Path));
	EXPECT_TRUE(Model.ApplyContentChanges(CaptureAssetQueryChanges(UnloadedFrom)));
	EXPECT_EQ(Model.GetItems().size(), 1u);
	const auto DiscardedFrom = GetAssetQueryRevision();
	DMaterial* Unsaved = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(DiscardPath, Unsaved));
	EXPECT_TRUE(Model.ApplyContentChanges(CaptureAssetQueryChanges(DiscardedFrom)));
	ASSERT_EQ(Model.GetItems().size(), 2u);
	const auto RemovedFrom = GetAssetQueryRevision();
	ASSERT_TRUE(UnloadPackage(DiscardPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(Model.ApplyContentChanges(CaptureAssetQueryChanges(RemovedFrom)));
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
	for (const bool bAsync : {false, true})
	{
		FNativeContentBrowserTestQuery ModelSource;
		FContentBrowserModel Model(ModelSource, bAsync);
		ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
		if (bAsync) while (Model.IsLoading()) { (void)Model.PumpPendingSnapshots(); std::this_thread::yield(); }
		EXPECT_TRUE(std::ranges::any_of(Model.GetItems(), [](const auto& Item) {
			return TestKind(Item) == EContentBrowserItemKind::Folder && TestName(Item) == "Unsaved";
		}));
		EXPECT_FALSE(!Model.RevealAsset(Testing::MakePackageLeafTopLevelAssetPathForTests(Path).ToString()).IsValid());
		if (bAsync) while (Model.IsLoading()) { (void)Model.PumpPendingSnapshots(); std::this_thread::yield(); }
		ASSERT_EQ(Model.GetItems().size(), 1u);
		EXPECT_EQ(TestPackagePath(Model.GetItems()[0]), Path);
		EXPECT_EQ(TestAssetClassName(Model.GetItems()[0]), DMaterial::StaticClass()->GetQualifiedName().ToString());
		Model.RequestDirectoryChildrenSnapshot((Root / "Content/Unsaved").generic_string());
		if (bAsync) Model.WaitForPendingSnapshotsForTesting();
		else Model.RefreshRequestedDirectoryChildrenSnapshots();
		const auto Children = Model.GetDirectorySnapshot((Root / "Content/Unsaved").generic_string());
		ASSERT_TRUE(Children);
		EXPECT_EQ(Children->Children, std::vector<std::filesystem::path>{Directory});
		EXPECT_TRUE(Model.GetEnumerationDiagnostics().empty());
		EXPECT_FALSE(std::filesystem::exists(Directory));
	}
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(Directory.generic_string()));
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	Model.SetSearch("does-not-match");
	ASSERT_TRUE(Model.GetItems().empty());

	const auto Revealed = Model.RevealAsset(
		Testing::MakePackageLeafTopLevelAssetPathForTests(AssetPath).ToString());

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
	ASSERT_TRUE(IAssetTools::Get().RelocateAssets({
		.Mappings = {{SourcePath, DestinationPath}}}));

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const FTopLevelAssetPath AssetPath =
		Testing::MakePackageLeafTopLevelAssetPathForTests(SourcePath);
	FTopLevelAssetPath RelocatedAssetPath;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(
		DestinationPath, AssetPath.GetAssetName(), RelocatedAssetPath));
	const auto Revealed = Model.RevealAsset(RelocatedAssetPath.ToString());
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	Model.SetTypeFilter(EContentBrowserTypeFilter::Files);
	Model.SetSearch("does-not-match");
	ASSERT_TRUE(Model.GetItems().empty());

	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	FContentBrowserItem CreateResultItem;
	const bool CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string(), "New Folder", CreateResultItem);
	ASSERT_TRUE(CreateResult) << OperationError;

	const auto Revealed =
		Model.RevealPhysicalItem(TestPhysicalPath(CreateResultItem));

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

	FNativeContentBrowserTestQuery ModelSource;

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
	EXPECT_FALSE(bObservedNestedFile);
	EXPECT_EQ(std::ranges::count_if(
		Model.GetItems(),
		[](const auto& Item) { return TestName(Item) == "Needle.txt"; }), 0);

	Model.SetSearch("needle");
	EXPECT_TRUE(bObservedNestedFile);
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Needle.txt");

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
		std::ofstream(Root / "Content/Raw.dbulk") << "bulk";
		std::ofstream(Root / "Content/Orphan.DBULK") << "orphan";
	}
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
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
	FNativeContentBrowserTestQuery ModelSource;
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
	Model.RequestDirectoryChildrenSnapshot(TreeRoot.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const auto PublishedChildrenSnapshot = Model.GetDirectorySnapshot(TreeRoot.generic_string());
	ASSERT_TRUE(PublishedChildrenSnapshot);
	const auto& PublishedChildren = PublishedChildrenSnapshot->Children;
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
		FNativeContentBrowserTestQuery ModelSource;
		FContentBrowserModel Model(ModelSource, true);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
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
	EXPECT_EQ(TestName(Model.GetItems().front()), "Latest");
	EXPECT_EQ(SkippedVisits->load(), 0);
	EXPECT_EQ(Model.GetCurrentPhysicalPath(), C.generic_string());
}

TEST_F(FContentBrowserModelTests, AsyncSearchPublishesUsingLatestQueryWithoutRestartingCapture)
{
	InitializeDObjectSystem();
	const auto A = Root / "Content/A";
	std::filesystem::create_directories(A / "Nested/Needle");
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
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
	const uint64 Generation = Model.GetItemsRequestCount();
	Model.SetSearch("Needle");
	Model.SetSort(EContentBrowserSortColumn::Name, false);
	EXPECT_EQ(Model.GetItemsRequestCount(), Generation);
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Needle");
}

TEST_F(FContentBrowserModelTests, AsyncTreeDiscardsResultsInvalidatedDuringEnumeration)
{
	InitializeDObjectSystem();
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Old");
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
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



TEST_F(FContentBrowserModelTests, AsyncRevealReturnsIdentityBeforeDirectoryCaptureCompletes)
{
	InitializeDObjectSystem();
	const auto Folder = Root / "Content/A/Revealed";
	std::filesystem::create_directory(Folder);
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
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
	Model.WaitForPendingSnapshotsForTesting();
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(Model.GetItems().front().GetItemKey(), Revealed);
}

TEST_F(FContentBrowserModelTests, RetainedDirectorySnapshotSurvivesInvalidationAndModelDestruction)
{
	std::shared_ptr<const FContentBrowserDirectorySnapshot> Published;
	const std::string TreeRoot = (Root / "Content").generic_string();
	{
		FNativeContentBrowserTestQuery ModelSource;
		FContentBrowserModel Model(ModelSource);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.SetSnapshotForTesting(
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
	ASSERT_EQ(Model.GetItems().size(), 1);
	EXPECT_EQ(TestName(Model.GetItems().front()), "Stone.png");
	Model.SetShowHiddenFiles(true);
	ASSERT_EQ(Model.GetItems().size(), 2);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(),
		[](const auto& Item) { return TestName(Item) == ".internal"; }));
	Model.SetSearch("notes");
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.SetSnapshotForTesting(
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
	ASSERT_EQ(Model.GetItems().size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(), [](const auto& Item) {
			return TestKind(Item) == EContentBrowserItemKind::Redirector;
		}));
	Model.SetShowRedirectors(false);
	Model.SetTypeFilter(EContentBrowserTypeFilter::Redirectors);
	ASSERT_EQ(Model.GetItems().size(), 2u);
	EXPECT_TRUE(std::ranges::any_of(
		Model.GetItems(), [](const auto& Item) {
			return TestKind(Item) == EContentBrowserItemKind::Redirector
				&& TestName(Item) == "OldStone";
		}));
	Model.SetSearch("Final/Stone");
	ASSERT_EQ(Model.GetItems().size(), 1u);
}

TEST_F(FContentBrowserModelTests, KeepsFoldersFirstAndSortsEqualKeysStably)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.SetSnapshotForTesting(
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

	ASSERT_EQ(Model.GetItems().size(), 3);
	EXPECT_EQ(TestKind(Model.GetItems()[0]), EContentBrowserItemKind::Folder);
	EXPECT_TRUE(TestPhysicalPath(Model.GetItems()[1]).ends_with("second.dasset"));
	EXPECT_TRUE(TestPhysicalPath(Model.GetItems()[2]).ends_with("first.dasset"));
}

TEST_F(FContentBrowserModelTests, OperationsRejectCollisionsAndRenameOrdinaryFolders)
{
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	EXPECT_EQ(OperationError, "An item with that name already exists.");

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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
		std::filesystem::absolute(Root / "Content/Original_Copy2.dasset")
			.lexically_normal().generic_string());
	DMaterial* Duplicate = nullptr;
	{
		auto LoadedValue = LoadObjectDetailed<DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(DuplicatePath));
		Duplicate = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_NE(Duplicate, Material);
	EXPECT_EQ(Duplicate->GetStaticProperties(), Properties);
	EXPECT_FALSE(Duplicate->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(ResultItem)));
	FContentBrowserItem PastedResultItem;
	const bool PastedResult = Operations.DuplicateItem(
		AssetItem(SourcePath),
		(Root / "Content/A").generic_string(), PastedResultItem);
	ASSERT_TRUE(PastedResult) << OperationError;
	EXPECT_EQ(TestVirtualPath(PastedResultItem),
		Testing::MakePackageLeafTopLevelAssetPathForTests(PastedPath).ToString());
	DMaterial* Pasted = nullptr;
	{
		auto LoadedValue = LoadObjectDetailed<DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(PastedPath));
		Pasted = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
			FAssetDeleteContribution& Contribution) -> FAssetWriteResult {
			if (TestPackagePath(Data) == AssetPath)
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem CompanionItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "Owned.meta",
		.PhysicalPath = Companion.generic_string(),
		.Extension = ".meta"};
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); ++Publications; }});
	Observe(Service);
	ASSERT_TRUE(Service.CanMoveItem(Items.front(), (Root / "Content/B").generic_string()));
	ASSERT_TRUE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string())) << OperationError;
	EXPECT_EQ(Publications, 1);
	EXPECT_EQ(ObservedChanges.Changes.size(), 2u);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/notes.txt"));
	FPackagePath Moved;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/SharedMove", Moved));
	ASSERT_NE(FindPackageExact(Moved), nullptr);
	const FContentBrowserItem Asset{.Kind = EContentBrowserItemKind::Asset,
		.VirtualPath = "/ContentBrowserTests/B/SharedMove.AssetObject", .PackagePath = Moved};
	ASSERT_TRUE(Service.CanRenameItem(Asset, "RenamedMove"));
	FContentBrowserItem Renamed;
	ASSERT_TRUE(Service.RenameItem(Asset, "RenamedMove", Renamed)) << OperationError;
	EXPECT_EQ(TestVirtualPath(Renamed), "/ContentBrowserTests/B/RenamedMove.AssetObject");
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
	FContentBrowserRequestContext Service;
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++Publications; }});
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++Publications; }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_EQ(Publications, 0);
	EXPECT_TRUE(std::filesystem::exists(First / "notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Second / "unknown.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/First"));
}

TEST_F(FContentBrowserModelTests, OperationsPropagateMoveFailureAndUseRecursiveDeletionPreflight)
{
	InitializeDObjectSystem();
	FPackagePath SourcePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/Old", SourcePath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SourcePath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	Material->GetPackage()->MarkDirty();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	EXPECT_FALSE(MoveResult);
	EXPECT_NE(OperationError.find("dirty"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(AssetItem)));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/New.dasset"));

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
	const FContentDeletionPlanPtr Plan =
		Operations.PrepareDeleteItems(Items);
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[&](const FContentDeletionFingerprint& Entry) {
			return TestKind(Entry) == EContentDeletionEntryKind::OrdinaryFile
				&& TestPhysicalPath(Entry)
					== std::filesystem::absolute(Folder / "child.txt")
						.lexically_normal()
						.generic_string();
		}));
	EXPECT_TRUE(std::filesystem::exists(Folder));
	EXPECT_TRUE(std::filesystem::exists(Folder / "child.txt"));

}

TEST_F(FContentBrowserModelTests, PackageRenameReturnsRelocatedAssetIdentity)
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);

	FContentBrowserItem ResultItem;
	const bool Result =
		Operations.RenameItem(AssetItem, "NewPackage", ResultItem);

	ASSERT_TRUE(Result) << OperationError;
	EXPECT_EQ(TestVirtualPath(ResultItem),
		"/ContentBrowserTests/NewPackage.AssetObject");
}

TEST_F(FContentBrowserModelTests, RefreshesSnapshotAfterFolderMutation)
{
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	Model.RequestDirectoryChildrenSnapshot((Root / "Content").generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const auto BeforeSnapshot = Model.GetDirectorySnapshot((Root / "Content").generic_string());
	ASSERT_TRUE(BeforeSnapshot);
	const size_t ChildrenBefore = BeforeSnapshot->Children.size();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);

	FContentBrowserItem CreateResultItem;
	const bool CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string(), "New Folder", CreateResultItem);
	ASSERT_TRUE(CreateResult) << OperationError;
	Model.RefreshItemsSnapshot();
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(
		(Root / "Content").generic_string()));
	Model.RequestDirectoryChildrenSnapshot((Root / "Content").generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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

	const FContentDeletionPlanPtr Plan =
		Operations.PrepareDeleteItems(Items);
	ASSERT_NE(Plan, nullptr);
	EXPECT_TRUE(Plan->CanExecute());
	ASSERT_EQ(Plan->MaximalRoots.size(), 1);
	EXPECT_EQ(Plan->Summary.FolderCount, 3);
	EXPECT_EQ(Plan->Summary.FileCount, 2);
	EXPECT_EQ(Plan->Summary.AssetCount, 0);
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[&](const FContentDeletionFingerprint& Entry) {
			return TestPhysicalPath(Entry).ends_with("/.hidden/secret.txt");
		}));
	EXPECT_TRUE(std::filesystem::exists(Hidden / "secret.txt"));
	{
		std::ofstream HiddenFile(Hidden / "secret.txt", std::ios::app);
		HiddenFile << " changed";
	}
	auto Confirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(Confirmation));
	EXPECT_TRUE(std::filesystem::exists(Hidden / "secret.txt"));
	EXPECT_TRUE(std::filesystem::exists(Tree / "visible.txt"));
	ASSERT_NE(Confirmation, nullptr);
	EXPECT_NE(Confirmation, Plan);
	ASSERT_TRUE(Confirmation->CanExecute());
	ASSERT_EQ(Confirmation->MaximalRoots.size(), 1);
	auto RetiredConfirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(RetiredConfirmation));
	EXPECT_TRUE(std::filesystem::exists(Tree));
	EXPECT_TRUE(Operations.DeleteItems(Confirmation));
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "UnknownPackage",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&FolderItem, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EContentDeletionBlocker::UnknownPackage;
		}));
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Entries,
		[](const FContentDeletionFingerprint& Entry) {
			return TestKind(Entry) == EContentDeletionEntryKind::UnknownPackage;
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations;
	const FContentBrowserItem Folder{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Folder",
		.PhysicalPath = (Root / "Content/Folder").generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&Folder, 1});
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EContentDeletionBlocker::ReadOnlyMount;
		}));
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/Folder"));

	const FContentBrowserItem MountRoot{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Content",
		.PhysicalPath = (Root / "Content").generic_string()};
	const FContentDeletionPlanPtr MountRootPlan = Operations.PrepareDeleteItems(
		std::span{&MountRoot, 1});
	EXPECT_TRUE(std::ranges::any_of(
		MountRootPlan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EContentDeletionBlocker::MountRoot;
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
			FAssetDeleteContribution& Contribution) -> FAssetWriteResult {
			if (TestPackagePath(Data).GetView().starts_with(
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
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {BasePath}}, Job));
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
			return Ec ? FAssetWriteResult{EAssetWriteError::IoError, Ec.message()} : FAssetWriteResult{};
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
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
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
	ASSERT_TRUE(OwnershipResult) << OwnershipResult.Message;
	EXPECT_EQ(Ownership.State, EAssetCompanionOwnershipState::Owned);
	ASSERT_EQ(Ownership.Owners.size(), 1u);
	EXPECT_EQ(Ownership.Owners.front(), Path);
	FAssetDeletionOperation Operation;
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
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
			FAssetDeleteContribution& Contribution) -> FAssetWriteResult {
			if (TestPackagePath(Data) == OwnerPath)
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
	const auto PackageFile = TestPhysicalPath(Operation.GetEntries().front().RegistryEntry);
	ASSERT_TRUE(Operation.Delete({.Delete = [&](auto&) -> FAssetWriteResult {
		std::error_code Ec;
		std::filesystem::remove(PackageFile, Ec);
		if (Ec) return {EAssetWriteError::IoError, Ec.message()};
		std::filesystem::remove(Companion, Ec);
		return Ec ? FAssetWriteResult{EAssetWriteError::IoError, Ec.message()} : FAssetWriteResult{};
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
			FAssetDeleteContribution&) -> FAssetWriteResult {
			if (TestPackagePath(Data) == Path) ++Inspections;
			return {};
		})};
	ASSERT_NE(Reset.Handle, 0u);
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File,
		.Name = "SingleDeletionPlan", .PhysicalPath = File};
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Inspections, 1u);
	ASSERT_TRUE(Service.DeleteItems(Plan));
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
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	const auto Handle = RegisterAssetDeleteContributor(DMaterial::StaticClass(),
		[](const FPackageData&, const FAssetPackageInspection&, FAssetDeleteContribution&) -> FAssetWriteResult {
			return {};
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File,
		.Name = "ScopedDelete", .PhysicalPath = SelectedFile};
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	std::ofstream(OutsideFile, std::ios::binary | std::ios::trunc) << "unrelated corrupt package";
	FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(QueryAssetCompanionOwnership(Root / "Content/unclaimed.txt", Ownership));
	EXPECT_EQ(Ownership.State, EAssetCompanionOwnershipState::Unclaimed);
	ASSERT_TRUE(Service.DeleteItems(Plan));
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
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	EXPECT_TRUE(std::ranges::any_of(Operation.GetBlockers(), [&](const auto& Blocker) {
		return TestKind(Blocker) == EAssetDeletionBlocker::ExternalLoadedReference
			&& Blocker.AssetPath == Path && Blocker.Details.find("Close the level") != std::string::npos;
	}));
	EXPECT_EQ(World->GetCurrentLevel(), Level);
	EXPECT_FALSE(Level->IsGarbage());
	EXPECT_TRUE(std::filesystem::exists(File));
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	World->CleanupWorld();
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	ASSERT_TRUE(Operation.Delete({.Delete = [&](auto& Removed) -> FAssetWriteResult {
		std::error_code Error;
		if (!std::filesystem::remove(File, Error)) return {EAssetWriteError::IoError, Error.message()};
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
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	EXPECT_EQ(Operation.Validate().Error, EAssetWriteError::InUse);
	uint32 Deletes = 0;
	const auto Delete = [&](auto&) -> FAssetWriteResult { ++Deletes; return {}; };
	EXPECT_FALSE(Operation.Delete({.Delete = Delete}));
	EXPECT_EQ(Deletes, 0u);
	ASSERT_TRUE(World->SetCurrentLevel(nullptr));
	EXPECT_TRUE(Operation.Validate());
	EXPECT_FALSE(Operation.Delete({
		.Delete = Delete,
		.ValidateFiles = [&](FAssetDeletionFileIdentities& Identities) -> FAssetWriteResult {
			if (!World->SetCurrentLevel(Level)) return {EAssetWriteError::InUse, "Could not open level."};
			const auto Identity = FFileHelper::HashFileXx128(File);
			if (!Identity) return {EAssetWriteError::IoError, Identity.error().ToString()};
			Identities.emplace(File.generic_string(), *Identity);
			return {};
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
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
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
			return Error ? FAssetWriteResult{EAssetWriteError::IoError, Error.message()} : FAssetWriteResult{};
		},
		.ValidateFiles = [&](FAssetDeletionFileIdentities& Identities) -> FAssetWriteResult {
			if (bOmitIdentity) return {};
			auto Identity = FFileHelper::HashFileXx128(File);
			if (!Identity)
				return {EAssetWriteError::IoError, Identity.error().ToString()};
			++Hashes;
			Identities.emplace(File.generic_string(), *Identity);
#ifdef _WIN32
			// A duplicate AssetTools payload read would fail until the physical callback.
			Guard.Handle = CreateFileW(File.c_str(), GENERIC_READ, FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (Guard.Handle == INVALID_HANDLE_VALUE)
				return {EAssetWriteError::IoError, "Could not guard the verified file against duplicate reads."};
#endif
			return {};
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
		auto CaptureSnapshot(FAssetReferenceStoreSnapshot& Snapshot) -> FAssetReadResult override
		{
			if (bFail) return {EAssetReadError::IoError, "Injected provider inspection failure."};
			Snapshot = {.ProviderId = "deletion-test", .Fingerprint = Fingerprint};
			return {};
		}
		auto PrepareRewrite(std::span<const FAssetReferenceRewrite>, std::string_view,
			FAssetReferenceStoreRewriteContribution&) -> FAssetWriteResult override
		{
			return {EAssetWriteError::InUse, "Deletion must not rewrite references."};
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
		return TestKind(Blocker) == EAssetDeletionBlocker::ReferenceStoreInspectionFailed;
	}));
	Store.bFail = false;
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
	Store.Fingerprint = "second";
	bool bCalled = false;
	EXPECT_FALSE(Operation.Delete({.Delete = [&](auto&) -> FAssetWriteResult {
		bCalled = true;
		return {};
	}}));
	EXPECT_FALSE(bCalled);
	EXPECT_EQ(Operation.GetWriteResult().Error, EAssetWriteError::StaleData);
	EXPECT_NE(FindResidentPackage(Path), nullptr);
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(*(FindPackageExact(Path)))));
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "ProviderDeletion", .PhysicalPath = TestPhysicalPath(*(FindPackageExact(Path)))};
	FContentBrowserRequestContext Service;
	Observe(Service);
	auto Confirmation = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Confirmation->CanExecute());
	const auto Original = Confirmation;
	Store.bFail = true;
	EXPECT_FALSE(Service.DeleteItems(Confirmation));
	EXPECT_EQ(Confirmation, Original);
	EXPECT_NE(OperationError.find("Injected provider inspection failure"), std::string::npos);
	Store.bFail = false;
	Store.Fingerprint = "third";
	EXPECT_FALSE(Service.DeleteItems(Confirmation));
	ASSERT_TRUE(Confirmation);
	EXPECT_NE(Confirmation, Original);
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(Item)));
	EXPECT_TRUE(Service.DeleteItems(Confirmation));
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ReparseRoot",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&FolderItem, 1});
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EContentDeletionBlocker::ReparsePoint;
		}));
}

TEST_F(FContentBrowserModelTests, EmptyFolderDeletionIsPermanent)
{
	const std::filesystem::path Folder = Root / "Content/EmptyFolder";
	std::filesystem::create_directories(Folder);
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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

	ASSERT_TRUE(Operations.DeleteItems(Plan));
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
				FAssetDeleteContribution& Contribution) -> FAssetWriteResult {
				if (TestPackagePath(Data) == AssetPath)
					Contribution.Files.push_back(OutsideFile);
				return {};
			});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "UnsafeCompanion",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&Item, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[&](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EContentDeletionBlocker::OutsideMount
				&& TestPhysicalPath(Blocker)
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
			[AssetPath, Companion](const FPackageData& Data,
				const FAssetPackageInspection&,
				FAssetDeleteContribution& Contribution) -> FAssetWriteResult {
				if (TestPackagePath(Data) == AssetPath)
					Contribution.Files.push_back(Companion);
				return {};
			});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ReparseCompanion",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.PrepareDeleteItems(
		std::span{&Item, 1});

	EXPECT_FALSE(Plan->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker) == EContentDeletionBlocker::ReparsePoint;
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
		[AssetPath, Companion](const FPackageData& Data,
			const FAssetPackageInspection&,
			FAssetDeleteContribution& Contribution) -> FAssetWriteResult {
			if (TestPackagePath(Data) == AssetPath)
				Contribution.Files.push_back(Companion);
			return {};
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(AssetPath, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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

	ASSERT_TRUE(Operations.DeleteItems(Plan));
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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "DestructiveMaterial",
		.VirtualPath = AssetPath.ToString(),
		.PhysicalPath = PackagePath.generic_string(),
		.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString()};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());

	ASSERT_TRUE(Operations.DeleteItems(Plan));
	EXPECT_FALSE(std::filesystem::exists(PackagePath));
	EXPECT_EQ(FindPackageExact(AssetPath), nullptr);
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
	ASSERT_TRUE(RelocateAssets(
		std::span{&Mapping, 1}).Result);

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

	FNativeContentBrowserTestQuery ModelSource;

	FContentBrowserModel Model(ModelSource);
	Model.RefreshMountSnapshot();
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentDeletionPlanPtr AliasOnly = Operations.PrepareDeleteItems(
		std::span{&AliasItem, 1});
	EXPECT_TRUE(std::ranges::any_of(
		AliasOnly->Blockers, [](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker)
				== EContentDeletionBlocker::RedirectorTargetNotSelected;
		}));
	const FContentDeletionPlanPtr TargetOnly = Operations.PrepareDeleteItems(
		std::span{&FinalItem, 1});
	EXPECT_TRUE(std::ranges::any_of(
		TargetOnly->Blockers, [](const FContentDeletionBlocker& Blocker) {
			return TestKind(Blocker)
				== EContentDeletionBlocker::TargetRedirectorsNotSelected;
		}));

	const std::array Items{AliasItem, FinalItem};
	auto Complete = Operations.PrepareDeleteItems(Items);
	ASSERT_TRUE(Complete->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Complete->Warnings, [](const FContentDeletionWarning& Warning) {
			return Warning.Details.find("authored old path")
				!= std::string::npos;
		}));

	ASSERT_TRUE(Operations.DeleteItems(Complete));
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	EXPECT_FALSE(Operations.DeleteItems(Plan));
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "rewrite.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	const FContentDeletionPlanPtr Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Plan->CanExecute());
	{
		std::ofstream File(FilePath, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(FilePath, ConfirmedWriteTime);
	auto Confirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(Confirmation));
	EXPECT_TRUE(std::filesystem::exists(FilePath));
	ASSERT_NE(Confirmation, nullptr);
	EXPECT_NE(Confirmation, Plan);
	ASSERT_TRUE(Confirmation->CanExecute());
	auto RetiredConfirmation = Plan;
	EXPECT_FALSE(Operations.DeleteItems(RetiredConfirmation));
	EXPECT_TRUE(std::filesystem::exists(FilePath));
	EXPECT_TRUE(Operations.DeleteItems(Confirmation));
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	EXPECT_FALSE(Operations.DeleteItems(Plan));
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
	FContentBrowserRequestContext Operations({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Operations);
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "evicted.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	auto Plan = Operations.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Operations.DeleteItems(Plan));
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
	ASSERT_TRUE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {Path}}, Operation));
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }, .CanMutateContent = [&] { return bAllowed; }});
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }, .CanMutateContent = {}});
	Observe(Service);
	const auto Result = Service.BulkSaveItems(std::array{AssetItem(Saved), AssetItem(Failed)});
	EXPECT_FALSE(Result);
	EXPECT_TRUE((ObservedChanges.bFullRefresh || !ObservedChanges.Changes.empty()));
	EXPECT_EQ(Publications, 1);
	EXPECT_NE(OperationError.find(Failed.ToString()), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/Saved.dasset"));
	ASSERT_EQ(ObservedChanges.Changes.size(), 1u);
	EXPECT_EQ(ObservedChanges.Changes.front().NewAssetPath, Saved.ToString());
}

TEST_F(FContentBrowserModelTests, DeletionKeepsConfirmationWhilePackageIsDirty)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/DirtyConfirmation", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "DirtyConfirmation", .PhysicalPath = TestPhysicalPath(*(FindPackageExact(Path)))};
	FContentBrowserRequestContext Service;
	Observe(Service);
	auto Confirmation = Service.PrepareDeleteItems(std::span{&Item, 1});
	ASSERT_TRUE(Confirmation->CanExecute());
	const auto Original = Confirmation;
	Material->GetPackage()->MarkDirty();
	EXPECT_FALSE(Service.DeleteItems(Confirmation));
	EXPECT_EQ(Confirmation, Original);
	EXPECT_NE(OperationError.find("dirty"), std::string::npos);
	EXPECT_TRUE(OperationWarning.empty());
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(Item)));
	// No authored bytes were changed; removing the transient blocker permits retry.
	Material->GetPackage()->ClearDirty();
	EXPECT_TRUE(Service.DeleteItems(Confirmation));
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
	FContentBrowserRequestContext Service;
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
		EXPECT_FALSE(Service.DeleteItems(Confirmation));
		EXPECT_EQ(Confirmation, Original);
		EXPECT_FALSE(OperationError.empty());
		EXPECT_TRUE(OperationWarning.empty());
		EXPECT_TRUE(std::filesystem::exists(File));
	}
	// A retained confirmation is not permission to skip validation on retry.
	std::ofstream(File, std::ios::trunc) << "changed";
	EXPECT_FALSE(Service.DeleteItems(Confirmation));
	ASSERT_TRUE(Confirmation);
	EXPECT_NE(Confirmation, Original);
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_TRUE(Service.DeleteItems(Confirmation));
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	auto Confirmation = Service.PrepareDeleteItems(Items);
	ASSERT_TRUE(Confirmation->CanExecute());
	int Calls = 0;
	const auto First = Service.DeleteItems(Confirmation, {.RemoveAll = [&](const auto& Path) {
		if (++Calls == 2) return std::make_error_code(std::errc::io_error);
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Path, Error);
		return Error;
	}});
	EXPECT_FALSE(First);

	EXPECT_FALSE(OperationError.empty());
	EXPECT_EQ(Publications, 1);
	EXPECT_FALSE(Service.DeleteItems(Confirmation));
	EXPECT_EQ(Calls, 2);
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(Items.front())));
	EXPECT_TRUE(std::filesystem::exists(TestPhysicalPath(Items.back())));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
	auto Fresh = Service.PrepareDeleteItems(std::span{&Items.back(), 1});
	ASSERT_TRUE(Fresh->CanExecute());
	ASSERT_TRUE(Service.DeleteItems(Fresh));
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	auto OriginalPlan = Plan;
	std::ofstream(Folder / "after.txt") << "after";
	const auto Stale = Service.DeleteItems(Plan);
	EXPECT_FALSE(Stale);
	ASSERT_TRUE(Plan);
	EXPECT_NE(Plan, OriginalPlan);
	EXPECT_EQ(Plan->Summary.FileCount, 2u);
	EXPECT_FALSE(Service.DeleteItems(OriginalPlan));
	EXPECT_TRUE(std::filesystem::exists(Folder));
	Service.DismissDeletion(Plan);
	EXPECT_FALSE(Service.DeleteItems(Plan));
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	auto Plan = Service.PrepareDeleteItems(std::span{&Item, 1});
	int Calls = 0;
	const auto Result = Service.DeleteItems(Plan, {.RemoveAll = [&](const auto& Physical) {
		++Calls;
		std::error_code Error;
		Durin::Testing::RemoveTestWorkDirectory(Physical, Error);
		EXPECT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
		return Error;
	}});

	EXPECT_TRUE(Result);
	EXPECT_TRUE(OperationError.empty());
	EXPECT_NE(OperationWarning.find("ContentCommittedProjectionPending"), std::string::npos);
	EXPECT_EQ(Publications, 1);
	EXPECT_FALSE(Service.DeleteItems(Plan));
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	const FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Asset,
		.Name = "Identity", .PackagePath = Path};
	EXPECT_TRUE(Service.BulkMoveItems(std::span{&Item, 1}, (Root / "Content").generic_string()));
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	const auto Tree = Model.GetDirectorySnapshot(A.generic_string());
	const auto Version = Model.GetSnapshotVersion();
	const auto Generation = Model.GetItemsRequestCount();
	FContentChangeBatch Changes{.Changes = {{EContentChangeKind::Modified,
		(B / "asset.dasset").generic_string(), (B / "asset.dasset").generic_string()}}};
	EXPECT_FALSE(Model.ApplyContentChanges(Changes));
	EXPECT_EQ(Model.GetSnapshotVersion(), Version);
	EXPECT_EQ(Model.GetItemsRequestCount(), Generation);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
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
	const auto B = Root / "Content/Renamed";
	std::filesystem::create_directories(A / "Nested");
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Generation = Model.GetItemsRequestCount();
	EXPECT_FALSE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(B / "Other").generic_string(), {}, {}, true}}}));
	EXPECT_EQ(Model.GetItemsRequestCount(), Generation);
	std::filesystem::create_directory(A / "Latest");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "Latest").generic_string(), {}, {}, true}}}));
	EXPECT_GT(Model.GetItemsRequestCount(), Generation);
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_EQ(Model.GetItems().size(), 2u);
	EXPECT_FALSE(Model.IsLoading());
}

TEST_F(FContentBrowserModelTests, MountedReconciliationFencePreventsPrematurePublication)
{
	const auto A = Root / "Content/A";
	std::filesystem::create_directory(A / "Child");
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
	FContentBrowserRefreshCoordinator Coordinator(1, GetAssetCatalogRevision());
	Coordinator.SetPublicationSuspension([&](bool bSuspended) { Model.SetSnapshotPublicationSuspended(bSuspended); });
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Coordinator.ObserveMountedContent(2);
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_TRUE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	ASSERT_TRUE(Coordinator.Synchronize(2, GetAssetCatalogRevision(),
		[] { return FAssetReadResult{}; }, [](const FContentChangeBatch&) {}, GetAssetCatalogRevision));
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_EQ(Model.GetItems().size(), 1u);
}

TEST_F(FContentBrowserModelTests, ScopedServiceNotificationsDescribeCommittedFilesystemOperations)
{
	std::vector<FContentChangeBatch> Batches;

	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { Batches.push_back(std::move(Batch)); }});
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
	EXPECT_EQ(Batches[1].Changes[0].OldPhysicalPath, TestPhysicalPath(CreatedItem));
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content/A").generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Generation = Model.GetItemsRequestCount();
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
	EXPECT_EQ(Model.GetItemsRequestCount(), Generation);
	const auto CommittedRevision = GetAssetCatalogRevision();
	EXPECT_FALSE(PublishAssetRegistryPublication(Publication));
	EXPECT_EQ(GetAssetCatalogRevision(), CommittedRevision);
	EXPECT_TRUE(CaptureAssetCatalogChanges(CommittedRevision).Changes.empty());
	Publication = CaptureAssetRegistryPublication();
	Publication.Assets.at(APath).SearchableNames = {"ChangedInsideScope"};
	ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	EXPECT_GT(Model.GetItemsRequestCount(), Generation);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
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
	EXPECT_EQ(Model.GetDirectorySnapshot(A.generic_string())->Children.size(), 1u);
}

TEST_F(FContentBrowserModelTests, SourceThumbnailRequestsUseFileIdentityAndInvalidateOnChange)
{
	FNativeContentBrowserTestQuery Query;
	FContentBrowserThumbnailReferences References;
	const auto Image = Query.BindItemForTesting(Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::File,
		.PhysicalPath = (Root / "Content/Preview.png").generic_string(), .Extension = ".png",
		.FileSize = 128, .LastWriteTime = std::filesystem::file_time_type::clock::now()});
	References.BeginFrame();
	References.Request(Image, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.Find(Image.GetItemKey()).State, EAssetThumbnailState::Queued);
	EXPECT_EQ(References.GetAssetReferenceCountForTesting(), 0u);
	References.ApplyContentChanges({.Changes = {{EContentChangeKind::Modified, TestPhysicalPath(Image), TestPhysicalPath(Image)}}});
	EXPECT_EQ(References.Find(Image.GetItemKey()).State, EAssetThumbnailState::NotRequested);
	const auto Text = Query.BindItemForTesting(FContentBrowserItem{.Kind = EContentBrowserItemKind::File,
		.PhysicalPath = (Root / "Content/Notes.txt").generic_string(), .Extension = ".txt"});
	References.Request(Text, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.Find(Text.GetItemKey()).State, EAssetThumbnailState::NotRequested);
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
	const auto OwnerData = FindPackageExact(OwnerPath);
	const auto OtherData = FindPackageExact(OtherPath);
	auto OwnerRenderer = GetDefaultThumbnailManager().RegisterScoped(
		std::make_unique<FRouteOnlyThumbnailRenderer>(TestAssetClassName(OwnerData->TopLevelAssets.front())));
	auto OtherRenderer = GetDefaultThumbnailManager().RegisterScoped(
		std::make_unique<FRouteOnlyThumbnailRenderer>(TestAssetClassName(OtherData->TopLevelAssets.front())));
	ASSERT_TRUE(OwnerRenderer);
	ASSERT_TRUE(OtherRenderer);
	FNativeContentBrowserTestQuery Query;
	FContentBrowserThumbnailReferences References;
	for (const auto& Path : {OwnerPath, OtherPath})
	{
		const auto Asset = FindPackageExact(Path);
		ASSERT_TRUE(Asset);
		const auto Identity = Asset->TopLevelAssets.front().AssetPath.ToString();
		References.Request(Query.BindItemForTesting(Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = Identity,
			.PackagePath = Path}), EAssetThumbnailPriority::Prefetch);
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource);
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Model.RequestDirectoryChildrenSnapshot(A.generic_string());
	Model.RefreshRequestedDirectoryChildrenSnapshots();
	ASSERT_TRUE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	std::filesystem::create_directories(A / "New/Nested");
	EXPECT_TRUE(Model.ApplyContentChanges({.Changes = {{EContentChangeKind::Added, {},
		(A / "New/Nested/Asset.dasset").generic_string()}}}));
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(A.generic_string()));
	ASSERT_EQ(Model.GetItems().size(), 1u);
	EXPECT_EQ(TestName(Model.GetItems().front()), "New");
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
	FNativeContentBrowserTestQuery ModelSource;
	FContentBrowserModel Model(ModelSource, true);
	FContentBrowserRefreshCoordinator Coordinator(1, GetAssetCatalogRevision());
	Coordinator.SetPublicationSuspension([&](bool bSuspended) { Model.SetSnapshotPublicationSuspended(bSuspended); });
	ASSERT_TRUE(Model.NavigateToPhysical(A.generic_string()));
	Coordinator.ObserveMountedContent(2);
	EXPECT_FALSE(Model.PumpPendingSnapshots());
	int Attempts = 0;
	const auto Reconcile = [&] { ++Attempts; return FAssetReadResult{EAssetReadError::IoError, "forced failure"}; };
	EXPECT_FALSE(Coordinator.Synchronize(2, GetAssetCatalogRevision(), Reconcile,
		[](const FContentChangeBatch&) {}, GetAssetCatalogRevision));
	// A fresh request can proceed, and a suppressed failure must not suspend it again.
	Model.RefreshItemsSnapshot(false);
	EXPECT_TRUE(Coordinator.Synchronize(2, GetAssetCatalogRevision(), Reconcile,
		[](const FContentChangeBatch&) {}, GetAssetCatalogRevision));
	EXPECT_EQ(Attempts, 1);
	Model.WaitForPendingSnapshotsForTesting();
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](auto Batch) { Batches.push_back(std::move(Batch)); }});
	Observe(Service);
	const auto Result = Service.BulkMoveItems(Items, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/B/A/Empty/Nested"));
	ASSERT_EQ(Batches.size(), 1);
	ASSERT_EQ(Batches[0].Changes.size(), 1);
	EXPECT_TRUE(Batches[0].Changes[0].bDirectory);
}

TEST_F(FContentBrowserModelTests, ContentMovePreflightsWholeSelectionAndRejectsInvalidTargets)
{
	std::ofstream(Root / "Content/A/one.txt") << "one";
	std::ofstream(Root / "Content/A/two.txt") << "two";
	std::ofstream(Root / "Content/B/two.txt") << "occupied";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/one.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/two.txt").generic_string()}};
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/one.txt"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/one.txt"));
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/A").generic_string()));
	const FContentBrowserItem Mount{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content").generic_string()};
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Mount, 1}, (Root / "Content/B").generic_string()));
	EXPECT_FALSE(Service.BulkMoveItems(Items, Root.generic_string()));
}

TEST_F(FContentBrowserModelTests, MixedContentMoveRestoresOrdinaryFilesWhenAssetMoveFails)
{
	FPackagePath Package;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/Asset", Package));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Package, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	std::filesystem::create_directories(Root / "Content/A/Empty");
	Material->GetPackage()->MarkDirty();
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	EXPECT_FALSE(Result);
	EXPECT_NE(OperationError.find("dirty"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/Asset.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/A"));
	EXPECT_FALSE(ObservedChanges.bFullRefresh);
	EXPECT_TRUE(ObservedChanges.Changes.empty());
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/notes.txt"));
	EXPECT_EQ(ResolvePackagePathForOperation(Source).FinalPath, Destination);
}

TEST_F(FContentBrowserModelTests, ContentMoveRejectsUnknownPackagesAndDuplicateDestinations)
{
	std::ofstream(Root / "Content/A/Unknown.dasset") << "invalid package";
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string()));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/A"));
	std::ofstream(Root / "Content/A/same.txt") << "a";
	std::ofstream(Root / "Content/B/same.txt") << "b";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/same.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/B/same.txt").generic_string()}};
	EXPECT_FALSE(Service.BulkMoveItems(Items, (Root / "Content").generic_string()));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/same.txt"));
}

TEST_F(FContentBrowserModelTests, ContentMoveReportsNativePartialPublicationAndRetainsRecoveryEvidence)
{
	FPackagePath Package;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/ProjectionAsset", Package));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Package, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	std::vector<std::string> Warnings;
	Service.SetDiagnosticSink([&](ELogLevel Level, std::string_view Message) {
		if (Level >= ELogLevel::Error) OperationError = Message;
		else if (Level == ELogLevel::Warn) Warnings.emplace_back(Message);
	});
	struct FFailureReset
	{
		~FFailureReset() { SetAssetRelocationFailurePointForTesting(EAssetRelocationFailurePoint::None); }
	} Reset;
	SetAssetRelocationFailurePointForTesting(EAssetRelocationFailurePoint::PublishRegistry);

	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkMoveItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	EXPECT_FALSE(Result);

	EXPECT_FALSE(OperationError.empty());
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/notes.txt"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/ProjectionAsset.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/ProjectionAsset.dasset"));
	EXPECT_TRUE(std::ranges::any_of(Warnings, [](const auto& Message) { return Message.starts_with("Recovery backup:"); }));
	EXPECT_TRUE(std::ranges::any_of(Warnings, [](const auto& Message) { return Message.starts_with("Affected file:"); }));
	EXPECT_TRUE(ObservedChanges.bFullRefresh);
}

TEST_F(FContentBrowserModelTests, ContentMoveMovesMultipleFilesAndSkipsIdentityMoves)
{
	std::ofstream(Root / "Content/A/one.txt") << "one";
	std::ofstream(Root / "Content/A/two.txt") << "two";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/one.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/two.txt").generic_string()}};
	int Publications = 0;
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Publications; }});
	Observe(Service);
	EXPECT_TRUE(Service.BulkMoveItems(Items, (Root / "Content/A").generic_string()));
	EXPECT_EQ(Publications, 0);
	ASSERT_TRUE(Service.BulkMoveItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_EQ(Publications, 1);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/one.txt"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/A/two.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/one.txt"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/two.txt"));
}

TEST_F(FContentBrowserModelTests, ContentCopyPreservesBytesAndChoosesUniqueNames)
{
	std::ofstream(Root / "Content/A/notes.txt", std::ios::binary).write("hello\0world", 11);
	std::ofstream(Root / "Content/B/notes.txt") << "other";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/notes.txt").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/B/notes.txt").generic_string()}};

	int Notifications = 0;
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](auto) { ++Notifications; }});
	Observe(Service);
	ASSERT_TRUE(Service.BulkCopyItems(Items, (Root / "Content/A").generic_string()));
	EXPECT_EQ(Notifications, 1);
	std::ifstream First(Root / "Content/A/notes_Copy.txt", std::ios::binary);
	std::ifstream Second(Root / "Content/A/notes_Copy2.txt", std::ios::binary);
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(First), {}), std::string("hello\0world", 11));
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(Second), {}), "other");
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/notes.txt"));
	ASSERT_TRUE(Service.BulkCopyItems(std::span{Items.data(), 1}, (Root / "Content/A").generic_string()));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/notes_Copy3.txt"));
}

TEST_F(FContentBrowserModelTests, ContentCopyPreservesDirectoriesAndCollapsesSelectedDescendants)
{
	std::filesystem::create_directories(Root / "Content/A/Empty/Nested");
	std::ofstream(Root / "Content/A/.hidden") << "hidden";
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::Folder, .PhysicalPath = (Root / "Content/A").generic_string()},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File, .PhysicalPath = (Root / "Content/A/.hidden").generic_string()}};
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	ASSERT_TRUE(Service.BulkCopyItems(Items, (Root / "Content/B").generic_string()));
	EXPECT_TRUE(std::filesystem::is_directory(Root / "Content/B/A/Empty/Nested"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/.hidden"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/B/.hidden"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/.hidden"));
	ASSERT_TRUE(Service.BulkCopyItems(Items, (Root / "Content").generic_string()));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A_Copy/.hidden"));
}

TEST_F(FContentBrowserModelTests, ContentCopyPreflightsMissingSourcesPackagesBulkAndNestedDestinations)
{
	std::ofstream(Root / "Content/A/notes.txt") << "notes";
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkCopyItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_TRUE(FindPackageExact(Source));
	EXPECT_TRUE(FindPackageExact(Destination));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/A/CopySource.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/CopySource.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/notes.txt"));
}

TEST_F(FContentBrowserModelTests, PasteSaveFailureKeepsDirtyCopyAndReportsContentChange)
{
	FPackagePath Source;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/PasteSaveFailure", Source));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Source, Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	const auto AssetPath = Testing::MakePackageLeafTopLevelAssetPathForTests(Source);
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	bool Result = false;
	FContentBrowserItem ResultItem;
	{
		Testing::FScopedPackageSaveFault Fault([](auto Phase, size_t) { return Phase == Testing::EPackageSaveFaultPoint::PublishPackage; });
		Result = Service.DuplicateItem(AssetItem(AssetPath), (Root / "Content/B").generic_string(), ResultItem);
	}
	EXPECT_FALSE(Result);
	EXPECT_TRUE((ObservedChanges.bFullRefresh || !ObservedChanges.Changes.empty()));
	EXPECT_FALSE(OperationWarning.empty());
	FSoftObjectPath CopyPath;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(TestVirtualPath(ResultItem), CopyPath));
	auto LoadedCopy = LoadObjectDetailed<DMaterial>(CopyPath);
	ASSERT_TRUE(LoadedCopy);
	auto* Copy = *LoadedCopy;
	ASSERT_NE(Copy, nullptr);
	EXPECT_TRUE(Copy->GetPackage()->IsDirty());
	EXPECT_EQ(FindResidentPackage(CopyPath.GetPackagePath()), Copy->GetPackage());
	EXPECT_FALSE(std::filesystem::exists(TestPhysicalPath(ResultItem)));
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = std::move(Batch); }});
	Observe(Service);
	EXPECT_FALSE(Service.BulkCopyItems(std::span{&Bulk, 1}, (Root / "Content/B").generic_string()));
	const FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder,
		.PhysicalPath = (Root / "Content/A").generic_string()};
	const auto Result = Service.BulkCopyItems(std::span{&Folder, 1}, (Root / "Content/B").generic_string());
	ASSERT_TRUE(Result) << OperationError;
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/B/A/CopyTexture.dbulk"));
	FPackagePath CopiedPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/B/A/CopyTexture", CopiedPath));
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
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch Batch) { ObservedChanges = Batch; ++Notifications; }});
	Observe(Service);
	bool Result = false;
	FContentBrowserItem ResultItem;
	{
		Testing::FScopedPackageSaveFault Fault([](auto Phase, size_t) {
			return Phase == Testing::EPackageSaveFaultPoint::PublishRegistry;
		});
		Result = Service.BulkSaveItems(std::array{AssetItem(Path)});
	}
	EXPECT_FALSE(Result);
	EXPECT_TRUE((ObservedChanges.bFullRefresh || !ObservedChanges.Changes.empty()));
	EXPECT_EQ(Notifications, 1);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/SaveProjectionPending.dasset"));
	EXPECT_FALSE(Asset->GetPackage()->IsDirty());
	EXPECT_TRUE(IsAssetRegistryProjectionFenced(Path));
	EXPECT_NE(OperationError.find("ContentCommittedProjectionPending"), std::string::npos);
	EXPECT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::Incremental));
}

TEST_F(FContentBrowserModelTests, CapabilityQueriesExplainRejectionWithoutExecutingOrReporting)
{
	const auto File = Root / "Content/A/capability.txt";
	std::ofstream(File) << "original";
	FContentBrowserItem Item{.Kind = EContentBrowserItemKind::File, .Name = "capability.txt", .PhysicalPath = File.generic_string()};
	bool bAllowed = true;
	int Updates = 0;
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++Updates; }, .CanMutateContent = [&] { return bAllowed; }});
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

TEST_F(FContentBrowserModelTests, SuccessfulNoOpFixUpDoesNotPublishItemUpdates)
{
	int Updates = 0;
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++Updates; }});
	Observe(Service);
	EXPECT_TRUE(Service.FixUpRedirectorsInFolder("/ContentBrowserTests/A/"));
	EXPECT_EQ(Updates, 0);
	EXPECT_TRUE(OperationError.empty());
}

TEST_F(FContentBrowserModelTests, RejectedDuplicateClearsExplicitOutputWithoutPublishingUpdates)
{
	int Updates = 0;
	FContentBrowserRequestContext Service({.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++Updates; }});
	Observe(Service);
	FContentBrowserItem Output{.Kind = EContentBrowserItemKind::Asset, .VirtualPath = "stale", .PhysicalPath = "stale"};
	EXPECT_FALSE(Service.DuplicateItem(FContentBrowserItem{}, (Root / "Content/B").generic_string(), Output));
	EXPECT_TRUE(TestVirtualPath(Output).empty());
	EXPECT_TRUE(TestPhysicalPath(Output).empty());
	EXPECT_FALSE(OperationError.empty());
	EXPECT_EQ(Updates, 0);
}

TEST_F(FContentBrowserModelTests, DataSourceShutdownCancelsPublicationAndRejectsNewCaptures)
{
	const auto Directory = Root / "Content/A";
	std::filesystem::create_directory(Directory / "Child");
	auto Gate = std::make_shared<FBrowserScanGate>();
	FNativeContentBrowserTestQuery Source;
	FContentBrowserModel Model(Source, true);
	FBrowserScanRelease Release{Gate};
	Model.SetEntryStatusQueryForTesting([Gate](const auto& Entry, auto& Error) {
		Gate->Pause();
		return Entry.symlink_status(Error);
	});
	ASSERT_TRUE(Model.NavigateToPhysical(Directory.generic_string()));
	ASSERT_TRUE(Gate->WaitUntilStarted());
	const auto Requests = Model.GetItemsRequestCount();
	const auto Version = Model.GetSnapshotVersion();
	Source.StopRequestAdmission();
	EXPECT_FALSE(Source.PrepareItemsCapture("/ContentBrowserTests/", false));
	Model.RefreshItemsSnapshot();
	Model.RequestDirectoryChildrenSnapshot(Directory.generic_string());
	Gate->Release();
	Model.WaitForPendingSnapshotsForTesting();
	EXPECT_FALSE(Model.IsLoading());
	EXPECT_TRUE(Model.GetItems().empty());
	EXPECT_EQ(Model.GetSnapshotVersion(), Version);
	EXPECT_EQ(Model.GetItemsRequestCount(), Requests);
	EXPECT_FALSE(Model.HasDirectoryChildrenSnapshot(Directory.generic_string()));
	FContentBrowserModel ClosedModel(Source);
	ClosedModel.RefreshItemsSnapshot();
	ClosedModel.RefreshRequestedDirectoryChildrenSnapshots();
	EXPECT_EQ(ClosedModel.GetItemsRequestCount(), 0u);
}

TEST_F(FContentBrowserModelTests, FolderValidationIsSilentAndFinalizationRechecksDestination)
{
	int Diagnostics = 0;
	int ScopedUpdates = 0;
	int LegacyUpdates = 0;
	FContentBrowserRequestContext Source({
		.NotifyMountedContentMutation = [&] { ++LegacyUpdates; },
		.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++ScopedUpdates; }});
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
	EXPECT_EQ(LegacyUpdates, 0);
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
	FContentBrowserRequestContext Source({.NotifyScopedContentMutation = [&](FContentChangeBatch) { ++Updates; }});
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
		FNativeContentBrowserTestQuery Source;
		Work = Source.PrepareItemsCapture("/ContentBrowserTests/A/", false, {});
		TreeWork = Source.PrepareDirectoryCapture("/ContentBrowserTests/A/", {});
		Source.StopRequestAdmission();
		EXPECT_FALSE(Source.PrepareItemsCapture("/ContentBrowserTests/A/", false, {}));
		EXPECT_FALSE(Source.PrepareDirectoryCapture("/ContentBrowserTests/A/", {}));
	}
	ASSERT_TRUE(Work);
	auto Snapshot = Work();
	ASSERT_EQ(Snapshot->Items.size(), 1u);
	EXPECT_EQ(TestName(Snapshot->Items.front()), "Child");
	ASSERT_TRUE(TreeWork);
	EXPECT_EQ(TreeWork()->Children.size(), 1u);
}

TEST_F(FContentBrowserModelTests, DirectoryCapturesPrepareCurrentCatalogWithoutItemRefresh)
{
	InitializeDObjectSystem();
	for (const bool bAsync : {false, true})
	{
		const std::string Folder = bAsync ? "TreeAsync" : "TreeSync";
		const auto Physical = Root / "Content" / Folder;
		FPackagePath FirstPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/" + Folder + "/First/Material", FirstPath));
		DMaterial* First = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(FirstPath, First));
		FNativeContentBrowserTestQuery Source;
		FContentBrowserModel Model(Source, bAsync);
		// No navigation or item capture has prepared a catalog yet.
		Model.RequestDirectoryChildrenSnapshot(Physical.generic_string());
		Model.RefreshRequestedDirectoryChildrenSnapshots();
		Model.WaitForPendingSnapshotsForTesting();
		auto Tree = Model.GetDirectorySnapshot(Physical.generic_string());
		ASSERT_NE(Tree, nullptr);
		ASSERT_EQ(Tree->Children.size(), 1u);
		EXPECT_EQ(Tree->Children.front(), Physical / "First");

		ASSERT_TRUE(Model.NavigateToPhysical(Physical.generic_string()));
		Model.WaitForPendingSnapshotsForTesting();
		FPackagePath LaterPath;
		ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/" + Folder + "/Later/Material", LaterPath));
		DMaterial* Later = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(LaterPath, Later));
		// The item catalog predates Later; tree preparation must refresh its owned inputs.
		Model.RequestDirectoryChildrenSnapshot(Physical.generic_string());
		Model.RefreshRequestedDirectoryChildrenSnapshots();
		Model.WaitForPendingSnapshotsForTesting();
		Tree = Model.GetDirectorySnapshot(Physical.generic_string());
		ASSERT_NE(Tree, nullptr);
		EXPECT_EQ(Tree->Children.size(), 2u);
		EXPECT_TRUE(std::ranges::contains(Tree->Children, Physical / "Later"));
		EXPECT_FALSE(std::filesystem::exists(Physical));
	}
}

TEST_F(FContentBrowserModelTests, SaveCapabilitiesFollowModeAndCurrentMountPolicy)
{
	InitializeDObjectSystem();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ContentBrowserTests/A/SavePolicy", Path));
	DMaterial* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	Asset->GetPackage()->MarkDirty();
	const auto Item = AssetItem(Path);
	FContentBrowserRequestContext Source;
	Observe(Source);
	EXPECT_TRUE(Source.CanSaveItem(Item));
	EXPECT_FALSE(Source.CanSaveItem(Item, EAssetSaveMode::CanonicalResave));
	ASSERT_TRUE(Source.BulkSaveItems(std::span(&Item, 1)));
	EXPECT_FALSE(Source.CanSaveItem(Item));
	EXPECT_TRUE(Source.CanSaveItem(Item, EAssetSaveMode::CanonicalResave));
	Asset->GetPackage()->MarkDirty();
	Registry.reset();
	const std::array Definitions{FMountPoint{.VirtualRoot = "/ContentBrowserTests/",
		.Owner = EMountOwner::Test, .Root = Root / "Content", .bAutoScan = true, .bContentWritable = false}};
	Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(Definitions);
	ASSERT_TRUE(Registry->IsValid());
	std::string Reason;
	EXPECT_FALSE(Source.CanSaveItem(Item, EAssetSaveMode::LoadedDirtyPackage, &Reason));
	EXPECT_FALSE(Reason.empty());
	EXPECT_FALSE(Source.CanSaveItem(Item, EAssetSaveMode::CanonicalResave));
	EXPECT_TRUE(OperationError.empty());
	EXPECT_FALSE(Source.BulkSaveItems(std::span(&Item, 1)));
	EXPECT_TRUE(Asset->GetPackage()->IsDirty());
}

TEST_F(FContentBrowserModelTests, NonphysicalProviderUsesNormalListTreeNavigationSearchAndCommands)
{
	FNativeContentBrowserTestQuery Query;
	auto *Source = NewObject<FVirtualBrowserQuerySource>(nullptr, {});
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
					{.Kind = EContentBrowserItemKind::Folder, .Name = "Sub", .VirtualPath = "/Memory/Sub"},
					{.Name = "Leaf", .VirtualPath = "/Memory/Sub/Leaf"}});
	Query.AddSource(*Source);
	FContentBrowserModel First(Query, true), Second(Query, true);
	ASSERT_TRUE(First.NavigateToVirtual("/Memory"));
	ASSERT_TRUE(Second.NavigateToVirtual("/Memory/Sub"));
	First.SetSearch("Leaf");
	First.CancelPendingSnapshots();
	Second.WaitForPendingSnapshotsForTesting();
	ASSERT_EQ(Second.GetItems().size(), 1u);
	EXPECT_TRUE(TestPhysicalPath(Second.GetItems().front()).empty());
	EXPECT_EQ(ContentBrowserQuery::TypeLabel(Second.GetItems().front()), "Memory object");
	EXPECT_TRUE(Second.GetItems().front().Copy("/Memory/Target"));
	EXPECT_EQ(Source->Copies, 1);
	First.SetSearch("Leaf");
	First.RefreshItemsSnapshot();
	First.WaitForPendingSnapshotsForTesting();
	ASSERT_EQ(First.GetItems().size(), 1u);
	EXPECT_EQ(TestVirtualPath(First.GetItems().front()), "/Memory/Sub/Leaf");
	First.RequestDirectoryChildrenSnapshot("/");
	First.RequestDirectoryChildrenSnapshot("/Memory");
	First.WaitForPendingSnapshotsForTesting();
	const auto Roots = First.GetDirectorySnapshot("/");
	ASSERT_NE(Roots, nullptr);
	EXPECT_TRUE(std::ranges::contains(Roots->Children, std::filesystem::path("/Memory")));
	const auto Tree = First.GetDirectorySnapshot("/Memory");
	ASSERT_NE(Tree, nullptr);
	ASSERT_EQ(Tree->Children.size(), 1u);
	EXPECT_EQ(Tree->Children.front(), std::filesystem::path("/Memory/Sub"));
}

TEST_F(FContentBrowserModelTests, PreparedProviderCapturesRejectReactivationAndStaleTreeData)
{
	FNativeContentBrowserTestQuery Query;
	auto *Source = NewObject<FVirtualBrowserQuerySource>(nullptr, {});
	Source->Rows = ItemDescriptions({{.Kind = EContentBrowserItemKind::Folder, .Name = "Memory", .VirtualPath = "/Memory"},
					{.Name = "Leaf", .VirtualPath = "/Memory/Leaf"}});
	Query.AddSource(*Source);
	auto Work = Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Work);
	Source->UnregisterDataSource();
	Query.AddSource(*Source);
	auto Old = Query.ProjectItems(std::move(*Work()), "/Memory", false);
	EXPECT_TRUE(Old.bStale);
	EXPECT_TRUE(Old.Items.empty());
	auto TreeWork = Query.PrepareDirectoryCapture("/Memory");
	ASSERT_TRUE(TreeWork);
	Source->Rows.push_back(Durin::ContentBrowser::Private::FContentBrowserNativeItem{.Kind = EContentBrowserItemKind::Folder, .Name = "Later", .VirtualPath = "/Memory/Later"});
	++Source->Revision;
	auto OldTree = Query.ProjectDirectory(std::move(*TreeWork()), "/Memory");
	EXPECT_TRUE(OldTree.bStale);
	EXPECT_TRUE(OldTree.Children.empty());
	FContentBrowserModel Model(Query, true);
	ASSERT_TRUE(Model.NavigateToVirtual("/Memory"));
	Model.RequestDirectoryChildrenSnapshot("/Memory");
	Model.WaitForPendingSnapshotsForTesting();
	ASSERT_EQ(Model.GetItems().size(), 2u);
	const auto Tree = Model.GetDirectorySnapshot("/Memory");
	ASSERT_NE(Tree, nullptr);
	ASSERT_EQ(Tree->Children.size(), 1u);
}

TEST_F(FContentBrowserModelTests, TwoServiceQueriesShareProviderDataWithoutSharingAdmissionOrCancellation)
{
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
	FContentBrowserDataQuery FirstQuery({.DataSubsystem = Data}), SecondQuery({.DataSubsystem = Data});
	FContentBrowserModel First(FirstQuery, true), Second(SecondQuery, true);
	ASSERT_TRUE(First.NavigateToVirtual("/Memory"));
	ASSERT_TRUE(Second.NavigateToVirtual("/Memory"));
	FirstQuery.StopRequestAdmission();
	First.WaitForPendingSnapshotsForTesting();
	Second.WaitForPendingSnapshotsForTesting();
	EXPECT_TRUE(First.GetItems().empty());
	ASSERT_EQ(Second.GetItems().size(), 1u);
	EXPECT_EQ(TestVirtualPath(Second.GetItems().front()), "/Memory/Leaf");
	EXPECT_TRUE(Second.GetItems().front().Copy("/Memory/Target"));
	EXPECT_EQ(Scope.Source->Copies, 1);
}

TEST_F(FContentBrowserModelTests, OwnerBindingCannotRebindOldWorkerRowsToANewActivation)
{
	FNativeContentBrowserTestQuery Query;
	auto* Source=NewObject<FVirtualBrowserQuerySource>(nullptr, {});
	Source->Rows=Durin::ContentBrowser::Private::ItemDescriptions({{.Name="Leaf", .VirtualPath="/Memory/Leaf"}});
	Source->UseCompiledCapture=true;
	Query.AddSource(*Source);
	auto Work=Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Work);
	Source->BeforeBind=[&] { Source->UnregisterDataSource(); Query.AddSource(*Source); };
	auto Old=Query.ProjectItems(std::move(*Work()), "/Memory", false);
	EXPECT_TRUE(Old.bStale);
	EXPECT_TRUE(Old.Items.empty());
	auto Fresh=Query.PrepareItemsCapture("/Memory", false);
	ASSERT_TRUE(Fresh);
	EXPECT_EQ(Query.ProjectItems(std::move(*Fresh()), "/Memory", false).Items.size(), 1u);
}

TEST_F(FContentBrowserModelTests, SourceImagePreviewUsesProviderAttributesRatherThanLegacyMetadataFields)
{
	FNativeContentBrowserTestQuery Query;
	auto* Source=NewObject<FVirtualBrowserQuerySource>(nullptr, {});
	Source->PreviewPath=(Root/"Preview.png").generic_string(); Query.AddSource(*Source);
	Durin::ContentBrowser::FContentBrowserItem Item;
	Item.InternalItems.emplace_back(*Source, Durin::ContentBrowser::EContentBrowserItemFlags::Type_File | Durin::ContentBrowser::EContentBrowserItemFlags::Category_Misc, "/Memory/Preview", std::string{}, std::string{}, nullptr);
	EXPECT_EQ(TestPhysicalPath(Item), Source->PreviewPath); EXPECT_EQ(TestExtension(Item), ".png");
	FContentBrowserThumbnailReferences References; References.BeginFrame();
	References.Request(Item, EAssetThumbnailPriority::Visible);
	EXPECT_EQ(References.Find(Item.GetItemKey()).State, EAssetThumbnailState::Queued);
}
