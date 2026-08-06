#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserOperations.h"

#include "Assets/AssetRelocationTransaction.h"
#include "Editor/EditorTransaction.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

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

	class FNoOpEditorTransaction final : public IEditorTransaction
	{
	public:
		auto GetDescription() const -> std::string_view override { return "No-op"; }
		auto Undo() -> bool override { return true; }
		auto Redo() -> bool override { return true; }
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
	Asset::FAssetRelocationBatchToken Token;
	ASSERT_TRUE(Asset::AnalyzeAssetRelocationBatch(
		std::span{&Mapping, 1}, Token));
	ASSERT_TRUE(Asset::ApplyAssetRelocationBatch(Token));
	FEditorTransactionManager Transactions;
	ASSERT_TRUE(Transactions.CommitApplied(
		std::make_unique<FAssetRelocationTransaction>(std::move(Token))));
	EXPECT_EQ(Transactions.GetUndoDescription(), "Move Asset");
	EXPECT_EQ(Asset::GetAssetRegistry().ResolveAssetPath(SourcePath).FinalPath,
		DestinationPath);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Asset::GetAssetRegistry().FindAssetExact(SourcePath)->EntryKind,
		Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Asset::GetAssetRegistry().FindAssetExact(DestinationPath), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Asset::GetAssetRegistry().ResolveAssetPath(SourcePath).FinalPath,
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

TEST_F(FContentBrowserModelTests, OperationsPropagateMoveFailureAndDeleteBlockers)
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
	const Asset::FAssetResult DeleteResult =
		Operations.Delete(Items, Selection);
	EXPECT_FALSE(DeleteResult);
	EXPECT_EQ(
		DeleteResult.Message,
		"Folders must be empty before they can be deleted. Delete or move their assets first.");
	EXPECT_TRUE(std::filesystem::exists(Folder));

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

	Asset::FAssetDeletionBatchToken Token;
	std::vector<Asset::FAssetDeletionBatchBlocker> Blockers;
	const std::array PartialPaths{BasePath, InternalPath};
	ASSERT_TRUE(Asset::AnalyzeAssetDeletionBatch(
		PartialPaths, {}, Token, Blockers));
	EXPECT_TRUE(std::ranges::any_of(
		Blockers,
		[&](const Asset::FAssetDeletionBatchBlocker& Blocker) {
			return Blocker.AssetPath == BasePath
				&& Blocker.RelatedAssetPath == ExternalPath;
		}));

	const std::array CompletePaths{BasePath, InternalPath, ExternalPath};
	ASSERT_TRUE(Asset::AnalyzeAssetDeletionBatch(
		CompletePaths, {}, Token, Blockers));
	EXPECT_TRUE(Blockers.empty());
	EXPECT_EQ(Token.GetEntries().size(), 3);

	ASSERT_TRUE(Asset::UnloadPackage(ExternalPath));
	ASSERT_TRUE(Asset::UnloadPackage(InternalPath));
	ASSERT_TRUE(Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Asset::DeleteAsset(ExternalPath));
	ASSERT_TRUE(Asset::DeleteAsset(InternalPath));
	ASSERT_TRUE(Asset::DeleteAsset(BasePath));
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
		~FContributorReset()
		{
			Asset::RegisterAssetDeleteContributor(
				DMaterial::StaticClass(),
				[](const Asset::FAssetData&,
					const Asset::FAssetPackageInspection&,
					Asset::FAssetDeleteContribution&) -> Asset::FAssetResult {
					return {};
				});
		}
	} Reset;
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

	Asset::FAssetDeletionBatchToken Token;
	std::vector<Asset::FAssetDeletionBatchBlocker> Blockers;
	const std::array Paths{FirstPath, SecondPath};
	ASSERT_TRUE(Asset::AnalyzeAssetDeletionBatch(Paths, {}, Token, Blockers));
	EXPECT_TRUE(std::ranges::any_of(
		Blockers,
		[](const Asset::FAssetDeletionBatchBlocker& Blocker) {
			return Blocker.Kind
				== Asset::EAssetDeletionBatchBlocker::CompanionOwnershipConflict;
		}));

	ASSERT_TRUE(Asset::UnloadPackage(SecondPath));
	ASSERT_TRUE(Asset::UnloadPackage(FirstPath));
	ASSERT_TRUE(Asset::DeleteAsset(SecondPath));
	ASSERT_TRUE(Asset::DeleteAsset(FirstPath));
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

	Asset::FAssetDeletionBatchToken Token;
	std::vector<Asset::FAssetDeletionBatchBlocker> Blockers;
	ASSERT_TRUE(Asset::AnalyzeAssetDeletionBatch(
		std::span{&BasePath, 1}, {}, Token, Blockers));
	ASSERT_TRUE(Blockers.empty());

	DMaterialInstance* External = nullptr;
	ASSERT_TRUE(Asset::CreateAsset(ExternalPath, External));
	ASSERT_TRUE(External->SetParent(Base));
	ASSERT_TRUE(Asset::SavePackage(External->GetPackage()));
	ASSERT_TRUE(Asset::RevalidateAssetDeletionBatch(Token, Blockers));
	EXPECT_TRUE(std::ranges::any_of(
		Blockers,
		[&](const Asset::FAssetDeletionBatchBlocker& Blocker) {
			return Blocker.AssetPath == BasePath
				&& Blocker.RelatedAssetPath == ExternalPath;
		}));

	ASSERT_TRUE(Asset::UnloadPackage(ExternalPath));
	ASSERT_TRUE(Asset::UnloadPackage(BasePath));
	ASSERT_TRUE(Asset::DeleteAsset(ExternalPath));
	ASSERT_TRUE(Asset::DeleteAsset(BasePath));
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

	FEditorTransactionManager Transactions;
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
		~FContributorReset()
		{
			Asset::RegisterAssetDeleteContributor(
				DMaterial::StaticClass(),
				[](const Asset::FAssetData&,
					const Asset::FAssetPackageInspection&,
					Asset::FAssetDeleteContribution&) -> Asset::FAssetResult {
					return {};
				});
		}
	} Reset;
	FAssetPath AssetPath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/ContentBrowserTests/Mixed/Nested/Material", AssetPath));
	Asset::RegisterAssetDeleteContributor(
		DMaterial::StaticClass(),
		[AssetPath, Companion](const Asset::FAssetData& Data,
			const Asset::FAssetPackageInspection&,
			Asset::FAssetDeleteContribution& Contribution) -> Asset::FAssetResult {
			if (Data.PackagePath == AssetPath)
				Contribution.Files.push_back(Companion);
			return {};
		});
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

	FEditorTransactionManager Transactions;
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FContentDeletionTransaction>(Plan)));
	EXPECT_FALSE(std::filesystem::exists(Folder));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	EXPECT_EQ(Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(std::filesystem::is_regular_file(OrdinaryFile));
	EXPECT_TRUE(std::filesystem::is_regular_file(Companion));
	EXPECT_NE(Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
	EXPECT_EQ(Asset::FindLoadedPackage(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_FALSE(std::filesystem::exists(Folder));
	EXPECT_FALSE(std::filesystem::exists(Companion));
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Asset::DeleteAsset(AssetPath));
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
		Asset::GetAssetRegistry().FindAsset(AssetPath)->PhysicalPath;

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

	FEditorTransactionManager Transactions;
	const uint64 InitialContentRevision =
		Transactions.GetContentMutationRevision();
	auto Transaction = std::make_unique<FContentDeletionTransaction>(Plan);
	FContentDeletionTransaction* TransactionView = Transaction.get();
	ASSERT_TRUE(Transactions.Execute(std::move(Transaction)));
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 1);
	EXPECT_FALSE(std::filesystem::exists(PackagePath));
	EXPECT_EQ(Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
	EXPECT_EQ(Asset::FindLoadedPackage(AssetPath), nullptr);
	EXPECT_EQ(TransactionView->GetState(), EContentDeletionTransactionState::Applied);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 2);
	EXPECT_TRUE(std::filesystem::is_regular_file(PackagePath));
	EXPECT_NE(Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
	EXPECT_EQ(Asset::FindLoadedPackage(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 3);
	EXPECT_FALSE(std::filesystem::exists(PackagePath));
	EXPECT_EQ(Asset::GetAssetRegistry().FindAsset(AssetPath), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 4);
	const std::filesystem::path StagingRoot = TransactionView->GetStagingRoot();
	Transactions.Clear();
	EXPECT_FALSE(std::filesystem::exists(StagingRoot));
	ASSERT_TRUE(Asset::DeleteAsset(AssetPath));
}

TEST_F(FContentBrowserModelTests, DeletionTransactionRejectsDestinationAndModificationConflicts)
{
	const std::filesystem::path Folder = Root / "Content/ConflictFolder";
	std::filesystem::create_directories(Folder);
	{
		std::ofstream File(Folder / "source.txt");
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
		.Name = "ConflictFolder",
		.PhysicalPath = Folder.generic_string()};
	const FContentDeletionPlanPtr Plan = BuildTransactionPlan(
		Operations, std::span{&Item, 1}, Root / "Undo");
	auto Transaction = std::make_unique<FContentDeletionTransaction>(Plan);
	FContentDeletionTransaction* View = Transaction.get();
	FEditorTransactionManager Transactions;
	const uint64 InitialContentRevision =
		Transactions.GetContentMutationRevision();
	ASSERT_TRUE(Transactions.Execute(std::move(Transaction)));
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 1);

	std::filesystem::create_directories(Folder);
	EXPECT_FALSE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 1);
	EXPECT_EQ(View->GetState(), EContentDeletionTransactionState::Applied);
	std::error_code Ec;
	std::filesystem::remove(Folder, Ec);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 2);
	{
		std::ofstream File(Folder / "source.txt", std::ios::app);
		File << "changed";
	}
	EXPECT_FALSE(Transactions.Redo());
	EXPECT_EQ(
		Transactions.GetContentMutationRevision(), InitialContentRevision + 2);
	EXPECT_EQ(View->GetState(), EContentDeletionTransactionState::Restored);
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
		EXPECT_TRUE(Transaction->GetDetails(EEditorTransactionOperation::Execute).find(
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
	FEditorTransactionManager Transactions;
	auto Transaction = std::make_unique<FContentDeletionTransaction>(Plan);
	FContentDeletionTransaction* View = Transaction.get();
	ASSERT_TRUE(Transactions.Execute(std::move(Transaction)));
	const std::filesystem::path StagingRoot = View->GetStagingRoot();
	for (size_t Index = 0; Index < 256; ++Index)
		ASSERT_TRUE(Transactions.Execute(
			std::make_unique<FNoOpEditorTransaction>()));
	EXPECT_FALSE(std::filesystem::exists(StagingRoot));
}
