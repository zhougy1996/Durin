#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserOperations.h"

#include "Assets/AssetRelocationTransaction.h"
#include "Editor/Transaction.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor::Level;

	class FContentBrowserModelTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			Root = Durin::Testing::GetTestWorkDirectory()
				/ "ContentBrowserModel";
			std::error_code Ec;
			Durin::Testing::RemoveTestWorkDirectory(Root, Ec);
			std::filesystem::create_directories(Root / "Content/A");
			std::filesystem::create_directories(Root / "Content/B");
			const std::array Definitions{
				PathUtilities::FMountPoint{
					.VirtualRoot = "/ContentBrowserTests/",
					.Owner = PathUtilities::EMountOwner::Test,
					.Root = Root / "Content",
					.bAutoScan = true,
					.bAuthoringWritable = true}};
			Registry =
				std::make_unique<PathUtilities::FScopedMountRegistryFixture>(
					Definitions);
			ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
		}

		std::filesystem::path Root;
		std::unique_ptr<PathUtilities::FScopedMountRegistryFixture> Registry;
	};

	auto BuildTransactionPlan(
		FContentBrowserOperations& Operations,
		std::span<const FContentBrowserItem> Items,
		const std::filesystem::path& StagingRoot)
		-> FContentDeletionPlanPtr
	{
		std::unordered_set<std::string> Selection;
		for (const FContentBrowserItem& Item : Items)
			Selection.insert(Item.StableId());
		const FContentDeletionPlanPtr Analyzed =
			Operations.BuildDeletionPlan(Items, Selection);
		auto Mutable = std::make_shared<FContentDeletionPlan>(*Analyzed);
		Mutable->StagingVolumeRoot = StagingRoot.generic_string();
		return Mutable;
	}

	class FNoOpEditorTransaction final : public Durin::Editor::ITransaction
	{
	public:
		auto GetDescription() const -> std::string_view override { return "No-op"; }
		auto Undo() -> bool override { return true; }
		auto Redo() -> bool override { return true; }
	};

	class FRouteOnlyThumbnailProvider final : public Editor::IAssetThumbnailProvider
	{
	public:
		explicit FRouteOnlyThumbnailProvider(
			std::string InAssetClassName,
			bool bInUsesSourceImage = false)
			: AssetClassName(std::move(InAssetClassName))
			, bUsesSourceImage(bInUsesSourceImage)
		{
		}
		auto UsesSourceImage() const -> bool override { return bUsesSourceImage; }

		auto GetRegistration() const -> Editor::FAssetThumbnailProviderRegistration override
		{
			return {
				.AssetClassName = AssetClassName,
				.ProviderName = "ContentBrowserRouteTest",
				.GeneratorSchemaVersion = 1};
		}

		auto CaptureGenerationRequest(
			const Editor::FAssetThumbnailRequest&,
			uint64,
			Editor::FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool override
		{
			OutRequest = {};
			OutError = "Route-only test provider.";
			return false;
		}

	private:
		std::string AssetClassName;
		bool bUsesSourceImage = false;
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

TEST_F(FContentBrowserModelTests, RoutesStaticMeshAssetsToRenderedThumbnails)
{
	InitializeDObjectSystem();
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/ThumbnailMesh", AssetPath));
	DStaticMesh* StaticMesh = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(AssetPath, StaticMesh));
	ASSERT_TRUE(Asset::SavePackage(StaticMesh->GetPackage()));
	const Asset::FAssetCatalogEntry AssetData =
		Asset::FindAssetExact(AssetPath);
	ASSERT_NE(AssetData, nullptr);
	std::string RegistrationError;
	auto ThumbnailRegistration =
		Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<FRouteOnlyThumbnailProvider>(AssetData->AssetClassName),
			RegistrationError);
	ASSERT_TRUE(ThumbnailRegistration) << RegistrationError;

	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const auto It = std::ranges::find_if(
		Model.GetItems(),
		[&](const FContentBrowserItem& Item) {
			return Item.VirtualPath == AssetPath.ToString();
		});
	ASSERT_NE(It, Model.GetItems().end());
	EXPECT_EQ(It->ThumbnailIdentity, AssetPath.ToString());
	EXPECT_TRUE(It->ThumbnailSourcePath.empty());
	EXPECT_EQ(It->ThumbnailFileSize, AssetData->FileSize);
	EXPECT_EQ(It->ThumbnailPackageFormatVersion, AssetData->FormatVersion);
	EXPECT_EQ(It->ThumbnailLastWriteTimeTicks, AssetData->LastWriteTimeTicks);

	ASSERT_TRUE(Asset::DeleteAssetForTesting(AssetPath));
}

