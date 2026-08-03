#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserOperations.h"

#include "EngineTestSupport.h"
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
					.bAssetPackages = true}};
			Registry =
				std::make_unique<PathUtilities::FScopedMountRegistryFixture>(
					Definitions);
			ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
		}

		std::filesystem::path Root;
		std::unique_ptr<PathUtilities::FScopedMountRegistryFixture> Registry;
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

TEST_F(FContentBrowserModelTests, SearchesRecursivelyButBrowsesImmediateChildren)
{
	const std::string RootPath =
		std::filesystem::absolute(Root / "Content").lexically_normal().generic_string();
	FContentBrowserModel Model;
	Model.SetShowSourceFiles(true);
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

TEST_F(FContentBrowserModelTests, HidesRawAuthoringFilesFromPackageView)
{
	{
		std::ofstream RawSource(Root / "Content/Raw.png");
		RawSource << "raw";
	}
	FContentBrowserModel Model;
	ASSERT_TRUE(Model.NavigateToPhysical((Root / "Content").generic_string()));
	EXPECT_TRUE(std::ranges::none_of(Model.GetItems(), [](const FContentBrowserItem& Item) {
		return Item.Kind == EContentBrowserItemKind::SourceFile || Item.Name == "Raw.png";
	}));
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
		.Kind = EContentBrowserItemKind::SourceFile,
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
