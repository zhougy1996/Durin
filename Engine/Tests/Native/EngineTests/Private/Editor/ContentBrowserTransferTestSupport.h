#include "Editor/ContentBrowserOperationFixtures.h"
#pragma once
#include "Editor/ContentBrowserQueryTestSupport.h"
#include "Misc/ContentMountPaths.h"
#include "AssetRegistry/Catalog.h"

namespace Durin::ContentBrowser::Private
{
	// Converts native storage fixtures to real provider contributions. All transfer
	// planning, collision checks and I/O use production Item/provider APIs.
	class FProviderTransferFixture
	{
	public:
		explicit FProviderTransferFixture(FContentBrowserOperationContext InHost = {}) : Folder(InHost), Host(std::move(InHost)) {}
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Host.ReportDiagnostic = Sink; Folder.SetDiagnosticSink(std::move(Sink)); }
		auto BulkCopyItems(std::span<const FStorageItemFixture> Items, std::string_view Directory) -> bool { return Execute(Items, Directory, false); }
		auto BulkMoveItems(std::span<const FStorageItemFixture> Items, std::string_view Directory) -> bool { return Execute(Items, Directory, true); }
		auto CanMoveItem(const FStorageItemFixture& Item, std::string_view Directory, std::string* Error = nullptr) -> bool
		{ return Bind(Item, Host).CanMove(ContentStorage::FContentMountPaths::PhysicalToVirtualDirectory(Directory), Error); }
		auto CanRenameItem(const FStorageItemFixture& Item, std::string_view Name, std::string* Error = nullptr) -> bool { return Bind(Item, Host).CanRename(Name, Error); }
		auto RenameItem(const FStorageItemFixture& Item, std::string_view Name, FStorageItemFixture& Out) -> bool
		{
			FContentChangeBatch Changes;
			auto Policy = Host;
			Policy.NotifyContentMutation = [&](FContentChangeBatch Batch) { Changes.bFullRefresh |= Batch.bFullRefresh; Changes.Changes.insert(Changes.Changes.end(), Batch.Changes.begin(), Batch.Changes.end()); };
			auto Bound = Bind(Item, Policy);
			ContentBrowser::FContentBrowserItem NewItem;
			const bool Success = Bound.Rename(Name, NewItem);
			Out = {};
			if (!NewItem.InternalItems.empty())
			{
				Out = Item;
				NewItem.GetItemPhysicalPath(Out.PhysicalPath);
				Out.VirtualPath = NewItem.InternalItems.front().GetVirtualPath();
				Out.Name = std::filesystem::path(Out.PhysicalPath).stem().string();
				Out.Extension = std::filesystem::path(Out.PhysicalPath).extension().string();
				if (Out.Kind == EContentBrowserItemKind::Folder) Out.Name = std::filesystem::path(Out.PhysicalPath).filename().string();
				if (Out.Kind == EContentBrowserItemKind::Asset)
				{
					FTopLevelAssetPath Path;
					if (FTopLevelAssetPath::TryCreate(Out.VirtualPath, Path)) Out.PackagePath = Path.GetPackagePath();
				}
			}
			if (Changes.bFullRefresh || !Changes.Changes.empty())
			{
				if (Host.NotifyContentMutation) Host.NotifyContentMutation(std::move(Changes));
			}
			return Success;
		}
		auto CreateFolder(std::string_view Directory, std::string_view Name, FStorageItemFixture& Out) -> bool { return Folder.CreateFolder(Directory, Name, Out); }
	private:
		auto Bind(FStorageItemFixture Item, const FContentBrowserOperationContext& Policy) -> ContentBrowser::FContentBrowserItem
		{
			for (const auto& [Path, Package] : CaptureAssetCatalogSnapshot().Assets)
				if ((!Item.PhysicalPath.empty() && std::filesystem::path(Item.PhysicalPath).lexically_normal() == std::filesystem::path(Package.PhysicalPath).lexically_normal()) || (Item.Kind == EContentBrowserItemKind::Asset && Item.PackagePath == Path))
				{
					Item.Kind = EContentBrowserItemKind::Asset; Item.PackagePath = Path; Item.PhysicalPath = Package.PhysicalPath;
					if (Item.VirtualPath.empty() && !Package.TopLevelAssets.empty()) Item.VirtualPath = Package.TopLevelAssets.front().AssetPath.ToString();
					break;
				}
			return Query.BindContributionsForTesting(Item, Policy);
		}
		auto Execute(std::span<const FStorageItemFixture> Items, std::string_view Directory, bool Move) -> bool
		{
			FContentChangeBatch Changes;
			auto Policy = Host;
			Policy.NotifyContentMutation = [&](FContentChangeBatch Batch) {
				Changes.bFullRefresh |= Batch.bFullRefresh;
				Changes.Changes.insert(Changes.Changes.end(), std::make_move_iterator(Batch.Changes.begin()), std::make_move_iterator(Batch.Changes.end()));
			};
			ContentBrowser::FContentBrowserItem Selection;
			for (const auto& Item : Items)
			{
				auto Bound = Bind(Item, Policy);
				if (Bound.InternalItems.empty()) return false;
				Selection.InternalItems.insert(Selection.InternalItems.end(), Bound.InternalItems.begin(), Bound.InternalItems.end());
			}
			const auto Destination = ContentStorage::FContentMountPaths::PhysicalToVirtualDirectory(Directory);
			const bool Success = Move ? Query.GetServiceForTesting()->BulkMoveItems(Selection.GetInternalItems(), Destination) : Query.GetServiceForTesting()->BulkCopyItems(Selection.GetInternalItems(), Destination);
			if (Changes.bFullRefresh || !Changes.Changes.empty())
			{
				if (Host.NotifyContentMutation) Host.NotifyContentMutation(std::move(Changes));
			}
			return Success;
		}
		FProviderQueryFixture Query;
		FProviderFolderFixture Folder;
		FContentBrowserOperationContext Host;
	};
}