TEST_F(FContentBrowserModelTests, SourceProviderWithoutUsableSourceKeepsAssetIcon)
{
	InitializeDObjectSystem();
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/SourceLessMesh", AssetPath));
	DStaticMesh* StaticMesh = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(AssetPath, StaticMesh));
	ASSERT_TRUE(Asset::SavePackage(StaticMesh->GetPackage()));
	const Asset::FAssetCatalogEntry AssetData =
		Asset::FindAssetExact(AssetPath);
	ASSERT_NE(AssetData, nullptr);
	std::string Error;
	auto Registration =
		Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<FRouteOnlyThumbnailProvider>(
				AssetData->AssetClassName, true),
			Error);
	ASSERT_TRUE(Registration) << Error;

	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const auto It = std::ranges::find_if(
		Model.GetItems(),
		[&](const FContentBrowserItem& Item) {
			return Item.VirtualPath == AssetPath.ToString();
		});
	ASSERT_NE(It, Model.GetItems().end());
	EXPECT_TRUE(It->ThumbnailIdentity.empty());
	EXPECT_TRUE(It->ThumbnailSourcePath.empty());
	ASSERT_TRUE(Asset::DeleteAssetForTesting(AssetPath));
}

TEST_F(FContentBrowserModelTests, RejectsExcludedMountsAndClearsStaleCurrentDirectory)
{
	Registry.reset();
	std::filesystem::create_directories(Root / "Excluded");
	const std::array Definitions{
		PathUtilities::FMountPoint{
			.VirtualRoot = "/ContentBrowserTests/",
			.Owner = PathUtilities::EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bAuthoringWritable = true},
		PathUtilities::FMountPoint{
			.VirtualRoot = "/ContentBrowserExcluded/",
			.Owner = PathUtilities::EMountOwner::Test,
			.Root = Root / "Excluded",
			.bAutoScan = false,
			.bAuthoringWritable = true}};
	Registry = std::make_unique<PathUtilities::FScopedMountRegistryFixture>(
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

	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
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
		PathUtilities::FMountPoint{
			.VirtualRoot = "/ContentBrowserReplacement/",
			.Owner = PathUtilities::EMountOwner::Test,
			.Root = Root / "Replacement",
			.bAutoScan = true,
			.bAuthoringWritable = true}};
	Registry = std::make_unique<PathUtilities::FScopedMountRegistryFixture>(
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
		PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = PathUtilities::EMountOwner::Engine,
			.Root = Root / "EngineContent",
			.bAutoScan = true},
		PathUtilities::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = PathUtilities::EMountOwner::ActiveProject,
			.Root = Root / "GameContent",
			.bAutoScan = true}};
	Registry = std::make_unique<PathUtilities::FScopedMountRegistryFixture>(
		Definitions);
	ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	ASSERT_EQ(Model.GetMounts().size(), 2u);
	EXPECT_EQ(Model.GetMounts()[0].VirtualRoot, "/Game/");
	EXPECT_EQ(Model.GetMounts()[1].VirtualRoot, "/Engine/");
}

TEST_F(FContentBrowserModelTests, RelocationUsesOneSharedUndoRedoTransaction)
{
	InitializeDObjectSystem();
	FAssetPath SourcePath;
	FAssetPath DestinationPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/TransactionalSource", SourcePath));
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/Folder/TransactionalDestination",
		DestinationPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(SourcePath, Material));
	ASSERT_TRUE(Asset::SavePackage(Material->GetPackage()));

	const Asset::FAssetRelocationMapping Mapping{
		SourcePath, DestinationPath};
	Asset::FAssetMutationSummary Summary;
	Asset::FAssetMutationTransaction Transaction;
	ASSERT_TRUE(Asset::PrepareAssetRelocationTransaction(
		std::span{&Mapping, 1}, Summary, Transaction));
	ASSERT_TRUE(Transaction.Commit());
	Durin::Editor::FTransactionManager Transactions;
	ASSERT_TRUE(Transactions.CommitApplied(
		std::make_unique<FAssetRelocationTransaction>(std::move(Transaction))));
	EXPECT_EQ(Transactions.GetUndoDescription(), "Move Asset");
	EXPECT_EQ(Asset::ResolveAssetPath(SourcePath).FinalPath,
		DestinationPath);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Asset::FindAssetExact(SourcePath)->EntryKind,
		Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Asset::FindAssetExact(DestinationPath), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Asset::ResolveAssetPath(SourcePath).FinalPath,
		DestinationPath);
	Transactions.Clear();
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

TEST_F(FContentBrowserModelTests, SkipsFailedTreeEntryAndReportsTraversalFailure)
{
	const std::filesystem::path TreeRoot = Root / "Content/EnumerationTree";
	for (const std::string_view Name : {"First", "Second", "Third"})
		std::filesystem::create_directories(TreeRoot / Name);
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical(TreeRoot.generic_string()));
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
	EXPECT_EQ(Model.GetDirectoryChildren(TreeRoot.generic_string()).size(), 2);
	ASSERT_EQ(Model.GetEnumerationDiagnostics().size(), 1);
	EXPECT_EQ(
		Model.GetEnumerationDiagnostics().front().Kind,
		FContentBrowserModel::EEnumerationDiagnosticKind::Entry);

	EXPECT_TRUE(Model.GetDirectoryChildren(
		(Root / "MissingDirectory").generic_string()).empty());
	ASSERT_EQ(Model.GetEnumerationDiagnostics().size(), 2);
	EXPECT_EQ(
		Model.GetEnumerationDiagnostics().back().Kind,
		FContentBrowserModel::EEnumerationDiagnosticKind::Traversal);
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

