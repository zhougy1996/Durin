#pragma once

#include "AssetSystem.h"

#include <filesystem>
#include <unordered_map>

namespace Durin
{
	// Distinguishes folders, registered assets, and ordinary files.
	enum class EContentBrowserItemKind : uint8
	{
		Folder,
		Asset,
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
		Materials,
		Textures,
		OtherAssets
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
		// Maps one virtual mount to its source and imported physical roots.
		struct FMountSnapshot
		{
			std::string VirtualRoot;
			std::string SourcePhysicalRoot;
			std::string PhysicalRoot;
			bool bAuthoringWritable = false;
		};

		auto RefreshMountSnapshot() -> void;
		auto RescanRegistry() -> Asset::FAssetResult;
		auto RefreshItemsSnapshot() -> void;
		auto RebuildItems() -> void;
		auto NavigateToPhysical(std::string_view PhysicalPath, bool bAddHistory = true) -> bool;
		auto NavigateHistory(int32 Delta) -> bool;

		auto SetSearch(std::string_view Search) -> void;
		auto SetTypeFilter(EContentBrowserTypeFilter Filter) -> void;
		auto SetSort(EContentBrowserSortColumn Column, bool bAscending) -> void;
		auto SetShowHiddenFiles(bool bShow) -> void;

		auto PhysicalToVirtualDirectory(std::string_view PhysicalPath) const -> std::string;
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

		auto GetDirectoryChildren(std::string_view PhysicalDirectory)
			-> std::span<const std::filesystem::path>;

		// Replaces the captured data without filesystem access for deterministic model tests.
		auto SetSnapshotForTesting(
			std::string CurrentDirectory,
			std::vector<FContentBrowserItem> Snapshot
		) -> void;

	private:
		auto MatchesTypeFilter(const FContentBrowserItem& Item) const -> bool;

		std::string CurrentPhysicalPath;
		std::string CurrentVirtualPath;
		std::vector<FMountSnapshot> MountSnapshot;
		std::unordered_map<std::string, std::vector<std::filesystem::path>> DirectoryChildrenCache;
		std::vector<FContentBrowserItem> ItemsSnapshot;
		std::vector<FContentBrowserItem> Items;
		std::vector<std::string> NavigationHistory;
		int32 HistoryIndex = -1;
		std::string Search;
		EContentBrowserTypeFilter TypeFilter = EContentBrowserTypeFilter::All;
		EContentBrowserSortColumn SortColumn = EContentBrowserSortColumn::Name;
		bool bSortAscending = true;
		bool bShowHiddenFiles = false;
	};
} // namespace Durin
