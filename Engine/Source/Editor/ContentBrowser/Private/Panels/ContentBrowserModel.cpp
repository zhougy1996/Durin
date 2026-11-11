#include "Threading/Task.h"
#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserChanges.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "Asset/Asset.h"
#include "Profiling/Profiling.h"
#include "Misc/MountPaths.h"

namespace Durin::ContentBrowser::Private
{
	namespace
	{
		using ContentBrowserFilesystem::NormalizePath;


	} // namespace

	FContentBrowserModel::FContentBrowserModel(FContentBrowserDataSource& InDataSource, bool bInAsync)
		: DataSource(InDataSource)
		, bAsync(bInAsync)
	{
	}

	FContentBrowserModel::~FContentBrowserModel()
	{
		CancelPendingSnapshots();
		// Module code must remain loaded until both owned worker bodies have exited.
		if (ItemsTask.IsValid()) (void)WaitTask(Tasks::FTask(ItemsTask));
		if (TreeTask.IsValid()) (void)WaitTask(Tasks::FTask(TreeTask));
	}

	auto FContentBrowserModel::InvalidateDirectoryTree() -> void
	{
		bActiveTreePublishable = false;
		TreeCancellation->Cancel();
		DirectoryChildrenCache.clear();
		RequestedDirectoryChildrenSnapshots.clear();
	}

	auto FContentBrowserModel::CancelPendingSnapshots() -> void
	{
		if (ItemsRequest) ItemsRequest->State = EItemsRequestState::Cancelled;
		ItemsCancellation->Cancel();
		ItemsRequest.reset();
		bSnapshotPublicationSuspended = false;
		InvalidateDirectoryTree();
	}

	auto FContentBrowserModel::ReportTaskFailure(std::string Message) -> void
	{
		if (EnumerationDiagnostics.size() < 8)
			EnumerationDiagnostics.push_back({EEnumerationDiagnosticKind::Traversal,
				Session.CurrentPhysicalPath, std::move(Message)});
		else
			++SuppressedEnumerationDiagnosticCount;
	}

	auto FContentBrowserModel::PumpPendingSnapshots() -> bool
	{
		if (!DataSource.IsAcceptingRequests()) CancelPendingSnapshots();
		if (bSnapshotPublicationSuspended) return false;
		// Unrelated catalog publications validate an old capture without restarting it.
		if (IsLoading() && ItemsRequest->CatalogCursor != GetAssetQueryRevision())
		{
			const auto Changes = CaptureAssetQueryChanges(ItemsRequest->CatalogCursor);
			ItemsRequest->CatalogCursor = Changes.ToRevision;
			if (IsAffectedBy(Changes))
			{
				RefreshItemsSnapshot(Changes.bFullRefresh);
				return false;
			}
		}
		bool bPublished = false;
		if (ItemsTask.IsValid() && ItemsTask.IsCompleted())
		{
			if (ActiveItemsRequest && ActiveItemsRequest == ItemsRequest
				&& ActiveItemsRequest->State == EItemsRequestState::Capturing)
			{
				ActiveItemsRequest->State = EItemsRequestState::Completed;
				if (ItemsTask.GetState() == ETaskState::Succeeded)
				{
					auto Snapshot = std::move(ItemsTask.GetResult());
					PublishItems(std::move(*Snapshot));
				}
				else
					ReportTaskFailure("Content scan did not complete: " + ItemsTask.GetDiagnostic());
				bPublished = true;
			}
			ItemsTask = {};
			ActiveItemsRequest.reset();
		}
		if (ItemsRequest && ItemsRequest->State == EItemsRequestState::Queued && !ItemsTask.IsValid())
		{
			ActiveItemsRequest = ItemsRequest;
			ActiveItemsRequest->State = EItemsRequestState::Capturing;
			ItemsCancellation = std::make_shared<Tasks::FCancellationToken>();
			auto Work = DataSource.PrepareItemsCapture(ActiveItemsRequest->Directory,
				ActiveItemsRequest->bRecursive, ActiveItemsRequest->Catalog, EntryStatusQuery, ItemsCancellation);
			if (!Work) { CancelPendingSnapshots(); return false; }
			ItemsTask = Tasks::LaunchTask("ContentBrowser.Items", std::move(Work),
				{.Attribution = RegisterTaskAttribution("ContentBrowser", "Items")});

		}
		if (TreeTask.IsValid() && TreeTask.IsCompleted())
		{
			if (bActiveTreePublishable)
			{
				if (TreeTask.GetState() == ETaskState::Succeeded)
				{
					auto Snapshot = std::move(TreeTask.GetResult());
					PublishDirectory(ActiveTreeDirectory, std::move(*Snapshot));
				}
				else
				{
					ReportTaskFailure("Directory scan did not complete: " + TreeTask.GetDiagnostic());
					DirectoryChildrenCache.emplace(ActiveTreeDirectory,
						std::make_shared<const FContentBrowserDirectorySnapshot>());
				}
			}
			TreeTask = {};
			ActiveTreeDirectory.clear();
		}
		if (bAsync && !TreeTask.IsValid() && !RequestedDirectoryChildrenSnapshots.empty())
		{
			const auto It = RequestedDirectoryChildrenSnapshots.begin();
			ActiveTreeDirectory = *It;
			RequestedDirectoryChildrenSnapshots.erase(It);
			bActiveTreePublishable = true;
			TreeCancellation = std::make_shared<Tasks::FCancellationToken>();
			Catalog = DataSource.PrepareDiscoveryCatalog(Catalog);
			auto Work = DataSource.PrepareDirectoryCapture(ActiveTreeDirectory, Catalog, EntryStatusQuery, TreeCancellation);
			if (!Work) { CancelPendingSnapshots(); return bPublished; }
			TreeTask = Tasks::LaunchTask("ContentBrowser.Directory", std::move(Work),
				{.Attribution = RegisterTaskAttribution("ContentBrowser", "Directory")});

		}
		return bPublished;
	}