TEST_F(FContentBrowserModelTests, GroupsExactSkeletalAssetTypes)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FContentBrowserModel Model;
	Model.SetSnapshotForTesting(RootPath, {
		{.Kind = EContentBrowserItemKind::Asset, .Name = "Rig",
			.PhysicalPath = RootPath + "/Rig.dasset", .AssetClassName = "Durin::DSkeleton"},
		{.Kind = EContentBrowserItemKind::Asset, .Name = "Body",
			.PhysicalPath = RootPath + "/Body.dasset", .AssetClassName = "Durin::DSkeletalMesh"},
		{.Kind = EContentBrowserItemKind::Asset, .Name = "Walk",
			.PhysicalPath = RootPath + "/Walk.dasset", .AssetClassName = "Durin::DAnimationClip"},
		{.Kind = EContentBrowserItemKind::Asset, .Name = "Prop",
			.PhysicalPath = RootPath + "/Prop.dasset", .AssetClassName = "Durin::DStaticMesh"}});
	Model.SetTypeFilter(EContentBrowserTypeFilter::SkeletalAssets);
	ASSERT_EQ(Model.GetItems().size(), 3u);
	EXPECT_TRUE(std::ranges::all_of(Model.GetItems(), [](const FContentBrowserItem& Item) {
		const std::string Type = ContentBrowserModel::TypeLabel(Item);
		return Type == "Skeleton" || Type == "Skeletal Mesh" || Type == "Animation Clip";
	}));
}

TEST_F(FContentBrowserModelTests, HidesRedirectorsButPreservesFoldersAndSupportsExplicitFilter)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FAssetPath Destination;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/Final/Stone", Destination));
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
				.VirtualPath = "/ContentBrowserTests/OldStone",
				.PhysicalPath = RootPath + "/OldStone.dasset",
				.AssetClassName = "Durin::Asset::DAssetRedirector",
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
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
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

TEST_F(FContentBrowserModelTests, UnclaimedSameStemFileRenamesIndependently)
{
	InitializeDObjectSystem();
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/Independent", AssetPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(AssetPath, Material));
	ASSERT_TRUE(Asset::SavePackage(Material->GetPackage()));
	const std::filesystem::path FilePath = Root / "Content/Independent.txt";
	{
		std::ofstream File(FilePath);
		File << "ordinary";
	}
	Asset::FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(Asset::QueryAssetCompanionOwnership(FilePath, Ownership));
	EXPECT_EQ(
		Ownership.State,
		Asset::EAssetCompanionOwnershipState::Unclaimed);

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
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
	ASSERT_TRUE(Asset::DeleteAssetForTesting(AssetPath));
}

TEST_F(FContentBrowserModelTests, OwnedCompanionIsProtectedAndIncompleteFolderMoveFails)
{
	InitializeDObjectSystem();
	const std::filesystem::path Folder = Root / "Content/OwnedFolder";
	std::filesystem::create_directories(Folder);
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/OwnedFolder/Owned", AssetPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(AssetPath, Material));
	ASSERT_TRUE(Asset::SavePackage(Material->GetPackage()));
	const std::filesystem::path Companion = Folder / "Owned.meta";
	{
		std::ofstream File(Companion);
		File << "owned";
	}
	struct FContributorReset
	{
		Asset::FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { Asset::UnregisterAssetDeleteContributor(Handle); }
	};
	const Asset::FAssetDeleteContributorHandle Contributor =
	Asset::RegisterAssetDeleteContributor(
		DMaterial::StaticClass(),
		[AssetPath, Companion](const Asset::FAssetData& Data,
			const Asset::FAssetPackageInspection&,
			Asset::FAssetDeleteContribution& Contribution) -> Asset::FAssetResult {
			if (Data.PackagePath == AssetPath)
				Contribution.Files.push_back(Companion);
			return {};
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};

	Asset::FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(Asset::QueryAssetCompanionOwnership(Companion, Ownership));
	ASSERT_EQ(Ownership.State, Asset::EAssetCompanionOwnershipState::Owned);
	ASSERT_EQ(Ownership.Owners.size(), 1);
	EXPECT_EQ(Ownership.Owners.front(), AssetPath);
	bool bMoveCalled = false;
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model, [&](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
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
	EXPECT_FALSE(FolderResult);
	EXPECT_TRUE(bMoveCalled);
	EXPECT_TRUE(FolderResult.Status.Message.find("source folder")
		!= std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Folder));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(AssetPath));
}

