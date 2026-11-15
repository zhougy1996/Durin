#include "AssetTools/ContentMutation.h"
#include "ContentBrowserAssetDataSource.h"
#include "Data/ContentBrowserNativeUpdates.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "AssetRegistry/Catalog.h"
#include "DObject/Package.h"
#include "Misc/MountPaths.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/Paths.h"
namespace Durin
{
	using namespace ContentBrowser::AssetDataSource::Private;
	auto DContentBrowserAssetDataSource::UpdateAssetFolders(const FContentChangeBatch& Changes, bool Publish) -> std::vector<ContentBrowser::FContentBrowserItemDataUpdate>
	{
		using namespace ContentBrowser;
		std::vector<FContentBrowserItemDataUpdate> Updates;
		std::unordered_map<std::string, bool> PreviousFolders;
		std::unordered_map<FPackagePath, size_t> PreviousPackages;
		auto ChangePath = [&](const std::string& Path, bool Added) {
			if (Path.empty()) return;
			FTopLevelAssetPath AssetPath;
			if (!FTopLevelAssetPath::TryCreate(Path, AssetPath)) return;
			if (Added)
			{
				const auto Resolved = FMountPaths::ResolveAssetPath(AssetPath.GetPackagePath().GetView());
				const auto Asset = FindAssetExact(AssetPath);
				if (!Resolved || !Asset) return;
				if (Asset.Package)
				{
					std::filesystem::path Relative;
					if (!FPaths::TryMakeLexicalRelativePath(Asset.Package->PhysicalPath,
						Resolved->PhysicalPath.generic_string() + ".dasset", Relative) || !Relative.empty()) return;
				}
			}
			if (!Added && !PackageAssets.contains(AssetPath.GetPackagePath())) return;
			auto& Members = PackageAssets[AssetPath.GetPackagePath()];
			PreviousPackages.try_emplace(AssetPath.GetPackagePath(), Members.size());
			if (Added ? !Members.insert(AssetPath).second : Members.erase(AssetPath) == 0) return;
			if (Members.empty()) PackageAssets.erase(AssetPath.GetPackagePath());
			for (auto Folder = std::filesystem::path(Path).parent_path(); !Folder.empty() && Folder != Folder.root_path(); Folder = Folder.parent_path())
			{
				const auto Virtual = Folder.generic_string();
				auto& Count = AssetFolderCounts[Virtual];
				PreviousFolders.try_emplace(Virtual, Count != 0);
				if (Added) ++Count;
				else if (Count) --Count;
				if (!Count) AssetFolderCounts.erase(Virtual);
			}
		};
		if (Changes.bFullRefresh)
		{
			PackageAssets.clear(); AssetFolderCounts.clear();
			const auto Snapshot = CaptureAssetQuerySnapshot();
			for (const auto& [Path, Asset] : Snapshot.Assets) ChangePath(Path.ToString(), true);
			return Updates;
		}
		for (const auto& Change : Changes.Changes)
		{
			if (Change.bDirectory) continue;
			if (Change.Kind == EContentChangeKind::Removed || Change.Kind == EContentChangeKind::Renamed) ChangePath(Change.OldAssetPath, false);
			if (Change.Kind != EContentChangeKind::Removed) ChangePath(Change.NewAssetPath, true);
		}
		if (!Publish) return Updates;
		FContentChangeBatch Related;
		for (const auto& [Package, Before] : PreviousPackages)
		{
			const auto It = PackageAssets.find(Package);
			if (It == PackageAssets.end() || (Before <= 1) == (It->second.size() <= 1)) continue;
			for (const auto& Asset : It->second)
			{
				const auto Path = Asset.ToString();
				if (std::ranges::any_of(Changes.Changes, [&](const auto& Change) { return Change.NewAssetPath == Path; })) continue;
				const auto Resolved = FMountPaths::ResolveAssetPath(Package.GetView());
				const auto Physical = Resolved ? Resolved->PhysicalPath.generic_string() + ".dasset" : std::string{};
				Related.Changes.push_back({EContentChangeKind::Modified, Physical, Physical, Path, Path});
			}
		}
		// Sibling display labels depend on package membership, not a view recapture.
		if (!Related.Changes.empty()) QueueNativeCommittedUpdates(*this, Related, true);
		for (const auto& [Virtual, Before] : PreviousFolders)
		{
			const bool Added = AssetFolderCounts.contains(Virtual);
			if (Before == Added) continue;
			if (std::ranges::any_of(ContentStorage::FContentMountPaths::CaptureMounts(), [&](const auto& Mount) {
				return FContentBrowserItemDataKey(EContentBrowserItemFlags::Type_Folder, Mount.VirtualRoot).GetVirtualPath() == Virtual;
			})) continue;
			const auto Resolved = FMountPaths::ResolveAssetPath(Virtual);
			if (!Resolved) continue;
			auto Data = BindItemData(MakeNativeItemDescription({.Kind = EContentBrowserItemKind::Folder,
				.Name = std::filesystem::path(Virtual).filename().generic_string(), .VirtualPath = Virtual, .PhysicalPath = Resolved->PhysicalPath.generic_string()}));
			if (Data.GetOwnerDataSource() != this) continue;
			Updates.push_back(Added ? FContentBrowserItemDataUpdate::MakeItemAddedUpdate(std::move(Data))
				: FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(std::move(Data)));
		}
		return Updates;
	}
	auto DContentBrowserAssetDataSource::Initialize(bool AutoRegister) -> void
	{
		if (IsInitialized()) return;
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		Super::Initialize(false);
		(void)UpdateAssetFolders({.bFullRefresh = true}, false);
		auto Invalidate = [Invalidated = FilterCacheInvalidated] { *Invalidated = true; };
		MountsInvalidated = FMountPaths::OnMountPointsChanged().AddLambda(Invalidate);
		PackagesInvalidated = OnAssetPackagesChanged().AddLambda(Invalidate);
		CatalogInvalidated = OnAssetCatalogChanged().AddLambda([Invalidate](const FContentChangeBatch&) { Invalidate(); });
		DirectoriesChanged = ContentStorage::OnContentDirectoriesChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserAssetDataSource>(this)](const FContentChangeBatch& Batch, const DObject* Origin, ContentStorage::EContentDirectoryChangeReason Reason) {
			const auto* OriginSource = Cast<DContentBrowserDataSource>(Origin);
			if (auto* Source = Weak.Get(); Source && OriginSource && Source != OriginSource && Source->GetOwningEditor()
				&& Source->GetOwningEditor() == OriginSource->GetOwningEditor() && Source->IsActive())
			{
				TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
				QueueNativeCommittedUpdates(*Source, Batch, Reason == ContentStorage::EContentDirectoryChangeReason::Discovery);
			}
		});
		QueryChanged = OnAssetQueryChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserAssetDataSource>(this)](const FContentChangeBatch& Batch) {
			if (auto* Source = Weak.Get())
			{
				TStrongObjectPtr<DContentBrowserDataSource> Lifetime(Source);
				auto Folders = Source->UpdateAssetFolders(Batch, Source->IsActive());
				Source->ClearCachedFilterData();
				if (Source->IsActive())
				{
					QueueNativeCommittedUpdates(*Source, Batch, true);
					// File moves migrate selection before old ancestor contributions retire.
					for (auto& Update : Folders) Source->QueueItemDataUpdate(std::move(Update));
					ContentStorage::PublishContentDirectoryChanges(Batch, Source, ContentStorage::EContentDirectoryChangeReason::Discovery);
				}
			}
		});
		if (AutoRegister) RegisterDataSource();
	}
	auto DContentBrowserAssetDataSource::Shutdown() -> void
	{
		ContentStorage::OnContentDirectoriesChanged().Remove(DirectoriesChanged); DirectoriesChanged = {};
		OnAssetQueryChanged().Remove(QueryChanged); QueryChanged = {};
		FMountPaths::OnMountPointsChanged().Remove(MountsInvalidated); MountsInvalidated = {};
		OnAssetPackagesChanged().Remove(PackagesInvalidated); PackagesInvalidated = {};
		OnAssetCatalogChanged().Remove(CatalogInvalidated); CatalogInvalidated = {};
		DiscoveryCatalog.reset(); PackageAssets.clear(); AssetFolderCounts.clear(); Super::Shutdown();
	}
}
