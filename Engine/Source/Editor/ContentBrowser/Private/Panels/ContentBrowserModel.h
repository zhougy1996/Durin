#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Misc/ContentChanges.h"
#include "Asset/LoadErrors.h"
#include "Threading/Task.h"

#include "ContentBrowserDataQuery.h"
#include "Misc/ContentMountPaths.h"
#include "Panels/ContentBrowserSession.h"

namespace Durin::ContentBrowser::Private
{
	// Coordinates capture publication with the browser session and indexed query projection.
	class FContentBrowserModel
	{
		friend struct FContentBrowserModelFixtureAccess;
	public:
		explicit FContentBrowserModel(FContentBrowserDataQuery& InDataQuery);
		~FContentBrowserModel();
		FContentBrowserModel(const FContentBrowserModel&) = delete;
		auto operator=(const FContentBrowserModel&) -> FContentBrowserModel& = delete;
		// Owner-thread, nonblocking frame pump; true means item publication or failure
		// completed and the panel may repair its selection against current rows.
		auto PumpPendingSnapshots() -> bool;
		// Reconciliation suspends existing captures; a fresh explicit capture starts a new lifecycle.
		auto SetSnapshotPublicationSuspended(bool bSuspended) -> void { bSnapshotPublicationSuspended = bSuspended; }
		auto IsLoading() const -> bool { return ItemsRequest && ItemsRequest->State != EItemsRequestState::Completed; }
		auto GetNavigationRevision() const -> uint64 { return Session.NavigationRevision; }
		// Invalidates publication and requests cooperative cancellation without waiting.
		auto CancelPendingSnapshots() -> void;

		using EEnumerationDiagnosticKind = Private::EEnumerationDiagnosticKind;
		using FEnumerationDiagnostic = Private::FEnumerationDiagnostic;
		using FEntryStatusQuery = Private::FEntryStatusQuery;

		using FPathStatusQuery = std::function<std::filesystem::file_status(
			const std::filesystem::path&,
			std::error_code&)>;

		using FMountSnapshot = ContentStorage::FContentMountPaths::FMountSnapshot;
		using FMountPath = ContentStorage::FContentMountPaths::FMountPath;
		auto RefreshMountSnapshot() -> void;
		auto RescanRegistry() -> std::expected<void, Durin::UnifiedError::FError>;
		// Replaces the pending request and clears actionable rows immediately.
		auto RefreshItemsSnapshot(bool bInvalidateDirectoryTree = true) -> void;
		auto RebuildItems() -> void;
		auto ApplyContentChanges(const FContentChangeBatch& Changes, bool bInferMissingParents = true) -> bool;
		auto IsAffectedBy(const FContentChangeBatch& Changes, bool bInferMissingParents = true) const -> bool;
		auto NavigateToPhysical(std::string_view PhysicalPath, bool bAddHistory = true) -> bool;
		auto NavigateToVirtual(std::string_view VirtualPath, bool bAddHistory = true) -> bool;
		auto NavigateHistory(int32 Delta) -> bool;

		auto SetSearch(std::string_view Search) -> void;
		auto SetTypeFilter(EContentBrowserTypeFilter Filter) -> void;
		auto SetCategoryPath(std::vector<std::string> Path) -> void;
		auto GetCategoryPath() const -> const std::vector<std::string>& { return Session.Query.CategoryPath; }
		auto SetSort(EContentBrowserSortColumn Column, bool bAscending) -> void;
		auto SetShowHiddenFiles(bool bShow) -> void;
		auto SetShowRedirectors(bool bShow) -> void;

		auto PhysicalToVirtualDirectory(std::string_view PhysicalPath) const -> std::string;
		auto ResolveMountPath(std::string_view PhysicalPath) const -> FMountPath;
		auto VirtualToPhysical(std::string_view VirtualPath) const -> std::string;
		// Returns the accepted identity; visibility is resolved after publication.
		auto RevealPhysicalItem(std::string_view PhysicalPath) -> FContentBrowserItemKey;
		auto RevealAsset(std::string_view AssetPath) -> FContentBrowserItemKey;

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
		auto HasDirectoryChildrenSnapshot(std::string_view PhysicalDirectory) const
			-> bool;
		// Queues tree-node I/O for the frame pump.
		auto RequestDirectoryChildrenSnapshot(std::string_view PhysicalDirectory)
			-> void;
		auto FindNearestAvailableDirectory(std::string_view PhysicalPath) const
			-> std::string;

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
		auto PublishItems(FContentBrowserItemsCapture Capture) -> void;
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
		// Per-view query state outlives the model; live source access stays on the owner thread.
		FContentBrowserDataQuery& DataQuery;
		FEntryStatusQuery EntryStatusQuery;
		bool bSnapshotPublicationSuspended = false;
		FDelegateHandle ItemsUpdated, ItemsRefreshed;
		FContentChangeBatch PendingChanges;
		std::shared_ptr<void> EventLifetime = std::make_shared<int>(0);
		bool bActiveTreePublishable = false;
		enum class EItemsRequestState : uint8 { Queued, Capturing, Completed, Cancelled };
		// The request owns its virtual scope and publication rights. Replacing it retires publication rights.
		struct FItemsRequest
		{
			std::string Directory;
			bool bRecursive = false;
			EItemsRequestState State = EItemsRequestState::Queued;
		};
		std::shared_ptr<FItemsRequest> ItemsRequest;
		std::shared_ptr<FItemsRequest> ActiveItemsRequest;
		// The model alone consumes these move-only results after worker completion.
		std::shared_ptr<Tasks::FCancellationToken> ItemsCancellation = std::make_shared<Tasks::FCancellationToken>();
		std::shared_ptr<Tasks::FCancellationToken> TreeCancellation = std::make_shared<Tasks::FCancellationToken>();
		Tasks::TTask<std::unique_ptr<FContentBrowserItemsCapture>> ItemsTask;
		Tasks::TTask<std::unique_ptr<FContentBrowserDirectorySnapshot>> TreeTask;
		std::string ActiveTreeDirectory;

		std::shared_ptr<const FContentBrowserItemsSnapshot> PublishedSnapshot =
			std::make_shared<const FContentBrowserItemsSnapshot>();
		FContentBrowserItemRange Items;
		std::vector<FEnumerationDiagnostic> EnumerationDiagnostics;
		size_t SuppressedEnumerationDiagnosticCount = 0;
		FPathStatusQuery PathStatusQuery;
	};
} // namespace Durin::ContentBrowser::Private