TEST_F(FContentBrowserModelTests, RejectsOrdinaryMutationsInReadOnlyMount)
{
	Registry.reset();
	const std::array Definitions{
		PathUtilities::FMountPoint{
			.VirtualRoot = "/ContentBrowserReadOnly/",
			.Owner = PathUtilities::EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bAuthoringWritable = false}};
	Registry = std::make_unique<PathUtilities::FScopedMountRegistryFixture>(
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
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});

	const FContentBrowserOperationResult CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string());
	EXPECT_FALSE(CreateResult);
	EXPECT_EQ(CreateResult.Status.Error, Asset::EAssetError::ReadOnlyMode);
	EXPECT_TRUE(CreateResult.Status.Message.find("read-only") != std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/New Folder"));

	const FContentBrowserItem FileItem{
		.Kind = EContentBrowserItemKind::File,
		.Name = "notes.txt",
		.PhysicalPath = File.generic_string(),
		.Extension = ".txt"};
	const FContentBrowserOperationResult FileResult =
		Operations.Rename(FileItem, "renamed.txt");
	EXPECT_FALSE(FileResult);
	EXPECT_EQ(FileResult.Status.Error, Asset::EAssetError::ReadOnlyMode);
	EXPECT_TRUE(std::filesystem::exists(File));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/renamed.txt"));

	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "WritableOrdinaryFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentBrowserOperationResult FolderResult =
		Operations.Rename(FolderItem, "RenamedFolder");
	EXPECT_FALSE(FolderResult);
	EXPECT_EQ(FolderResult.Status.Error, Asset::EAssetError::ReadOnlyMode);
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
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
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
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {
				Asset::EAssetError::IoError,
				"Injected move failure."};
		});
	const FContentBrowserItem AssetItem{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "Old",
		.VirtualPath = "/ContentBrowserTests/Old",
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
		Operations.BuildDeletionPlan(Items, Selection);
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

	const FContentBrowserItem InvalidAsset{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "Invalid",
		.VirtualPath = "not-an-asset-path",
		.PhysicalPath = (Root / "Content/Invalid.dasset").generic_string()};
	std::vector<std::pair<std::string, Asset::FAssetDeleteAnalysis>> Analyses;
	std::vector<std::pair<std::string, Asset::FAssetResult>> Errors;
	Operations.AnalyzeDeletion(
		std::span{&InvalidAsset, 1},
		std::unordered_set<std::string>{InvalidAsset.StableId()},
		Analyses,
		Errors);
	EXPECT_TRUE(Analyses.empty());
	ASSERT_EQ(Errors.size(), 1);
	EXPECT_EQ(Errors.front().second.Message, "Asset path is invalid.");
}

TEST_F(FContentBrowserModelTests, RefreshesSnapshotAfterFolderMutation)
{
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	const size_t ChildrenBefore = Model.GetDirectoryChildren(
		(Root / "Content").generic_string()).size();
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});

	const FContentBrowserOperationResult CreateResult =
		Operations.CreateFolder((Root / "Content").generic_string());
	ASSERT_TRUE(CreateResult) << CreateResult.Status.Message;
	Model.RefreshItemsSnapshot();
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
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
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
		Operations.BuildDeletionPlan(Items, Selection);
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
		Operations.BuildDeletionPlan(Items, Selection);
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
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "UnknownPackage",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&FolderItem, 1},
		std::unordered_set<std::string>{FolderItem.StableId()});

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
		PathUtilities::FMountPoint{
			.VirtualRoot = "/ContentDeletionReadOnly/",
			.Owner = PathUtilities::EMountOwner::Test,
			.Root = Root / "Content",
			.bAutoScan = true,
			.bAuthoringWritable = false}};
	PathUtilities::FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Folder{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Folder",
		.PhysicalPath = (Root / "Content/Folder").generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&Folder, 1},
		std::unordered_set<std::string>{Folder.StableId()});
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
		std::span{&MountRoot, 1},
		std::unordered_set<std::string>{MountRoot.StableId()});
	EXPECT_TRUE(std::ranges::any_of(
		MountRootPlan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::MountRoot;
		}));
}

