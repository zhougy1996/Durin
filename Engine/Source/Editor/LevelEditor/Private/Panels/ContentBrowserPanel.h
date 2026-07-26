#pragma once

#include "Panels/LevelEditorPanel.h"
#include "Assets/EditorAssetMoveCoordinator.h"

#include <array>
#include <filesystem>
#include <unordered_set>

namespace Durin
{
	class FLevelEditorSessionSettings;
	class FContentBrowserThumbnailCache;
	struct FLevelEditorContext;

	// Distinguishes folders, assets, and source files in the content browser.
	enum class EContentBrowserItemKind : uint8
	{
		Folder,
		Asset,
		SourceFile
	};

	// Selects grid or list presentation for content-browser items.
	enum class EContentBrowserViewMode : uint8
	{
		Grid,
		Details
	};

	// Selects the import workflow inferred for an external file.
	enum class EContentBrowserImportType : uint8
	{
		Texture,
		TextureCube,
		StaticMesh
	};

	// Captures one mounted content item and its searchable metadata.
	struct FContentBrowserItem
	{
		EContentBrowserItemKind Kind = EContentBrowserItemKind::SourceFile;
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

	// Owns navigation, filtering, thumbnails, drag/drop, and import UI for assets.
	class FContentBrowserPanel final : public ILevelEditorPanel
	{
	public:
		using FOpenAsset = std::function<bool(const std::string&, const std::string&)>;
		using FRequestImport = std::function<void(const std::string&, EContentBrowserImportType)>;
		using FMoveAssets = std::function<Asset::FAssetResult(std::span<const FEditorAssetMove>)>;

		FContentBrowserPanel(FLevelEditorSessionSettings& InSessionSettings, FOpenAsset InOpenAsset, FRequestImport InRequestImport, FMoveAssets InMoveAssets);
		~FContentBrowserPanel() override;

		auto GetWindowName() const -> const char* override { return "Content Browser"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto RevealAsset(std::string_view AssetPath) -> void;

	private:
		// Maps one virtual mount to its source and imported physical roots.
		struct FMountSnapshot
		{
			std::string VirtualRoot;
			std::string SourcePhysicalRoot;
			std::string PhysicalRoot;
		};

		// Identifies the active content-browser sort key.
		enum class ESortColumn : uint8
		{
			Name,
			Type,
			Size,
			Modified
		};

		auto Refresh(bool bRescanRegistry) -> void;
		auto RefreshItemsSnapshot() -> void;
		auto RebuildItems() -> void;
		auto NavigateToPhysical(std::string_view PhysicalPath, bool bAddHistory = true) -> bool;
		auto NavigateHistory(int32 Delta) -> void;
		auto RefreshMountSnapshot() -> void;
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
		auto DrawCreateMenu(std::string_view PhysicalDirectory, std::string_view VirtualDirectory) -> void;
		auto DrawImportMenu(std::string_view VirtualDirectory) -> void;
		auto DrawDirectoryContextMenu(std::string_view PhysicalDirectory, bool bMountRoot) -> void;
		auto DrawBackgroundContextMenu() -> void;
		auto DrawStatusBar() -> void;
		auto DrawSelectionDetails() -> void;
		auto DrawDialogs() -> void;
		auto BeginAssetDragDrop(const FContentBrowserItem& Item) -> void;
		auto AcceptAssetDrop(std::string_view DestinationDirectory, bool bPhysicalDirectory = false) -> void;

		auto SelectItem(size_t Index) -> void;
		auto OpenItem(const FContentBrowserItem& Item) -> void;
		auto BeginRename(const FContentBrowserItem& Item) -> void;
		auto DrawRenameEditor(const FContentBrowserItem& Item) -> void;
		auto CommitRename(const FContentBrowserItem& Item) -> bool;
		auto RenameFolder(const FContentBrowserItem& Item, std::string_view NewName) -> bool;
		auto IsManagedCompanion(const FContentBrowserItem& Item) const -> bool;
		auto CreateFolder(std::string_view PhysicalDirectory) -> void;
		auto CreateLevelAsset(std::string_view VirtualDirectory) -> void;
		auto CreateMaterialAsset(std::string_view VirtualDirectory, bool bInstance) -> void;
		auto FocusFolderInParent(std::string_view PhysicalDirectory) -> const FContentBrowserItem*;
		auto RequestDeleteSelection() -> void;
		auto AnalyzeDeleteSelection() -> void;
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

		FLevelEditorSessionSettings& SessionSettings;
		FOpenAsset OpenAsset;
		FRequestImport RequestImport;
		FMoveAssets MoveAssets;
		std::string CurrentPhysicalPath;
		std::string CurrentVirtualPath;
		std::vector<FMountSnapshot> MountSnapshot;
		std::unordered_map<std::string, std::vector<std::filesystem::path>> DirectoryChildrenCache;
		std::vector<FContentBrowserItem> ItemsSnapshot;
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
		bool bBackgroundContextPending = false;
		float IconSize;
		float DirectoryTreeWidth;
		std::string RenameTarget;
		std::array<char, 256> RenameBuffer{};
		bool bFocusRename = false;
		bool bRenameEditorHovered = false;
		bool bDeletePopupRequested = false;
		std::vector<std::pair<std::string, Asset::FAssetDeleteAnalysis>> DeleteAnalysis;
		std::vector<std::pair<std::string, Asset::FAssetResult>> DeleteAnalysisErrors;
		std::function<void()> DeferredContentAction;
		std::string ErrorMessage;
		std::unique_ptr<FContentBrowserThumbnailCache> ThumbnailCache;
	};
} // namespace Durin
