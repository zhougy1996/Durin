#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserChanges.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "Asset/Asset.h"
#include "Profiling/Profiling.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		using ContentBrowserFilesystem::NormalizePath;

	} // namespace

	FContentBrowserModel::FContentBrowserModel(bool bInAsync, FTaskScopeToken Scope)
		: bAsync(bInAsync), TaskScope(std::move(Scope))
	{
	}

	FContentBrowserModel::~FContentBrowserModel()
	{
		CancelPendingSnapshots();
		// Module code must remain loaded until both owned worker bodies have exited.
		if (ItemsTask.IsValid()) (void)WaitTask(ItemsTask.GetTaskHandle());
		if (TreeTask.IsValid()) (void)WaitTask(TreeTask.GetTaskHandle());
	}

	auto FContentBrowserModel::InvalidateDirectoryTree() -> void
	{
		++TreeGeneration;
		if (TreeTask.IsValid()) (void)CancelTask(TreeTask.GetTaskHandle());
		DirectoryChildrenCache.clear();
		DirectoryGenerations.clear();
		RequestedDirectoryChildrenSnapshots.clear();
	}

	auto FContentBrowserModel::CancelPendingSnapshots() -> void
	{
		++ItemsGeneration;
		if (ItemsTask.IsValid()) (void)CancelTask(ItemsTask.GetTaskHandle());
		PendingItemsRequest.reset();
		bItemsLoading = false;
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
		if (CurrentMountedRevision && AcknowledgedMountedRevision)
		{
			const auto Current = CurrentMountedRevision();
			// Old captures wait for reconciliation; a new explicit or catalog-triggered
			// capture may use the current filesystem even while that revision is suppressed.
			if (Current != AcknowledgedMountedRevision() && Current != ValidatedMountedRevision) return false;
			ValidatedMountedRevision = Current;
		}
		// Unrelated catalog publications validate an old capture without restarting it.
		if (bItemsLoading && Catalog && ValidatedCatalogRevision != GetAssetCatalogRevision())
		{
			const auto Changes = CaptureAssetCatalogChanges(ValidatedCatalogRevision);
			ValidatedCatalogRevision = Changes.ToRevision;
			if (IsAffectedBy(Changes))
			{
				RefreshItemsSnapshot(Changes.bFullRefresh);
				return false;
			}
		}
		bool bPublished = false;
		if (ItemsTask.IsComplete())
		{
			if (ActiveItemsGeneration == ItemsGeneration
				&& ActiveItemsNavigationRevision == Session.NavigationRevision)
			{
				bItemsLoading = false;
				if (const auto Snapshot = ItemsTask.GetResultShared())
					PublishItems(std::move(**Snapshot));
				else
					ReportTaskFailure("Content scan did not complete: " + ItemsTask.GetDiagnostic());
				bPublished = true;
			}
			ItemsTask = {};
		}
		if (PendingItemsRequest && !ItemsTask.IsValid())
		{
			auto Request = std::move(*PendingItemsRequest);
			PendingItemsRequest.reset();
			ActiveItemsGeneration = ItemsGeneration;
			ActiveItemsNavigationRevision = Request.NavigationRevision;
			ValidatedMountedRevision = std::max(ValidatedMountedRevision, Request.MountedRevision);
			ItemsTask = LaunchCancelableTask<std::unique_ptr<FContentBrowserItemsSnapshot>>(
				"ContentBrowser.Items",
				[Source = DataSource, Request = std::move(Request), Query = EntryStatusQuery]
				(const FTaskCancellationToken& Cancellation) {
					Source->SetEntryStatusQueryForTesting(Query);
					return std::make_unique<FContentBrowserItemsSnapshot>(Source->CaptureItems(
						Request.Directory, Request.bRecursive, Request.Catalog, Cancellation));
				}, {.Attribution = RegisterTaskAttribution("ContentBrowser", "Items"), .Scope = TaskScope});
			if (!ItemsTask.IsValid())
			{
				bItemsLoading = false;
				ReportTaskFailure("Content scan could not be scheduled.");
				bPublished = true;
			}
		}
		if (TreeTask.IsComplete())
		{
			if (ActiveTreeGeneration == TreeGeneration
				&& ActiveTreeDirectoryGeneration == DirectoryGenerations[ActiveTreeDirectory])
			{
				if (auto Snapshot = TreeTask.GetResultShared())
					PublishDirectory(ActiveTreeDirectory, std::move(**Snapshot));
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
			ActiveTreeGeneration = TreeGeneration;
			ActiveTreeDirectoryGeneration = DirectoryGenerations[ActiveTreeDirectory];
			TreeTask = LaunchCancelableTask<std::unique_ptr<FContentBrowserDirectorySnapshot>>(
				"ContentBrowser.Directory",
				[Directory = ActiveTreeDirectory, Query = EntryStatusQuery]
				(const FTaskCancellationToken& Cancellation) {
					FContentBrowserDataSource Source;
					Source.SetEntryStatusQueryForTesting(Query);
					return std::make_unique<FContentBrowserDirectorySnapshot>(
						Source.CaptureDirectory(Directory, Cancellation));
				}, {.Attribution = RegisterTaskAttribution("ContentBrowser", "Directory"), .Scope = TaskScope});
			if (!TreeTask.IsValid())
			{
				ReportTaskFailure("Directory scan could not be scheduled.");
				DirectoryChildrenCache.emplace(ActiveTreeDirectory,
					std::make_shared<const FContentBrowserDirectorySnapshot>());
				ActiveTreeDirectory.clear();
			}
		}
		return bPublished;
	}

	auto FContentBrowserModel::WaitForPendingSnapshotsForTesting() -> void
	{
		do
		{
			(void)PumpPendingSnapshots();
			if (ItemsTask.IsValid()) (void)WaitTask(ItemsTask.GetTaskHandle());
			if (TreeTask.IsValid()) (void)WaitTask(TreeTask.GetTaskHandle());
		} while (ItemsTask.IsValid() || TreeTask.IsValid() || PendingItemsRequest
			|| (bAsync && !RequestedDirectoryChildrenSnapshots.empty()));
	}

	auto FContentBrowserModel::RefreshMountSnapshot() -> void
	{
		auto NextMountSnapshot = FContentBrowserPaths::CaptureMounts();

		const bool bUnchanged = std::ranges::equal(
			NextMountSnapshot,
			MountSnapshot,
			[](const FMountSnapshot& A, const FMountSnapshot& B) {
				return A.VirtualRoot == B.VirtualRoot
					&& A.SourcePhysicalRoot == B.SourcePhysicalRoot
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

	auto FContentBrowserModel::RescanRegistry() -> FAssetResult
	{
		const FAssetCatalogRefreshResult Refresh =
			RefreshAssetRegistry(
				EAssetRegistryScanMode::Incremental);
		if (Refresh) return {};
		return Refresh.Errors.empty()
			? FAssetResult{
				EAssetError::IoError,
				"Asset catalog refresh was incomplete."}
			: FAssetResult{
				EAssetError::IoError, Refresh.Errors.front().Message};
	}

	auto FContentBrowserModel::PhysicalToVirtualDirectory(
		std::string_view PhysicalPath) const -> std::string
	{
		const FMountPath Resolved = ResolveMountPath(PhysicalPath);
		if (!Resolved) return {};
		std::string Result = Resolved.VirtualPath;
		if (!Result.ends_with('/')) Result += '/';
		return Result;
	}

	auto FContentBrowserModel::ResolveMountPath(std::string_view PhysicalPath) const -> FMountPath
	{
		return FContentBrowserPaths::ResolveMountPath(PhysicalPath, MountSnapshot);
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

	auto FContentBrowserModel::IsInsideCurrentDirectory(
		std::string_view PhysicalPath,
		bool bRecursive) const -> bool
	{
		return FPaths::IsLexicalDescendantPath(
			NormalizePath(PhysicalPath), Session.CurrentPhysicalPath, bRecursive);
	}

	auto FContentBrowserModel::RevealAsset(std::string_view AssetPath)
		-> std::string
	{
		FTopLevelAssetPath Path;
		if (!FTopLevelAssetPath::TryCreate(AssetPath, Path)) return {};
		const FTopLevelAssetCatalogEntry Entry =
			FindTopLevelAssetExact(Path);
		if (!Entry) return {};
		if (Entry.Asset->IsRedirector())
			Session.Query.bShowRedirectors = true;
		const std::string Revealed = RevealPhysicalItem(Entry.Package->PhysicalPath);
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
		RefreshItemsSnapshot(false);

		if (bAsync && bItemsLoading)
		{
			std::error_code Error;
			return std::filesystem::exists(QueryPathStatus(PhysicalPath, Error)) && !Error
				? PhysicalPath : std::string{};
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
			++DirectoryGenerations[ActiveTreeDirectory];
			if (TreeTask.IsValid()) (void)CancelTask(TreeTask.GetTaskHandle());
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
		++ItemsGeneration;
		if (ItemsTask.IsValid()) (void)CancelTask(ItemsTask.GetTaskHandle());
		if (!Catalog || Catalog->Revision != GetAssetCatalogRevision())
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.CaptureCatalog");
			Catalog = std::make_shared<const FAssetCatalogSnapshot>(CaptureAssetCatalogSnapshot());
		}
		ValidatedCatalogRevision = Catalog->Revision;
		ValidatedMountedRevision = CurrentMountedRevision ? CurrentMountedRevision() : 0;
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		if (bAsync)
		{
			// Do not expose actionable rows from a previous directory or revision.
			PublishedSnapshot = std::make_shared<const FContentBrowserItemsSnapshot>();
			Items = {};
			bItemsLoading = true;
			PendingItemsRequest = FItemsRequest{Session.CurrentPhysicalPath, !Session.Query.Search.empty(), Catalog,
				Session.NavigationRevision, ValidatedMountedRevision};
			(void)PumpPendingSnapshots();
			return;
		}
		DataSource->SetEntryStatusQueryForTesting(EntryStatusQuery);
		PublishItems(DataSource->CaptureItems(Session.CurrentPhysicalPath, !Session.Query.Search.empty(), Catalog));
	}

	auto FContentBrowserModel::PublishItems(FContentBrowserItemsSnapshot Snapshot) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("ContentBrowser.PublishItems");
		Snapshot.Version = ++SnapshotVersion;
		for (FContentBrowserItem& Item : Snapshot.Items)
		{
			if (Item.Kind == EContentBrowserItemKind::Folder)
				Item.VirtualPath = PhysicalToVirtualDirectory(Item.PhysicalPath);
			if ((Item.Kind == EContentBrowserItemKind::Asset
				|| Item.Kind == EContentBrowserItemKind::Redirector)
				&& ::Durin::Editor::GetDefaultThumbnailManager().Find(Item.AssetClassName))
				Item.ThumbnailIdentity = Item.VirtualPath;
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

	auto FContentBrowserModel::GetDirectoryChildren(std::string_view PhysicalDirectory) const
		-> std::span<const std::filesystem::path>
	{
		const auto Snapshot = GetDirectorySnapshot(PhysicalDirectory);
		return Snapshot ? std::span<const std::filesystem::path>(Snapshot->Children)
			: std::span<const std::filesystem::path>{};
	}

	auto FContentBrowserModel::HasDirectoryChildrenSnapshot(
		std::string_view PhysicalDirectory) const -> bool
	{
		return DirectoryChildrenCache.contains(NormalizePath(PhysicalDirectory));
	}

	auto FContentBrowserModel::RequestDirectoryChildrenSnapshot(
		std::string_view PhysicalDirectory) -> void
	{
		const std::string Physical = NormalizePath(PhysicalDirectory);
		if (!DirectoryChildrenCache.contains(Physical)
			&& !(TreeTask.IsValid() && ActiveTreeGeneration == TreeGeneration
				&& ActiveTreeDirectoryGeneration == DirectoryGenerations[ActiveTreeDirectory]
				&& ActiveTreeDirectory == Physical))
			RequestedDirectoryChildrenSnapshots.insert(Physical);
	}

	auto FContentBrowserModel::RefreshRequestedDirectoryChildrenSnapshots() -> void
	{
		if (bAsync) return; // The frame pump schedules a bounded serial tree lane.
		std::vector<std::string> Requests(
			RequestedDirectoryChildrenSnapshots.begin(), RequestedDirectoryChildrenSnapshots.end());
		RequestedDirectoryChildrenSnapshots.clear();
		DataSource->SetEntryStatusQueryForTesting(EntryStatusQuery);
		for (const std::string& Physical : Requests)
		{
			if (DirectoryChildrenCache.contains(Physical)) continue;
			PublishDirectory(Physical, DataSource->CaptureDirectory(Physical));
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
		return !Error && std::filesystem::is_directory(Status);
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
		DirectoryGenerations.clear();
		RequestedDirectoryChildrenSnapshots.clear();
		EnumerationDiagnostics.clear();
		SuppressedEnumerationDiagnosticCount = 0;
		RebuildItems();
	}
} // namespace Durin::Editor::ContentBrowser::Private