TEST_F(FContentBrowserModelTests, BatchAnalysisExcludesInternalReferences)
{
	InitializeDObjectSystem();
	FAssetPath BasePath;
	FAssetPath InternalPath;
	FAssetPath ExternalPath;
	ASSERT_TRUE(FAssetPath::TryCreate("/ContentBrowserTests/Base", BasePath));
	ASSERT_TRUE(FAssetPath::TryCreate("/ContentBrowserTests/Internal", InternalPath));
	ASSERT_TRUE(FAssetPath::TryCreate("/ContentBrowserTests/External", ExternalPath));
	DMaterial* Base = nullptr;
	DMaterialInstance* Internal = nullptr;
	DMaterialInstance* External = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(BasePath, Base));
	ASSERT_TRUE(Asset::CreateAsset(InternalPath, Internal));
	ASSERT_TRUE(Asset::CreateAsset(ExternalPath, External));
	ASSERT_TRUE(Internal->SetParent(Base));
	ASSERT_TRUE(External->SetParent(Base));
	ASSERT_TRUE(Asset::SavePackage(Base->GetPackage()));
	ASSERT_TRUE(Asset::SavePackage(Internal->GetPackage()));
	ASSERT_TRUE(Asset::SavePackage(External->GetPackage()));

	Asset::FAssetDeletionTransaction Transaction;
	std::vector<Asset::FAssetDeletionBatchBlocker> Blockers;
	const std::array PartialPaths{BasePath, InternalPath};
	ASSERT_TRUE(Asset::PrepareAssetDeletionTransaction(
		PartialPaths, {}, Transaction, Blockers));
	EXPECT_TRUE(std::ranges::any_of(
		Blockers,
		[&](const Asset::FAssetDeletionBatchBlocker& Blocker) {
			return Blocker.AssetPath == BasePath
				&& Blocker.RelatedAssetPath == ExternalPath;
		}));

	const std::array CompletePaths{BasePath, InternalPath, ExternalPath};
	ASSERT_TRUE(Asset::PrepareAssetDeletionTransaction(
		CompletePaths, {}, Transaction, Blockers));
	EXPECT_TRUE(Blockers.empty());
	EXPECT_EQ(Transaction.GetEntries().size(), 3);

	ASSERT_TRUE(Asset::UnloadPackage(ExternalPath));
	ASSERT_TRUE(Asset::UnloadPackage(InternalPath));
	ASSERT_TRUE(Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(ExternalPath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(InternalPath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(BasePath));
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
		Asset::FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { Asset::UnregisterAssetDeleteContributor(Handle); }
	};
	const Asset::FAssetDeleteContributorHandle Contributor =
	Asset::RegisterAssetDeleteContributor(
		DMaterial::StaticClass(),
		[SharedCompanion](const Asset::FAssetData& Data,
			const Asset::FAssetPackageInspection&,
			Asset::FAssetDeleteContribution& Contribution) -> Asset::FAssetResult {
			if (Data.PackagePath.GetView().starts_with(
					"/ContentBrowserTests/Companion"))
				Contribution.Files.push_back(SharedCompanion);
			return {};
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};

	FAssetPath FirstPath;
	FAssetPath SecondPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/CompanionFirst", FirstPath));
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/CompanionSecond", SecondPath));
	DMaterial* First = nullptr;
	DMaterial* Second = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(FirstPath, First));
	ASSERT_TRUE(Asset::CreateAsset(SecondPath, Second));
	ASSERT_TRUE(Asset::SavePackage(First->GetPackage()));
	ASSERT_TRUE(Asset::SavePackage(Second->GetPackage()));
	Asset::FAssetCompanionOwnership Ownership;
	ASSERT_TRUE(Asset::QueryAssetCompanionOwnership(
		SharedCompanion, Ownership));
	EXPECT_EQ(
		Ownership.State,
		Asset::EAssetCompanionOwnershipState::Ambiguous);
	EXPECT_EQ(Ownership.Owners.size(), 2);

	Asset::FAssetDeletionTransaction Transaction;
	std::vector<Asset::FAssetDeletionBatchBlocker> Blockers;
	const std::array Paths{FirstPath, SecondPath};
	ASSERT_TRUE(Asset::PrepareAssetDeletionTransaction(
		Paths, {}, Transaction, Blockers));
	EXPECT_TRUE(std::ranges::any_of(
		Blockers,
		[](const Asset::FAssetDeletionBatchBlocker& Blocker) {
			return Blocker.Kind
				== Asset::EAssetDeletionBatchBlocker::CompanionOwnershipConflict;
		}));

	ASSERT_TRUE(Asset::UnloadPackage(SecondPath));
	ASSERT_TRUE(Asset::UnloadPackage(FirstPath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(SecondPath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(FirstPath));
}

TEST_F(FContentBrowserModelTests, BatchRevalidationDetectsNewExternalReference)
{
	InitializeDObjectSystem();
	FAssetPath BasePath;
	FAssetPath ExternalPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/RevalidateBase", BasePath));
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/RevalidateExternal", ExternalPath));
	DMaterial* Base = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(BasePath, Base));
	ASSERT_TRUE(Asset::SavePackage(Base->GetPackage()));

	Asset::FAssetDeletionTransaction Transaction;
	std::vector<Asset::FAssetDeletionBatchBlocker> Blockers;
	ASSERT_TRUE(Asset::PrepareAssetDeletionTransaction(
		std::span{&BasePath, 1}, {}, Transaction, Blockers));
	ASSERT_TRUE(Blockers.empty());

	DMaterialInstance* External = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(ExternalPath, External));
	ASSERT_TRUE(External->SetParent(Base));
	ASSERT_TRUE(Asset::SavePackage(External->GetPackage()));
	const Asset::FAssetResult Commit = Transaction.Commit({
		.Stage = [] { return Asset::FAssetResult{}; },
		.Restore = [] { return Asset::FAssetResult{}; },
	});
	EXPECT_EQ(Commit.Error, Asset::EAssetError::InUse);

	ASSERT_TRUE(Asset::UnloadPackage(ExternalPath));
	ASSERT_TRUE(Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(ExternalPath));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(BasePath));
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
	FContentBrowserOperations Operations(
		Model,
		[](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem FolderItem{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ReparseRoot",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = Operations.BuildDeletionPlan(
		std::span{&FolderItem, 1},
		std::unordered_set<std::string>{FolderItem.StableId()});
	EXPECT_TRUE(std::ranges::any_of(
		Plan->Blockers,
		[](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind == EContentDeletionBlocker::ReparsePoint;
		}));
}

TEST_F(FContentBrowserModelTests, EmptyFolderDeletionRoundTripsThroughHistory)
{
	const std::filesystem::path Folder = Root / "Content/EmptyFolder";
	std::filesystem::create_directories(Folder);
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "EmptyFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Plan->Summary.FolderCount, 1);
	EXPECT_EQ(Plan->Summary.AssetCount, 0);
	EXPECT_EQ(Plan->Summary.FileCount, 0);

	Durin::Editor::FTransactionManager Transactions;
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FContentDeletionTransaction>(Plan)));
	EXPECT_FALSE(std::filesystem::exists(Folder));
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(std::filesystem::is_directory(Folder));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_FALSE(std::filesystem::exists(Folder));
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(std::filesystem::is_directory(Folder));
}

