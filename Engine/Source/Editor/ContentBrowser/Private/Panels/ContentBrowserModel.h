#pragma once

#include "Panels/ContentBrowserDataSource.h"
#include "Operations/ContentBrowserPaths.h"
#include "Panels/ContentBrowserSession.h"

#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace Durin::Editor::ContentBrowser::Private
{
	// Coordinates capture publication with the browser session and indexed query projection.
	class FContentBrowserModel
	{
	public:
		// Standalone synchronous mode supports callers without a frame pump.
		// Editor panels opt into asynchronous mode and pump before drawing.
		explicit FContentBrowserModel(bool bAsync = false, FTaskScopeToken Scope = {});
		~FContentBrowserModel();
		// Owner-thread, nonblocking frame pump; true means item publication or failure
		// completed and the panel may repair its selection against current rows.
		auto PumpPendingSnapshots() -> bool;
		auto IsLoading() const -> bool { return bItemsLoading; }
		auto GetNavigationRevision() const -> uint64 { return Session.NavigationRevision; }
		auto GetRequestGeneration() const -> uint64 { return ItemsGeneration; }
		// Invalidates publication and requests cooperative cancellation without waiting.
		auto CancelPendingSnapshots() -> void;
		auto WaitForPendingSnapshotsForTesting() -> void;

		using EEnumerationDiagnosticKind = Private::EEnumerationDiagnosticKind;
		using FEnumerationDiagnostic = Private::FEnumerationDiagnostic;
		using FEntryStatusQuery = Private::FEntryStatusQuery;

		using FPathStatusQuery = std::function<std::filesystem::file_status(
			const std::filesystem::path&,
			std::error_code&)>;

		using FMountSnapshot = FContentBrowserPaths::FMountSnapshot;
		using FMountPath = FContentBrowserPaths::FMountPath;
		auto RefreshMountSnapshot() -> void;
		auto RescanRegistry() -> FAssetResult;
		// Async mode replaces the pending request and clears actionable rows immediately.
		auto RefreshItemsSnapshot(bool bInvalidateDirectoryTree = true) -> void;
		auto RebuildItems() -> void;
		auto NavigateToPhysical(std::string_view PhysicalPath, bool bAddHistory = true) -> bool;
		auto NavigateHistory(int32 Delta) -> bool;

		auto SetSearch(std::string_view Search) -> void;
		auto SetTypeFilter(EContentBrowserTypeFilter Filter) -> void;
		auto SetSort(EContentBrowserSortColumn Column, bool bAscending) -> void;
		auto SetShowHiddenFiles(bool bShow) -> void;
		auto SetShowRedirectors(bool bShow) -> void;

		auto PhysicalToVirtualDirectory(std::string_view PhysicalPath) const -> std::string;
		auto ResolveMountPath(std::string_view PhysicalPath) const -> FMountPath;
		auto VirtualToPhysical(std::string_view VirtualPath) const -> std::string;
		auto IsInsideCurrentDirectory(std::string_view PhysicalPath, bool bRecursive) const -> bool;
		// Async mode returns the accepted identity; visibility is resolved after publication.
		auto RevealPhysicalItem(std::string_view PhysicalPath) -> std::string;
		auto RevealAsset(std::string_view AssetPath) -> std::string;

		auto GetCurrentPhysicalPath() const -> const std::string& { return Session.CurrentPhysicalPath; }
		auto GetCurrentVirtualPath() const -> const std::string& { return Session.CurrentVirtualPath; }
		auto GetMounts() const -> std::span<const FMountSnapshot> { return MountSnapshot; }
		// Copy the range to retain its snapshot and ordering across a refresh.
		auto GetItems() const -> const FContentBrowserItemRange& { return Items; }
		auto GetItemsSnapshot() const -> std::span<const FContentBrowserItem> { return PublishedSnapshot->Items; }
		auto GetHistory() const -> std::span<const std::string> { return Session.NavigationHistory; }
		auto GetHistoryIndex() const -> int32 { return Session.HistoryIndex; }
		auto GetSearch() const -> const std::string& { return Session.Query.Search; }
		auto GetTypeFilter() const -> EContentBrowserTypeFilter { return Session.Query.TypeFilter; }
		auto GetSortColumn() const -> EContentBrowserSortColumn { return Session.Query.SortColumn; }
		auto IsSortAscending() const -> bool { return Session.Query.bSortAscending; }
		auto IsShowingHiddenFiles() const -> bool { return Session.Query.bShowHiddenFiles; }
		auto IsShowingRedirectors() const -> bool { return Session.Query.bShowRedirectors; }
		auto GetEnumerationDiagnostics() const
			-> std::span<const FEnumerationDiagnostic> { return EnumerationDiagnostics; }
		auto GetSuppressedEnumerationDiagnosticCount() const -> size_t
		{
			return SuppressedEnumerationDiagnosticCount;
		}

		// Retaining this owner keeps children valid across cache invalidation.
		auto GetDirectorySnapshot(std::string_view PhysicalDirectory) const
			-> std::shared_ptr<const FContentBrowserDirectorySnapshot>;
		auto GetSnapshotVersion() const -> uint64 { return PublishedSnapshot->Version; }
		// Borrowed view for immediate use; retain GetDirectorySnapshot during traversal.
		auto GetDirectoryChildren(std::string_view PhysicalDirectory) const
			-> std::span<const std::filesystem::path>;
		auto HasDirectoryChildrenSnapshot(std::string_view PhysicalDirectory) const
			-> bool;
		// Queues tree-node I/O for the frame pump (or explicit synchronous refresh).
		auto RequestDirectoryChildrenSnapshot(std::string_view PhysicalDirectory)
			-> void;
		auto RefreshRequestedDirectoryChildrenSnapshots() -> void;
		auto FindNearestAvailableDirectory(std::string_view PhysicalPath) const
			-> std::string;

		// Replaces the captured data without filesystem access for deterministic model tests.
		auto SetSnapshotForTesting(
			std::string CurrentDirectory,
			std::vector<FContentBrowserItem> Snapshot
		) -> void;
		// Async capture copies this hook; its captures must be safe on worker threads.
		auto SetEntryStatusQueryForTesting(FEntryStatusQuery Query) -> void
		{
			EntryStatusQuery = std::move(Query);
		}
		auto SetPathStatusQueryForTesting(FPathStatusQuery Query) -> void
		{
			PathStatusQuery = std::move(Query);
		}

	private:
		auto PublishItems(FContentBrowserItemsSnapshot Snapshot) -> void;
		auto PublishDirectory(const std::string& Physical,
			FContentBrowserDirectorySnapshot Snapshot) -> void;
		auto InvalidateDirectoryTree() -> void;
		auto ReportTaskFailure(std::string Message) -> void;
		auto QueryPathStatus(
			const std::filesystem::path& Path,
			std::error_code& Error) const -> std::filesystem::file_status;
		auto IsDirectoryAvailable(const std::filesystem::path& Path) const -> bool;

		FContentBrowserSession Session;
		std::vector<FMountSnapshot> MountSnapshot;
		std::unordered_map<std::string, std::shared_ptr<const FContentBrowserDirectorySnapshot>> DirectoryChildrenCache;
		std::unordered_set<std::string> RequestedDirectoryChildrenSnapshots;
		// Only the serial item worker touches its collector; captures pin its lifetime.
		std::shared_ptr<FContentBrowserDataSource> DataSource =
			std::make_shared<FContentBrowserDataSource>();
		std::shared_ptr<const FAssetCatalogSnapshot> Catalog;
		FEntryStatusQuery EntryStatusQuery;
		bool bAsync = false;
		bool bItemsLoading = false;
		FTaskScopeToken TaskScope;
		uint64 SnapshotVersion = 0;
		uint64 ItemsGeneration = 0;
		uint64 TreeGeneration = 0;
		uint64 ActiveItemsGeneration = 0;
		uint64 ActiveTreeGeneration = 0;
		// Latest pending request owns all inputs needed after the active worker exits.
		struct FItemsRequest
		{
			std::string Directory;
			bool bRecursive = false;
			std::shared_ptr<const FAssetCatalogSnapshot> Catalog;
		};
		std::optional<FItemsRequest> PendingItemsRequest;
		// The model alone consumes these move-only results after worker completion.
		TTaskHandle<std::unique_ptr<FContentBrowserItemsSnapshot>> ItemsTask;
		TTaskHandle<std::unique_ptr<FContentBrowserDirectorySnapshot>> TreeTask;
		std::string ActiveTreeDirectory;

		std::shared_ptr<const FContentBrowserItemsSnapshot> PublishedSnapshot =
			std::make_shared<const FContentBrowserItemsSnapshot>();
		FContentBrowserItemRange Items;
		std::vector<FEnumerationDiagnostic> EnumerationDiagnostics;
		size_t SuppressedEnumerationDiagnosticCount = 0;
		FPathStatusQuery PathStatusQuery;
		bool bSnapshotInjectedForTesting = false;
	};
} // namespace Durin::Editor::ContentBrowser::Private