	auto FContentBrowserModel::WaitForPendingSnapshotsForTesting() -> void
	{
		do
		{
			(void)PumpPendingSnapshots();
			if (ItemsTask.IsValid()) (void)WaitTask(Tasks::FTask(ItemsTask));
			if (TreeTask.IsValid()) (void)WaitTask(Tasks::FTask(TreeTask));
		} while (ItemsTask.IsValid() || TreeTask.IsValid() || IsLoading()
			|| (bAsync && !RequestedDirectoryChildrenSnapshots.empty()));
	}

	auto FContentBrowserModel::RefreshMountSnapshot() -> void
	{
		const uint64 RegistryRevision = FMountPaths::GetMountRegistryRevision();
		if (MountSnapshotRevision == RegistryRevision) return;
		auto NextMountSnapshot = FContentBrowserPaths::CaptureMounts();
		MountSnapshotRevision = RegistryRevision;

		const bool bUnchanged = std::ranges::equal(
			NextMountSnapshot,
			MountSnapshot,
			[](const FMountSnapshot& A, const FMountSnapshot& B) {
				return A.VirtualRoot == B.VirtualRoot
					&& A.PhysicalRoot == B.PhysicalRoot
					&& A.bContentWritable == B.bContentWritable;
			});
		if (bUnchanged) return;

		MountSnapshot = std::move(NextMountSnapshot);
		CancelPendingSnapshots();
		if (!Session.CurrentPhysicalPath.empty() && !ResolveMountPath(Session.CurrentPhysicalPath))
		{
			Session.CurrentPhysicalPath.clear();
			Session.CurrentVirtualPath.clear();
			PublishedSnapshot = std::make_shared<const FContentBrowserItemsSnapshot>();
			Items = {};
		}
		else if (!Session.CurrentPhysicalPath.empty())
			RefreshItemsSnapshot(false);
	}

	auto FContentBrowserModel::RescanRegistry() -> FAssetReadResult
	{
		const FAssetCatalogRefreshResult Refresh =
			RefreshAssetRegistry(
				EAssetRegistryScanMode::Incremental);
		if (Refresh) return {};
		return Refresh.Errors.empty()
			? FAssetReadResult{
				EAssetReadError::IoError,
				"Asset catalog refresh was incomplete."}
			: FAssetReadResult{
				.Error = EAssetReadError::IoError, .Message = FormatAssetRegistryError(Refresh.Errors.front())};
	}

	auto FContentBrowserModel::PhysicalToVirtualDirectory(
		std::string_view PhysicalPath) const -> std::string
	{
		return FContentBrowserPaths::PhysicalToVirtualDirectory(PhysicalPath);
	}

	auto FContentBrowserModel::ResolveMountPath(std::string_view PhysicalPath) const -> FMountPath
	{
		return FContentBrowserPaths::ResolveMountPath(PhysicalPath);
	}