TEST_F(FContentBrowserModelTests, MixedFolderAndExternalCompanionRoundTripAsOneTransaction)
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
		Asset::FAssetDeleteContributorHandle Handle = 0;
		~FContributorReset() { Asset::UnregisterAssetDeleteContributor(Handle); }
	};
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/Mixed/Nested/Material", AssetPath));
	const Asset::FAssetDeleteContributorHandle Contributor =
	Asset::RegisterAssetDeleteContributor(
		DMaterial::StaticClass(),
		[AssetPath, Companion](const Asset::FAssetData& Data,
			const Asset::FAssetPackageInspection&,
			Asset::FAssetDeleteContribution& Contribution) -> Asset::FAssetResult {
			if (Data.PackagePath == AssetPath)
				Contribution.Files.push_back(Companion);
			return {};
		});
	ASSERT_NE(Contributor, 0);
	FContributorReset Reset{Contributor};
	DMaterial* Material = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(AssetPath, Material));
	ASSERT_TRUE(Asset::SavePackage(Material->GetPackage()));

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "Mixed",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	ASSERT_TRUE(Plan->CanExecute());
	EXPECT_EQ(Plan->Summary.AssetCount, 1);
	EXPECT_EQ(Plan->Summary.FileCount, 1);
	EXPECT_EQ(Plan->Summary.CompanionCount, 1);
	EXPECT_EQ(Plan->Summary.FolderCount, 2);
	ASSERT_EQ(Plan->MaximalRoots.size(), 2);

	Durin::Editor::FTransactionManager Transactions;
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FContentDeletionTransaction>(Plan)));
	EXPECT_FALSE(std::filesystem::exists(Folder));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	EXPECT_EQ(Asset::FindAssetExact(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(std::filesystem::is_regular_file(OrdinaryFile));
	EXPECT_TRUE(std::filesystem::is_regular_file(Companion));
	EXPECT_NE(Asset::FindAssetExact(AssetPath), nullptr);
	EXPECT_EQ(Asset::FindResidentPackage(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_FALSE(std::filesystem::exists(Folder));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Asset::DeleteAssetForTesting(AssetPath));
}

TEST_F(FContentBrowserModelTests, DeletionTransactionPreservesRegistryWithoutResidency)
{
	InitializeDObjectSystem();
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/TransactionalMaterial", AssetPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(AssetPath, Material));
	ASSERT_TRUE(Asset::SavePackage(Material->GetPackage()));
	const std::filesystem::path PackagePath =
		Asset::FindAssetExact(AssetPath)->PhysicalPath;

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "TransactionalMaterial",
		.VirtualPath = AssetPath.ToString(),
		.PhysicalPath = PackagePath.generic_string(),
		.AssetClassName = DMaterial::StaticClass()->GetQualifiedName().ToString()};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	ASSERT_TRUE(Plan->CanExecute());

	Durin::Editor::FTransactionManager Transactions;
	const uint64 InitialContentRevision =
		Transactions.GetMountedContentMutationRevision();
	auto Transaction = std::make_unique<FContentDeletionTransaction>(Plan);
	FContentDeletionTransaction* TransactionView = Transaction.get();
	ASSERT_TRUE(Transactions.Execute(std::move(Transaction)));
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 1);
	EXPECT_FALSE(std::filesystem::exists(PackagePath));
	EXPECT_EQ(Asset::FindAssetExact(AssetPath), nullptr);
	EXPECT_EQ(Asset::FindResidentPackage(AssetPath), nullptr);
	EXPECT_EQ(TransactionView->GetState(), EContentDeletionTransactionState::Applied);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 2);
	EXPECT_TRUE(std::filesystem::is_regular_file(PackagePath));
	EXPECT_NE(Asset::FindAssetExact(AssetPath), nullptr);
	EXPECT_EQ(Asset::FindResidentPackage(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 3);
	EXPECT_FALSE(std::filesystem::exists(PackagePath));
	EXPECT_EQ(Asset::FindAssetExact(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 4);
	const std::filesystem::path StagingRoot = TransactionView->GetStagingRoot();
	Transactions.Clear();
	EXPECT_FALSE(std::filesystem::exists(StagingRoot));
	ASSERT_TRUE(Asset::DeleteAssetForTesting(AssetPath));
}

TEST_F(FContentBrowserModelTests, RedirectorDeletionRequiresClosureAndUndoRestoresExactAlias)
{
	InitializeDObjectSystem();
	FAssetPath OldPath;
	FAssetPath FinalPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/DeleteRedirectOld", OldPath));
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/DeleteRedirectFinal", FinalPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(OldPath, Material));
	ASSERT_TRUE(Asset::SavePackage(Material->GetPackage()));
	const Asset::FAssetRelocationMapping Mapping{OldPath, FinalPath};
	Asset::FAssetMutationSummary Summary;
	Asset::FAssetMutationTransaction Relocation;
	ASSERT_TRUE(Asset::PrepareAssetRelocationTransaction(
		std::span{&Mapping, 1}, Summary, Relocation));
	ASSERT_TRUE(Relocation.Commit());

	Asset::FAssetCatalogEntry AliasData =
		Asset::FindAssetExact(OldPath);
	Asset::FAssetCatalogEntry FinalData =
		Asset::FindAssetExact(FinalPath);
	ASSERT_NE(AliasData, nullptr);
	ASSERT_NE(FinalData, nullptr);
	const FContentBrowserItem AliasItem{
		.Kind = EContentBrowserItemKind::Redirector,
		.Name = "DeleteRedirectOld",
		.VirtualPath = OldPath.ToString(),
		.PhysicalPath = AliasData->PhysicalPath,
		.AssetClassName = AliasData->AssetClassName,
		.RedirectDestination = AliasData->RedirectDestination};
	const FContentBrowserItem FinalItem{
		.Kind = EContentBrowserItemKind::Asset,
		.Name = "DeleteRedirectFinal",
		.VirtualPath = FinalPath.ToString(),
		.PhysicalPath = FinalData->PhysicalPath,
		.AssetClassName = FinalData->AssetClassName};

	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentDeletionPlanPtr AliasOnly = Operations.BuildDeletionPlan(
		std::span{&AliasItem, 1},
		std::unordered_set<std::string>{AliasItem.StableId()});
	EXPECT_TRUE(std::ranges::any_of(
		AliasOnly->Blockers, [](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind
				== EContentDeletionBlocker::RedirectorTargetNotSelected;
		}));
	const FContentDeletionPlanPtr TargetOnly = Operations.BuildDeletionPlan(
		std::span{&FinalItem, 1},
		std::unordered_set<std::string>{FinalItem.StableId()});
	EXPECT_TRUE(std::ranges::any_of(
		TargetOnly->Blockers, [](const FContentDeletionBlocker& Blocker) {
			return Blocker.Kind
				== EContentDeletionBlocker::TargetRedirectorsNotSelected;
		}));

	const std::array Items{AliasItem, FinalItem};
	const FContentDeletionPlanPtr Complete = BuildTransactionPlan(
		Operations, Items, Root / "UndoRedirector");
	ASSERT_TRUE(Complete->CanExecute());
	EXPECT_TRUE(std::ranges::any_of(
		Complete->Warnings, [](const FContentDeletionWarning& Warning) {
			return Warning.Details.find("authored old path")
				!= std::string::npos;
		}));

	Durin::Editor::FTransactionManager Transactions;
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FContentDeletionTransaction>(Complete)));
	EXPECT_EQ(Asset::FindAssetExact(OldPath), nullptr);
	EXPECT_EQ(Asset::FindAssetExact(FinalPath), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	AliasData = Asset::FindAssetExact(OldPath);
	FinalData = Asset::FindAssetExact(FinalPath);
	ASSERT_NE(AliasData, nullptr);
	ASSERT_NE(FinalData, nullptr);
	EXPECT_EQ(AliasData->EntryKind,
		Asset::EAssetRegistryEntryKind::Redirector);
	EXPECT_EQ(AliasData->RedirectDestination, FinalPath);
	EXPECT_EQ(FinalData->EntryKind, Asset::EAssetRegistryEntryKind::Asset);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Transactions.Redo());
	Transactions.Clear();
}

