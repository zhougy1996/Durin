#pragma once

#include "AssetRegistry/Catalog.h"

#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace Durin::Editor::ContentBrowser::Private
{
	// Distinguishes folders, registered assets, and ordinary files.
	enum class EContentBrowserItemKind : uint8
	{
		Folder,
		Asset,
		Redirector,
		File
	};

	// Selects the content category shown without changing directory navigation.
	enum class EContentBrowserTypeFilter : uint8
	{
		All,
		Assets,
		Files,
		Levels,
		StaticMeshes,
		SkeletalAssets,
		Materials,
		Textures,
		OtherAssets,
		Redirectors
	};

	// Identifies the active content-browser sort key.
	enum class EContentBrowserSortColumn : uint8
	{
		Name,
		Type,
		Size,
		Modified
	};

	// Captures one mounted content item and its searchable metadata.
	struct FContentBrowserItem
	{
		EContentBrowserItemKind Kind = EContentBrowserItemKind::File;
		std::string Name;
		std::string VirtualPath;
		std::string PhysicalPath;
		std::string AssetClassName;
		std::string Extension;
		FAssetPath RedirectDestination;
		std::string ThumbnailIdentity;
		std::string ThumbnailSourcePath;
		uintmax_t ThumbnailFileSize = 0;
		std::filesystem::file_time_type ThumbnailLastWriteTime{};
		uint32 ThumbnailPackageFormatVersion = 0;
		int64 ThumbnailLastWriteTimeTicks = 0;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};

		auto StableId() const -> const std::string& { return PhysicalPath; }
	};

	namespace ContentBrowserModel
	{
		// Returns the searchable, user-facing type name derived from item metadata.
		auto TypeLabel(const FContentBrowserItem& Item) -> std::string;
	}

	// Owns the content browser's mounted-location snapshot and deterministic item projection.
	class FContentBrowserModel
	{
	public:
		enum class EEnumerationDiagnosticKind : uint8
		{
			Entry,
			Traversal,
		};

		struct FEnumerationDiagnostic
		{
			EEnumerationDiagnosticKind Kind = EEnumerationDiagnosticKind::Entry;
			std::string PhysicalPath;
			std::string Message;
		};

		using FEntryStatusQuery = std::function<std::filesystem::file_status(
			const std::filesystem::directory_entry&,
			std::error_code&)>;
		using FPathStatusQuery = std::function<std::filesystem::file_status(
			const std::filesystem::path&,
			std::error_code&)>;

		// Maps one virtual mount to its source and imported physical roots.
		struct FMountSnapshot
		{
			std::string VirtualRoot;
			std::string SourcePhysicalRoot;
			std::string PhysicalRoot;
			bool bContentWritable = false;
		};

		struct FMountPath
		{
			const FMountSnapshot* Mount = nullptr;
			std::string NormalizedPhysicalPath;
			std::string VirtualPath;

			explicit operator bool() const { return Mount != nullptr; }
		};

		auto RefreshMountSnapshot() -> void;
		auto RescanRegistry() -> Asset::FAssetResult;
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
		auto RevealAsset(std::string_view AssetPath) -> std::string;

		auto GetCurrentPhysicalPath() const -> const std::string& { return CurrentPhysicalPath; }
		auto GetCurrentVirtualPath() const -> const std::string& { return CurrentVirtualPath; }
		auto GetMounts() const -> std::span<const FMountSnapshot> { return MountSnapshot; }
		auto GetItems() const -> std::span<const FContentBrowserItem> { return Items; }
		auto GetItemsSnapshot() const -> std::span<const FContentBrowserItem> { return ItemsSnapshot; }
		auto GetHistory() const -> std::span<const std::string> { return NavigationHistory; }
		auto GetHistoryIndex() const -> int32 { return HistoryIndex; }
		auto GetSearch() const -> const std::string& { return Search; }
		auto GetTypeFilter() const -> EContentBrowserTypeFilter { return TypeFilter; }
		auto GetSortColumn() const -> EContentBrowserSortColumn { return SortColumn; }
		auto IsSortAscending() const -> bool { return bSortAscending; }
		auto IsShowingHiddenFiles() const -> bool { return bShowHiddenFiles; }
		auto IsShowingRedirectors() const -> bool { return bShowRedirectors; }
		auto GetEnumerationDiagnostics() const
			-> std::span<const FEnumerationDiagnostic> { return EnumerationDiagnostics; }
		auto GetSuppressedEnumerationDiagnosticCount() const -> size_t
		{
			return SuppressedEnumerationDiagnosticCount;
		}

		// The returned span survives insertion of distinct cache entries, but not
		// invalidation or mutation of its published entry.
		auto GetDirectoryChildren(std::string_view PhysicalDirectory) const
			-> std::span<const std::filesystem::path>;
		auto HasDirectoryChildrenSnapshot(std::string_view PhysicalDirectory) const
			-> bool;
		// Queues tree-node I/O for the next pre-draw model refresh.
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
		auto SetEntryStatusQueryForTesting(FEntryStatusQuery Query) -> void
		{
			EntryStatusQuery = std::move(Query);
		}
		auto SetPathStatusQueryForTesting(FPathStatusQuery Query) -> void
		{
			PathStatusQuery = std::move(Query);
		}

	private:
		struct FIndexedAsset
		{
			const FAssetPath* Path = nullptr;
			const Asset::FAssetData* Data = nullptr;
		};

		auto QueryEntryStatus(
			const std::filesystem::directory_entry& Entry,
			std::error_code& Error) const -> std::filesystem::file_status;
		auto QueryPathStatus(
			const std::filesystem::path& Path,
			std::error_code& Error) const -> std::filesystem::file_status;
		auto IsDirectoryAvailable(const std::filesystem::path& Path) const -> bool;
		auto RefreshAssetDirectoryIndex() -> void;
		auto AppendAssetItem(const FAssetPath& Path, const Asset::FAssetData& Data)
			-> void;
		auto AddEnumerationDiagnostic(
			EEnumerationDiagnosticKind Kind,
			const std::filesystem::path& Path,
			std::string Message) -> void;
		auto MatchesTypeFilter(const FContentBrowserItem& Item) const -> bool;

		std::string CurrentPhysicalPath;
		std::string CurrentVirtualPath;
		std::vector<FMountSnapshot> MountSnapshot;
		std::unordered_map<std::string, std::vector<std::filesystem::path>> DirectoryChildrenCache;
		std::unordered_set<std::string> RequestedDirectoryChildrenSnapshots;
		Asset::FAssetCatalogSnapshot AssetCatalogSnapshot;
		std::unordered_map<std::string, std::vector<FIndexedAsset>> AssetDirectoryIndex;
		std::vector<FContentBrowserItem> ItemsSnapshot;
		std::vector<FContentBrowserItem> Items;
		std::vector<FEnumerationDiagnostic> EnumerationDiagnostics;
		size_t SuppressedEnumerationDiagnosticCount = 0;
		FEntryStatusQuery EntryStatusQuery;
		FPathStatusQuery PathStatusQuery;
		std::vector<std::string> NavigationHistory;
		int32 HistoryIndex = -1;
		std::string Search;
		EContentBrowserTypeFilter TypeFilter = EContentBrowserTypeFilter::All;
		EContentBrowserSortColumn SortColumn = EContentBrowserSortColumn::Name;
		bool bSortAscending = true;
		bool bShowHiddenFiles = false;
		bool bShowRedirectors = false;
		bool bSnapshotInjectedForTesting = false;
	};
} // namespace Durin::Editor::ContentBrowser::Private
