#include "Panels/ContentBrowserDataSource.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Misc/MountPaths.h"
#include "Profiling/Profiling.h"

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		using ContentBrowserFilesystem::NormalizePath;
		auto CaptureContentBrowserAssets() -> std::shared_ptr<const FContentBrowserAssetSnapshot>
		{
			const auto Query = CaptureAssetQuerySnapshot();
			auto Result = std::make_shared<FContentBrowserAssetSnapshot>();
			Result->Revision = Query.Revision;
			std::unordered_map<FPackagePath, size_t> AssetCounts;
			for (const auto& [Path, Asset] : Query.Assets) ++AssetCounts[Path.GetPackagePath()];
			for (const auto& [Path, Asset] : Query.Assets)
			{
				const auto PackagePath = Path.GetPackagePath();
				const auto* Disk = Query.FindPackageExact(PackagePath);
				const auto Resolved = FMountPaths::ResolveAssetPath(PackagePath.GetView());
				if (!Disk && !Resolved) continue;
				FContentBrowserItem Item{
					.Kind = Asset.IsRedirector() ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset,
					.Name = AssetCounts[PackagePath] == 1 ? std::string(PackagePath.GetPackageName()) : std::string(Path.GetAssetName()),
					.VirtualPath = Path.ToString(), .PackagePath = PackagePath,
					.PhysicalPath = NormalizePath(Disk ? Disk->PhysicalPath : Resolved->PhysicalPath.generic_string() + ".dasset"),
					.AssetClassName = Asset.AssetClassName, .Extension = ".dasset", .RedirectDestination = Asset.RedirectDestination};
				if (Disk)
				{
					Item.PackageFileSize = Disk->FileSize;
					Item.BulkFileSize = Disk->BulkSegmentExtent;
					Item.FileSize = Item.PackageFileSize + Item.BulkFileSize;
					Item.LastWriteTime = Disk->LastWriteTime;
				}
				Result->Items.push_back(std::move(Item));
			}
			return Result;
		}

	}
	FContentBrowserDataSource::FContentBrowserDataSource(const FContentBrowserHostContext& Host)
		: MutationAllowed(Host.CanMutateContent)
		, ItemUpdates(Host.NotifyScopedContentMutation)
	{
		if (!ItemUpdates && Host.NotifyMountedContentMutation)
			ItemUpdates = [Notify = Host.NotifyMountedContentMutation](FContentChangeBatch) { Notify(); };
	}

	auto FContentBrowserDataSource::PrepareDiscoveryCatalog(std::shared_ptr<const FContentBrowserAssetSnapshot> Previous) const
		-> std::shared_ptr<const FContentBrowserAssetSnapshot>
	{
		if (!bAccepting) return nullptr;
		if (Previous && Previous->Revision == GetAssetQueryRevision()) return Previous;
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureCatalog");
		return CaptureContentBrowserAssets();
	}

	auto FContentBrowserDataSource::PrepareItemsCapture(std::string Directory, bool bRecursive, std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog,
		FEntryStatusQuery Query, std::shared_ptr<Tasks::FCancellationToken> Cancellation) const -> FItemsCapture
	{
		if (!bAccepting || !Catalog) return {};
		if (!Cancellation) Cancellation = std::make_shared<Tasks::FCancellationToken>();
		return [Discovery = ItemDiscovery, Directory = std::move(Directory), bRecursive,
			Catalog = std::move(Catalog), Query = std::move(Query), Cancellation = std::move(Cancellation)]() {
			Discovery->SetEntryStatusQueryForTesting(Query);
			return std::make_unique<FContentBrowserItemsSnapshot>(
				Discovery->CaptureItems(Directory, bRecursive, Catalog, *Cancellation));
		};
	}

	auto FContentBrowserDataSource::PrepareDirectoryCapture(std::string Directory,
		std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog, FEntryStatusQuery Query,
		std::shared_ptr<Tasks::FCancellationToken> Cancellation) const -> FDirectoryCapture
	{
		if (!bAccepting || !Catalog) return {};
		if (!Cancellation) Cancellation = std::make_shared<Tasks::FCancellationToken>();
		return [Directory = std::move(Directory), Catalog = std::move(Catalog),
			Query = std::move(Query), Cancellation = std::move(Cancellation)]() {
			FContentBrowserItemDiscovery Discovery;
			Discovery.SetEntryStatusQueryForTesting(Query);
			return std::make_unique<FContentBrowserDirectorySnapshot>(
				Discovery.CaptureDirectory(Directory, Catalog, *Cancellation));
		};
	}

}