TEST_F(FContentBrowserModelTests, DeletionTransactionRejectsDestinationAndModificationConflicts)
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
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "ConflictFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	auto Transaction = std::make_unique<FContentDeletionTransaction>(Plan);
	FContentDeletionTransaction* View = Transaction.get();
	Durin::Editor::FTransactionManager Transactions;
	const uint64 InitialContentRevision =
		Transactions.GetMountedContentMutationRevision();
	ASSERT_TRUE(Transactions.Execute(std::move(Transaction)));
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 1);

	std::filesystem::create_directories(Folder);
	EXPECT_FALSE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 1);
	EXPECT_EQ(View->GetState(), EContentDeletionTransactionState::Applied);
	std::error_code Ec;
	std::filesystem::remove(Folder, Ec);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 2);
	{
		std::ofstream File(SourceFile, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(SourceFile, ConfirmedWriteTime);
	EXPECT_FALSE(Transactions.Redo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialContentRevision + 2);
	EXPECT_EQ(View->GetState(), EContentDeletionTransactionState::Restored);
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
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "rewrite.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	ASSERT_TRUE(Plan->CanExecute());
	{
		std::ofstream File(FilePath, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(FilePath, ConfirmedWriteTime);
	EXPECT_FALSE(Operations.IsDeletionPlanCurrent(*Plan));
	FContentDeletionTransaction Transaction(Plan);
	EXPECT_FALSE(Transaction.Redo());
	EXPECT_TRUE(std::filesystem::exists(FilePath));
}

TEST_F(FContentBrowserModelTests, DeletionUndoRejectsChangedStagedBytes)
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
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::Folder,
		.Name = "StagedRewrite",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	auto Transaction = std::make_unique<FContentDeletionTransaction>(Plan);
	FContentDeletionTransaction* View = Transaction.get();
	Durin::Editor::FTransactionManager Transactions;
	ASSERT_TRUE(Transactions.Execute(std::move(Transaction)));
	const std::filesystem::path StagedFile =
		View->GetStagingRoot() / "entry-0000/source.txt";
	const auto ConfirmedWriteTime = std::filesystem::last_write_time(StagedFile);
	{
		std::ofstream File(StagedFile, std::ios::trunc);
		File << "change";
	}
	std::filesystem::last_write_time(StagedFile, ConfirmedWriteTime);
	EXPECT_FALSE(Transactions.Undo());
	EXPECT_EQ(View->GetState(), EContentDeletionTransactionState::Applied);
	{
		std::ofstream File(StagedFile, std::ios::trunc);
		File << "source";
	}
	std::filesystem::last_write_time(StagedFile, ConfirmedWriteTime);
	EXPECT_TRUE(Transactions.Undo());
}

TEST_F(FContentBrowserModelTests, DeletionTransactionCompensatesAndRetainsFailedRecovery)
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
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const std::array Items{
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File,
			.Name = "first.txt", .PhysicalPath = Paths[0].generic_string(),
			.Extension = ".txt"},
		FContentBrowserItem{.Kind = EContentBrowserItemKind::File,
			.Name = "second.txt", .PhysicalPath = Paths[1].generic_string(),
			.Extension = ".txt"}};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, Items, Root / "Undo");

	FContentDeletionTransactionHooks OrdinaryHooks;
	OrdinaryHooks.Rename = [](const std::filesystem::path& Source,
		const std::filesystem::path& Destination,
		EContentDeletionMovePhase Phase) -> std::error_code {
		if (Phase == EContentDeletionMovePhase::Apply
			&& Source.filename() == "second.txt")
			return std::make_error_code(std::errc::io_error);
		std::error_code Ec;
		std::filesystem::rename(Source, Destination, Ec);
		return Ec;
	};
	{
		auto Transaction = std::make_unique<FContentDeletionTransaction>(
			Plan, OrdinaryHooks);
		EXPECT_FALSE(Transaction->Redo());
		EXPECT_EQ(Transaction->GetState(), EContentDeletionTransactionState::Restored);
		EXPECT_TRUE(std::filesystem::exists(Paths[0]));
		EXPECT_TRUE(std::filesystem::exists(Paths[1]));
	}

	FContentDeletionTransactionHooks RecoveryHooks;
	RecoveryHooks.Rename = [](const std::filesystem::path& Source,
		const std::filesystem::path& Destination,
		EContentDeletionMovePhase Phase) -> std::error_code {
		if ((Phase == EContentDeletionMovePhase::Apply
				&& Source.filename() == "second.txt")
			|| Phase == EContentDeletionMovePhase::CompensateApply)
			return std::make_error_code(std::errc::io_error);
		std::error_code Ec;
		std::filesystem::rename(Source, Destination, Ec);
		return Ec;
	};
	std::filesystem::path RecoveryRoot;
	{
		auto Transaction = std::make_unique<FContentDeletionTransaction>(
			Plan, RecoveryHooks);
		EXPECT_FALSE(Transaction->Redo());
		EXPECT_EQ(Transaction->GetState(), EContentDeletionTransactionState::RecoveryRequired);
		RecoveryRoot = Transaction->GetStagingRoot();
		EXPECT_TRUE(Transaction->GetDetails(Durin::Editor::ETransactionOperation::Execute).find(
			RecoveryRoot.generic_string()) != std::string::npos);
	}
	EXPECT_TRUE(std::filesystem::is_directory(RecoveryRoot));
	std::error_code Ec;
	Durin::Testing::RemoveTestWorkDirectory(RecoveryRoot, Ec);
}

