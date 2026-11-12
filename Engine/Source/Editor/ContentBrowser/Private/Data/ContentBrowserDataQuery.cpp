#include "Data/ContentBrowserDataQuery.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Operations/ContentBrowserPaths.h"
#include "Misc/MountPaths.h"
#include "Profiling/Profiling.h"
#include "CoreGlobals.h"
namespace Durin::ContentBrowser::Private
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
			for (const auto& [Path, Asset] : Query.Assets)
				++AssetCounts[Path.GetPackagePath()];
			for (const auto& [Path, Asset] : Query.Assets)
			{
				const auto PackagePath = Path.GetPackagePath();
				const auto* Disk = Query.FindPackageExact(PackagePath);
				const auto Resolved = FMountPaths::ResolveAssetPath(PackagePath.GetView());
				if (!Disk && !Resolved) continue;
				FContentBrowserItem Item{
					.Kind = Asset.IsRedirector() ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset,
					.Name = AssetCounts[PackagePath] == 1 ? std::string(PackagePath.GetPackageName()) : std::string(Path.GetAssetName()),
					.VirtualPath = Path.ToString(),
					.PackagePath = PackagePath,
					.PhysicalPath = NormalizePath(Disk ? Disk->PhysicalPath : Resolved->PhysicalPath.generic_string() + ".dasset"),
					.AssetClassName = Asset.AssetClassName,
					.Extension = ".dasset",
					.RedirectDestination = Asset.RedirectDestination
				};
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

	} // namespace
	auto FContentBrowserDataQuery::PrepareDiscoveryCatalog(std::shared_ptr<const FContentBrowserAssetSnapshot> Previous) const
		-> std::shared_ptr<const FContentBrowserAssetSnapshot>
	{
		if (!IsAcceptingRequests()) return nullptr;
		if (Previous && Previous->Revision == GetAssetQueryRevision()) return Previous;
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureCatalog");
		return CaptureContentBrowserAssets();
	}

	auto FContentBrowserDataQuery::PrepareItemsCapture(std::string Directory, bool bRecursive, std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog, FEntryStatusQuery Query, std::shared_ptr<Tasks::FCancellationToken> Cancellation) const -> FItemsCapture
	{
		if (!IsAcceptingRequests() || !Catalog) return {};
		if (!Cancellation) Cancellation = std::make_shared<Tasks::FCancellationToken>();
		return [Discovery = ItemDiscovery, Directory = std::move(Directory), bRecursive,
				Catalog = std::move(Catalog), Query = std::move(Query), Cancellation = std::move(Cancellation)]() {
			Discovery->SetEntryStatusQueryForTesting(Query);
			return std::make_unique<FContentBrowserItemsSnapshot>(
				Discovery->CaptureItems(Directory, bRecursive, Catalog, *Cancellation)
			);
		};
	}

	auto FContentBrowserDataQuery::PrepareDirectoryCapture(std::string Directory, std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog, FEntryStatusQuery Query, std::shared_ptr<Tasks::FCancellationToken> Cancellation) const -> FDirectoryCapture
	{
		if (!IsAcceptingRequests() || !Catalog) return {};
		if (!Cancellation) Cancellation = std::make_shared<Tasks::FCancellationToken>();
		return [Directory = std::move(Directory), Catalog = std::move(Catalog),
				Query = std::move(Query), Cancellation = std::move(Cancellation)]() {
			FContentBrowserItemDiscovery Discovery;
			Discovery.SetEntryStatusQueryForTesting(Query);
			return std::make_unique<FContentBrowserDirectorySnapshot>(
				Discovery.CaptureDirectory(Directory, Catalog, *Cancellation)
			);
		};
	}

	auto FContentBrowserDataQuery::IsAcceptingRequests() const -> bool
	{
		if (!bAccepting) return false;
		if (!bServiceBound) return true;
		auto* Service = Host.DataSubsystem.Get();
		return Service && Service->IsActive();
	}
	auto FContentBrowserDataQuery::Tick(float DeltaSeconds) -> void
	{
		auto* Service = Host.DataSubsystem.Get();
		if (!IsAcceptingRequests() || !Service || Service->LastBrowserFrame == GFrameCounter) return;
		Service->LastBrowserFrame = GFrameCounter;
		Service->Tick(DeltaSeconds);
	}

	auto FContentBrowserDataQuery::IsProviderDirectory(std::string_view Physical) const -> bool
	{
		if (!bServiceBound || !IsAcceptingRequests()) return false;
		bool Found = false;
		Host.DataSubsystem.Get()->EnumerateItemsAtPath(FContentBrowserPaths::PhysicalToVirtualDirectory(Physical), [&](FContentBrowserItem&& Item) {
   Found=Item.Kind==EContentBrowserItemKind::Folder; return !Found; }, Host);
		return Found;
	}

	auto FContentBrowserDataQuery::IsAffectedByUpdates(uint64 Since, std::string_view Directory, bool Recursive) const -> bool
	{
		auto* Service = Host.DataSubsystem.Get();
		if (!Service) return bServiceBound;
		const auto Current = Service->GetRevision();
		if (Since == Current) return false;
		if (Since < Service->SourceRevision) return true;
		auto Coverage = Service->PendingUpdates.empty() ? Current : Service->PendingFromRevision;
		if (!Service->PublishedUpdates.empty() && Service->PublishedToRevision >= Coverage) Coverage = Service->PublishedFromRevision;
		if (Since < Coverage) return true; // No retained journal: reconcile after missing a frame.
		auto Normalize = [](std::string_view Path) {
			auto Result = std::filesystem::path(Path).lexically_normal().generic_string();
			while (Result.size() > 1 && Result.back() == '/')
				Result.pop_back();
			return Result;
		};
		const auto Scope = Normalize(Directory);
		auto Affected = [&](const auto& Update) {
			auto Touches = [&](std::string_view Path) {
				if (Path.empty()) return false;
				const auto Item = Normalize(Path);
				if (Item == Scope) return true;
				if (std::filesystem::path(Item).parent_path().generic_string() == Scope) return true;
				if (Recursive && Item.starts_with(Scope + "/")) return true;
				return Update.GetItemData().GetItemKind() == EContentBrowserItemKind::Folder && Scope.starts_with(Item + "/");
			};
			return Touches(Update.GetItemData().GetVirtualPath()) || Touches(Update.GetPreviousVirtualPath());
		};
		return std::ranges::any_of(Service->PublishedUpdates, Affected) || std::ranges::any_of(Service->PendingUpdates, Affected);
	}

	auto FContentBrowserDataQuery::GetRevision() const -> uint64
	{
		auto* Service = Host.DataSubsystem.Get();
		return Service ? Service->GetRevision() : 0;
	}
	auto FContentBrowserDataQuery::ProjectItems(FContentBrowserItemsSnapshot Snapshot, std::string_view Path, bool Recursive) const -> FContentBrowserItemsSnapshot
	{
		if (!bServiceBound) return Snapshot;
		if (!IsAcceptingRequests()) return {};
		auto Native = std::make_shared<FNativeContentBrowserFilter>();
		Native->Snapshot = std::make_shared<const FContentBrowserItemsSnapshot>(std::move(Snapshot));
		FContentBrowserItemsSnapshot Result;
		Result.Diagnostics = Native->Snapshot->Diagnostics;
		Result.SuppressedDiagnosticCount = Native->Snapshot->SuppressedDiagnosticCount;
		Host.DataSubsystem.Get()->EnumerateItemsUnderPath(Path, {.bRecursive = Recursive, .ExtraFilter = Native}, [&](FContentBrowserItem&& Item) {
   Result.Items.push_back(std::move(Item)); return true; }, Host);
		return Result;
	}
	auto FContentBrowserDataQuery::ProjectDirectory(FContentBrowserDirectorySnapshot Snapshot, std::string_view Physical) const -> FContentBrowserDirectorySnapshot
	{
		if (!bServiceBound) return Snapshot;
		FContentBrowserItemsSnapshot Raw;
		for (const auto& Child : Snapshot.Children)
			Raw.Items.push_back({.Kind = EContentBrowserItemKind::Folder, .Name = Child.filename().string(), .PhysicalPath = Child.generic_string()});
		auto Items = ProjectItems(std::move(Raw), FContentBrowserPaths::PhysicalToVirtualDirectory(Physical), false);
		Snapshot.Children.clear();
		for (const auto& Item : Items.Items)
			if (Item.Kind == EContentBrowserItemKind::Folder)
				Snapshot.Children.emplace_back(Item.PhysicalPath.empty() ? FContentBrowserPaths::VirtualToPhysical(Item.VirtualPath) : Item.PhysicalPath);
		return Snapshot;
	}
} // namespace Durin::ContentBrowser::Private
