#pragma once

#include "Panels/LevelEditorPanel.h"

#include <array>
#include <filesystem>
#include <unordered_set>

namespace Durin
{
	class FEditorSessionSettings;
	class FSourceImageThumbnailCache;
	struct FLevelEditorContext;

	enum class EContentBrowserItemKind : uint8
	{
		Folder,
		Asset,
		SourceFile
	};

	enum class EContentBrowserViewMode : uint8
	{
		Grid,
		Details
	};

	struct FContentBrowserItem
	{
		EContentBrowserItemKind Kind = EContentBrowserItemKind::SourceFile;
		std::string Name;
		std::string VirtualPath;
		std::string PhysicalPath;
		std::string AssetClassName;
		std::string Extension;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};

		auto StableId() const -> const std::string& { return PhysicalPath; }
	};

	class FContentBrowserPanel final : public ILevelEditorPanel
	{
	public:
		using FOpenAsset = std::function<bool(const std::string&, const std::string&)>;
		using FRequestImport = std::function<void(const std::string&)>;

		FContentBrowserPanel(FEditorSessionSettings& InSessionSettings, FOpenAsset InOpenAsset, FRequestImport InRequestImport);
		~FContentBrowserPanel() override;

		auto GetWindowName() const -> const char* override { return "Content Browser"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto RevealAsset(std::string_view AssetPath) -> void;

	private:
		enum class ESortColumn : uint8
		{
			Name,
			Type,
			Size,
			Modified
		};

		auto Refresh(bool bRescanRegistry) -> void;
		auto RebuildItems() -> void;
		auto NavigateToPhysical(std::string_view PhysicalPath, bool bAddHistory = true) -> bool;
		auto NavigateHistory(int32 Delta) -> void;
		auto PhysicalToVirtualDirectory(std::string_view PhysicalPath) const -> std::string;
		auto VirtualToPhysical(std::string_view VirtualPath) const -> std::string;
		auto IsInsideCurrentDirectory(std::string_view PhysicalPath, bool bRecursive) const -> bool;

		auto DrawToolbar() -> void;
		auto DrawDirectoryTree() -> void;
		auto DrawDirectoryNode(const std::filesystem::path& Path, std::string_view Label, bool bMountRoot) -> void;
		auto DrawContentArea() -> void;
		auto DrawGrid() -> void;
		auto DrawDetails() -> void;
		auto DrawItemContextMenu(const FContentBrowserItem& Item) -> void;
		auto DrawBackgroundContextMenu() -> void;
		auto DrawStatusBar() -> void;
		auto DrawSelectionDetails() -> void;
		auto DrawDialogs() -> void;
		auto BeginAssetDragDrop(const FContentBrowserItem& Item) -> void;
		auto AcceptAssetDrop(std::string_view DestinationVirtualDirectory) -> void;

		auto SelectItem(size_t Index) -> void;
		auto OpenItem(const FContentBrowserItem& Item) -> void;
		auto BeginRename(const FContentBrowserItem& Item) -> void;
		auto DrawRenameEditor(const FContentBrowserItem& Item) -> void;
		auto CommitRename(const FContentBrowserItem& Item) -> bool;
		auto RenameFolder(const FContentBrowserItem& Item, std::string_view NewName) -> bool;
		auto IsManagedCompanion(const FContentBrowserItem& Item) const -> bool;
		auto CreateFolder() -> void;
		auto RequestDeleteSelection() -> void;
		auto DeleteSelection() -> void;
		auto DeleteEmptyFolder(const FContentBrowserItem& Item) -> bool;
		auto ShowInExplorer(std::string_view PhysicalPath) const -> void;
		auto CopyToClipboard(std::string_view Text) const -> void;

		auto ItemTypeLabel(const FContentBrowserItem& Item) const -> std::string;
		auto ItemIcon(const FContentBrowserItem& Item) const -> const char*;
		auto FormatFileSize(uintmax_t Bytes) const -> std::string;
		auto FormatFileTime(const std::filesystem::file_time_type& Time) const -> std::string;
		auto MatchesTypeFilter(const FContentBrowserItem& Item) const -> bool;
		auto SetError(std::string Message) -> void;

		FEditorSessionSettings& SessionSettings;
		FOpenAsset OpenAsset;
		FRequestImport RequestImport;
		std::string CurrentPhysicalPath;
		std::string CurrentVirtualPath;
		std::vector<FContentBrowserItem> Items;
		std::vector<std::string> NavigationHistory;
		int32 HistoryIndex = -1;
		std::unordered_set<std::string> Selection;
		std::string SelectionAnchor;
		std::array<char, 256> SearchBuffer{};
		int32 TypeFilter = 0;
		EContentBrowserViewMode ViewMode = EContentBrowserViewMode::Grid;
		ESortColumn SortColumn = ESortColumn::Name;
		bool bSortAscending = true;
		bool bShowSourceFiles = false;
		bool bShowSelectionDetails = false;
		bool bIconSizeLocked = false;
		bool bContentItemHovered = false;
		float IconSize;
		float DirectoryTreeWidth;
		std::string RenameTarget;
		std::array<char, 256> RenameBuffer{};
		bool bFocusRename = false;
		bool bRenameEditorHovered = false;
		bool bDeletePopupRequested = false;
		std::string ErrorMessage;
		std::unique_ptr<FSourceImageThumbnailCache> ThumbnailCache;
	};
} // namespace Durin