TEST_F(FContentBrowserModelTests, EvictionCleansOwnedDeletionStaging)
{
	const std::filesystem::path FilePath = Root / "Content/evicted.txt";
	{
		std::ofstream File(FilePath);
		File << "evict me";
	}
	FContentBrowserModel Model;
	Model.RefreshMountSnapshot();
	FContentBrowserOperations Operations(
		Model, [](std::span<const FEditorAssetMove>) -> Asset::FAssetResult {
			return {};
		});
	const FContentBrowserItem Item{
		.Kind = EContentBrowserItemKind::File,
		.Name = "evicted.txt",
		.PhysicalPath = FilePath.generic_string(),
		.Extension = ".txt"};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	Durin::Editor::FTransactionManager Transactions;
	auto Transaction = std::make_unique<FContentDeletionTransaction>(Plan);
	FContentDeletionTransaction* View = Transaction.get();
	ASSERT_TRUE(Transactions.Execute(std::move(Transaction)));
	const std::filesystem::path StagingRoot = View->GetStagingRoot();
	for (size_t Index = 0; Index < 256; ++Index)
		ASSERT_TRUE(Transactions.Execute(
			std::make_unique<FNoOpEditorTransaction>()));
	EXPECT_FALSE(std::filesystem::exists(StagingRoot));
}