	auto FContentBrowserModel::VirtualToPhysical(std::string_view VirtualPath) const -> std::string
	{
		return FContentBrowserPaths::VirtualToPhysical(VirtualPath);
	}

	auto FContentBrowserModel::NavigateToPhysical(
		std::string_view PhysicalPath,
		bool bAddHistory) -> bool
	{
		RefreshMountSnapshot();
		const FMountPath Resolved = ResolveMountPath(PhysicalPath);
		if (!Resolved || !IsDirectoryAvailable(Resolved.NormalizedPhysicalPath))
			return false;
		if (Resolved.NormalizedPhysicalPath == Session.CurrentPhysicalPath)
			return true;
		std::string Virtual = Resolved.VirtualPath;
		if (!Virtual.ends_with('/')) Virtual += '/';

		++Session.NavigationRevision;
		Session.CurrentPhysicalPath = Resolved.NormalizedPhysicalPath;
		Session.CurrentVirtualPath = Virtual;
		if (bAddHistory)
		{
			if (Session.HistoryIndex >= 0
				&& static_cast<size_t>(Session.HistoryIndex + 1) < Session.NavigationHistory.size())
				Session.NavigationHistory.resize(static_cast<size_t>(Session.HistoryIndex + 1));
			if (Session.NavigationHistory.empty()
				|| Session.NavigationHistory.back() != Resolved.NormalizedPhysicalPath)
			{
				Session.NavigationHistory.push_back(Resolved.NormalizedPhysicalPath);
				Session.HistoryIndex = static_cast<int32>(Session.NavigationHistory.size() - 1);
			}
		}
		RefreshItemsSnapshot(false);
		return true;
	}

	auto FContentBrowserModel::NavigateHistory(int32 Delta) -> bool
	{
		const int32 Target = Session.HistoryIndex + Delta;
		if (Target < 0 || static_cast<size_t>(Target) >= Session.NavigationHistory.size())
			return false;
		Session.HistoryIndex = Target;
		if (NavigateToPhysical(
				Session.NavigationHistory[static_cast<size_t>(Session.HistoryIndex)], false))
			return true;

		Session.NavigationHistory.erase(Session.NavigationHistory.begin() + Session.HistoryIndex);
		Session.HistoryIndex =
			std::min(Session.HistoryIndex, static_cast<int32>(Session.NavigationHistory.size()) - 1);
		return false;
	}

	auto FContentBrowserModel::RevealAsset(std::string_view AssetPath)
		-> std::string
	{
		FTopLevelAssetPath Path;
		if (!FTopLevelAssetPath::TryCreate(AssetPath, Path)) return {};
		const auto Entry = FindAssetExact(Path);
		if (!Entry) return {};
		if (Entry.Asset->IsRedirector())
			Session.Query.bShowRedirectors = true;
		const auto Resolved = FMountPaths::ResolveAssetPath(Path.GetPackagePath().GetView());
		if (!Entry.Package && !Resolved) return {};
		const std::string Revealed = RevealPhysicalItem(Entry.Package ? Entry.Package->PhysicalPath : Resolved->PhysicalPath.generic_string() + ".dasset");
		const std::string StablePath = Path.ToString();
		if (bAsync) return Revealed.empty() ? std::string{} : StablePath;
		return std::ranges::any_of(
			Items, [&StablePath](const FContentBrowserItem& Item) {
				return Item.StableId() == StablePath;
			}) ? StablePath : std::string{};
	}

	auto FContentBrowserModel::RevealPhysicalItem(
		std::string_view InPhysicalPath) -> std::string
	{
		const std::string PhysicalPath = NormalizePath(InPhysicalPath);
		if (!NavigateToPhysical(
				std::filesystem::path(PhysicalPath)
					.parent_path()
					.generic_string()))
			return {};

		Session.Query.Search.clear();
		Session.Query.TypeFilter = EContentBrowserTypeFilter::All;
		Session.Query.CategoryPath.clear();
		RefreshItemsSnapshot(false);

		if (bAsync && IsLoading())
		{
			std::error_code Error;
			return ((std::filesystem::exists(QueryPathStatus(PhysicalPath, Error)) && !Error)
				|| (Catalog && std::ranges::any_of(Catalog->Items, [&](const auto& Item) {
				return ContentBrowserChanges::SamePath(Item.PhysicalPath, PhysicalPath);
			}))) ? PhysicalPath : std::string{};
		}
		if (std::ranges::none_of(
				Items,
				[&](const FContentBrowserItem& Item) {
					return Item.StableId() == PhysicalPath;
				}))
			return {};
		return PhysicalPath;
	}

	auto FContentBrowserModel::IsAffectedBy(const FContentChangeBatch& Changes) const -> bool
	{
		return Changes.bFullRefresh || std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
			if (ContentBrowserChanges::Affects(Change, Session.CurrentPhysicalPath, !Session.Query.Search.empty())) return true;
			const auto Child = ContentBrowserChanges::IntroducedChild(Change, Session.CurrentPhysicalPath);
			return !Child.empty() && std::ranges::none_of(PublishedSnapshot->Items, [&](const auto& Item) {
				return Item.Kind == EContentBrowserItemKind::Folder && ContentBrowserChanges::SamePath(Item.PhysicalPath, Child);
			});
		});
	}

	auto FContentBrowserModel::ApplyContentChanges(const FContentChangeBatch& Changes) -> bool
	{
		const bool bAffected = IsAffectedBy(Changes);
		if (Changes.bFullRefresh) { RefreshItemsSnapshot(); return true; }
		const auto Invalidates = [&](const std::string& Directory) {
			return std::ranges::any_of(Changes.Changes, [&](const FContentChange& Change) {
				const auto Child = ContentBrowserChanges::IntroducedChild(Change, Directory);
				if (!Child.empty())
				{
					const auto Cached = DirectoryChildrenCache.find(Directory);
					if (Cached == DirectoryChildrenCache.end() || std::ranges::none_of(Cached->second->Children,
						[&](const auto& Path) { return ContentBrowserChanges::SamePath(Path.generic_string(), Child); })) return true;
				}
				if (!Change.bDirectory) return false;
				for (const auto* Path : {&Change.OldPhysicalPath, &Change.NewPhysicalPath})
					if (!Path->empty() && ContentBrowserChanges::SamePath(Directory,
						std::filesystem::path(*Path).parent_path().generic_string())) return true;
				return (Change.Kind == EContentChangeKind::Removed || Change.Kind == EContentChangeKind::Renamed)
					&& ContentBrowserChanges::Within(Directory, Change.OldPhysicalPath);
			});
		};
		std::erase_if(DirectoryChildrenCache, [&](const auto& Entry) { return Invalidates(Entry.first); });
		std::erase_if(RequestedDirectoryChildrenSnapshots, Invalidates);
		if (!ActiveTreeDirectory.empty() && Invalidates(ActiveTreeDirectory))
		{
			bActiveTreePublishable = false;
			TreeCancellation->Cancel();
		}
		bool bMoved = false;
		for (const auto& Change : Changes.Changes)
		{
			if (!Change.bDirectory) continue;
			bMoved |= ContentBrowserChanges::RemapPhysical(Session.CurrentPhysicalPath, Change);
			for (auto& Path : Session.NavigationHistory) ContentBrowserChanges::RemapPhysical(Path, Change);
		}
		if (bMoved)
		{
			++Session.NavigationRevision;
			Session.CurrentVirtualPath = PhysicalToVirtualDirectory(Session.CurrentPhysicalPath);
		}
		if (!bAffected) return false;
		const auto Available = FindNearestAvailableDirectory(Session.CurrentPhysicalPath);
		if (Available.empty() || !ResolveMountPath(Available))
		{
			for (const auto& Mount : MountSnapshot)
				if (NavigateToPhysical(Mount.PhysicalRoot, false)) return true;
			CancelPendingSnapshots();
			++Session.NavigationRevision;
			Session.CurrentPhysicalPath.clear();
			Session.CurrentVirtualPath.clear();
			PublishedSnapshot = std::make_shared<const FContentBrowserItemsSnapshot>();
			RebuildItems();
			return true;
		}
		if (!ContentBrowserChanges::SamePath(Available, Session.CurrentPhysicalPath))
			return NavigateToPhysical(Available, false);
		RefreshItemsSnapshot(false);
		return true;
	}

	auto FContentBrowserModel::RefreshItemsSnapshot(bool bInvalidateDirectoryTree) -> void
	{
		bSnapshotInjectedForTesting = false;
		if (bInvalidateDirectoryTree) InvalidateDirectoryTree();
		if (ItemsRequest) ItemsRequest->State = EItemsRequestState::Cancelled;
		ItemsCancellation->Cancel();
		Catalog = DataSource.PrepareDiscoveryCatalog(Catalog);
		if (!Catalog) { CancelPendingSnapshots(); return; }
		++ItemsRequestCount;
		bSnapshotPublicationSuspended = false;
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		if (bAsync)
		{
			// Do not expose actionable rows from a previous directory or revision.
			PublishedSnapshot = std::make_shared<const FContentBrowserItemsSnapshot>();
			Items = {};
			ItemsRequest = std::make_shared<FItemsRequest>(FItemsRequest{
				Session.CurrentPhysicalPath, !Session.Query.Search.empty(), Catalog, Catalog->Revision});
			(void)PumpPendingSnapshots();
			return;
		}
		auto Work = DataSource.PrepareItemsCapture(Session.CurrentPhysicalPath,
			!Session.Query.Search.empty(), Catalog, EntryStatusQuery);
		if (Work) PublishItems(std::move(*Work()));
	}

	auto FContentBrowserModel::PublishItems(FContentBrowserItemsSnapshot Snapshot) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.PublishItems");
		Snapshot.Version = ++SnapshotVersion;
		for (FContentBrowserItem& Item : Snapshot.Items)
		{
			if (Item.Kind == EContentBrowserItemKind::Folder)
				Item.VirtualPath = PhysicalToVirtualDirectory(Item.PhysicalPath);
		}
		// Tree completions may have published diagnostics while item I/O was pending.
		for (const auto& Diagnostic : Snapshot.Diagnostics)
		{
			if (EnumerationDiagnostics.size() < 8) EnumerationDiagnostics.push_back(Diagnostic);
			else ++SuppressedEnumerationDiagnosticCount;
		}
		SuppressedEnumerationDiagnosticCount += Snapshot.SuppressedDiagnosticCount;
		PublishedSnapshot = std::make_shared<const FContentBrowserItemsSnapshot>(std::move(Snapshot));
		RebuildItems();
	}

	auto FContentBrowserModel::RebuildItems() -> void
	{
		Items = ContentBrowserQuery::Project(PublishedSnapshot, Session.CurrentPhysicalPath, Session.Query);
	}

	auto FContentBrowserModel::SetSearch(std::string_view InSearch) -> void
	{
		const bool bScopeChanged = Session.Query.Search.empty() != InSearch.empty();
		Session.Query.Search = InSearch;
		if (bScopeChanged && !bSnapshotInjectedForTesting)
		{
			RefreshItemsSnapshot(false);
			return;
		}
		RebuildItems();
	}

	auto FContentBrowserModel::SetTypeFilter(EContentBrowserTypeFilter Filter) -> void
	{
		Session.Query.TypeFilter = Filter;
		Session.Query.CategoryPath.clear();
		RebuildItems();
	}
	auto FContentBrowserModel::SetCategoryPath(std::vector<std::string> Path) -> void
	{
		Session.Query.TypeFilter = EContentBrowserTypeFilter::Assets;
		Session.Query.CategoryPath = std::move(Path);
		RebuildItems();
	}

	auto FContentBrowserModel::SetSort(
		EContentBrowserSortColumn Column,
		bool bAscending) -> void
	{
		Session.Query.SortColumn = Column;
		Session.Query.bSortAscending = bAscending;
		RebuildItems();
	}

	auto FContentBrowserModel::SetShowHiddenFiles(bool bShow) -> void
	{
		Session.Query.bShowHiddenFiles = bShow;
		RebuildItems();
	}

	auto FContentBrowserModel::SetShowRedirectors(bool bShow) -> void
	{
		Session.Query.bShowRedirectors = bShow;
		RebuildItems();
	}

	auto FContentBrowserModel::GetDirectorySnapshot(std::string_view PhysicalDirectory) const
		-> std::shared_ptr<const FContentBrowserDirectorySnapshot>
	{
		const auto It = DirectoryChildrenCache.find(NormalizePath(PhysicalDirectory));
		return It != DirectoryChildrenCache.end() ? It->second : nullptr;
	}

	auto FContentBrowserModel::HasDirectoryChildrenSnapshot(
		std::string_view PhysicalDirectory) const -> bool
	{
		return DirectoryChildrenCache.contains(NormalizePath(PhysicalDirectory));
	}

	auto FContentBrowserModel::RequestDirectoryChildrenSnapshot(
		std::string_view PhysicalDirectory) -> void
	{
		if (!DataSource.IsAcceptingRequests()) return;
		const std::string Physical = NormalizePath(PhysicalDirectory);
		if (!DirectoryChildrenCache.contains(Physical)
			&& !(TreeTask.IsValid() && bActiveTreePublishable
				&& ActiveTreeDirectory == Physical))
			RequestedDirectoryChildrenSnapshots.insert(Physical);
	}

	auto FContentBrowserModel::RefreshRequestedDirectoryChildrenSnapshots() -> void
	{
		if (!DataSource.IsAcceptingRequests()) return;
		if (bAsync) return; // The frame pump schedules a bounded serial tree lane.
		Catalog = DataSource.PrepareDiscoveryCatalog(Catalog);
		std::vector<std::string> Requests(
			RequestedDirectoryChildrenSnapshots.begin(), RequestedDirectoryChildrenSnapshots.end());
		RequestedDirectoryChildrenSnapshots.clear();
		for (const std::string& Physical : Requests)
		{
			if (DirectoryChildrenCache.contains(Physical)) continue;
			auto Work = DataSource.PrepareDirectoryCapture(Physical, Catalog, EntryStatusQuery);
			if (Work) PublishDirectory(Physical, std::move(*Work()));
		}
	}

	auto FContentBrowserModel::PublishDirectory(const std::string& Physical,
		FContentBrowserDirectorySnapshot Snapshot) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.PublishDirectory");
		for (const auto& Diagnostic : Snapshot.Diagnostics)
		{
			if (EnumerationDiagnostics.size() < 8) EnumerationDiagnostics.push_back(Diagnostic);
			else ++SuppressedEnumerationDiagnosticCount;
		}
		SuppressedEnumerationDiagnosticCount += Snapshot.SuppressedDiagnosticCount;
		Snapshot.Version = ++SnapshotVersion;
		DirectoryChildrenCache[Physical] =
			std::make_shared<const FContentBrowserDirectorySnapshot>(std::move(Snapshot));
	}

	auto FContentBrowserModel::FindNearestAvailableDirectory(
		std::string_view PhysicalPath) const -> std::string
	{
		std::filesystem::path Directory = NormalizePath(PhysicalPath);
		while (!Directory.empty() && !IsDirectoryAvailable(Directory))
		{
			const std::filesystem::path Parent = Directory.parent_path();
			if (Parent == Directory) return {};
			Directory = Parent;
		}
		return Directory.generic_string();
	}

	auto FContentBrowserModel::QueryPathStatus(
		const std::filesystem::path& Path,
		std::error_code& Error) const -> std::filesystem::file_status
	{
		return PathStatusQuery
			? PathStatusQuery(Path, Error)
			: std::filesystem::status(Path, Error);
	}

	auto FContentBrowserModel::IsDirectoryAvailable(
		const std::filesystem::path& Path) const -> bool
	{
		std::error_code Error;
		const std::filesystem::file_status Status = QueryPathStatus(Path, Error);
		if (!Error && std::filesystem::is_directory(Status)) return true;
		const auto Query = DataSource.PrepareDiscoveryCatalog();
		if (!Query) return {};
		return std::ranges::any_of(Query->Items, [&](const auto& Item) {
			const auto Directory = std::filesystem::path(Item.PhysicalPath).parent_path();
			return ContentBrowserChanges::SamePath(Directory.generic_string(), Path.generic_string())
				|| FPaths::IsLexicalDescendantPath(Directory.generic_string(), Path.generic_string(), true);
		});
	}

	auto FContentBrowserModel::SetSnapshotForTesting(
		std::string CurrentDirectory,
		std::vector<FContentBrowserItem> Snapshot) -> void
	{
		CancelPendingSnapshots();
		Session.CurrentPhysicalPath = std::filesystem::path(CurrentDirectory)
								  .lexically_normal()
								  .generic_string();
		PublishedSnapshot = std::make_shared<const FContentBrowserItemsSnapshot>(
			FContentBrowserItemsSnapshot{.Items = std::move(Snapshot)});
		bSnapshotInjectedForTesting = true;
		DirectoryChildrenCache.clear();
		RequestedDirectoryChildrenSnapshots.clear();
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		RebuildItems();
	}
} // namespace Durin::ContentBrowser::Private
