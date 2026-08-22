#pragma once

#include "Panels/LevelEditorPanel.h"
#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserOperations.h"
#include "Panels/ContentBrowserItemView.h"
#include "Panels/ContentBrowserRefreshCoordinator.h"
#include "Threading/Task.h"

#include <array>
#include <unordered_set>

namespace Durin::Asset { enum class EImportRecordAction : uint8; }

namespace Durin::Editor::Level
{
	class FLevelEditorSessionSettings;
	class FContentBrowserThumbnailCache;
	struct FLevelEditorContext;

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
		TerrainHeightmap,
		StaticMesh,
		Scene
	};

	// Coordinates content-browser model, operation, thumbnail, and immediate UI state.
	class FContentBrowserPanel final : public ILevelEditorPanel
	{
	public:
		using FOpenAsset = std::function<bool(const std::string&, const std::string&)>;
		using FRequestImport = std::function<void(const std::string&, EContentBrowserImportType)>;
		using FMoveAssets = std::function<Asset::FAssetResult(std::span<const FEditorAssetMove>)>;
		using FExecuteTransaction =
			std::function<bool(std::unique_ptr<::Durin::Editor::ITransaction>)>;
		using FGetMountedContentMutationRevision = std::function<uint64()>;
		using FNotifyMountedContentMutation = std::function<void()>;

		FContentBrowserPanel(
			FLevelEditorSessionSettings& InSessionSettings,
			FOpenAsset InOpenAsset,
			FRequestImport InRequestImport,
			FMoveAssets InMoveAssets,
			FExecuteTransaction InExecuteTransaction,
			FGetMountedContentMutationRevision InGetMountedContentMutationRevision,
			FNotifyMountedContentMutation InNotifyMountedContentMutation,
			std::shared_ptr<FMountedContentReconciliationState>
				InMountedContentReconciliationState,
			FTaskScopeToken InThumbnailTaskScope);
		~FContentBrowserPanel() override;

		auto GetWindowName() const -> const char* override { return "Content Browser"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto DrawEmbedded(FLevelEditorContext& Context) -> void;
		auto RequestSearchFocus() -> void { bFocusSearch = true; }
		auto RevealAsset(std::string_view AssetPath) -> void;
		auto RevealDirectory(std::string_view DirectoryPath) -> void;
		auto NotifyMountedContentChanged() -> void;

	private:
		auto PrepareForDraw() -> void;
		auto DrawContents() -> void;
		auto Refresh(bool bRescanRegistry) -> void;
		auto RefreshPublishedContent() -> void;
		auto PublishMountedContentMutation() -> void;
		auto RefreshItemsSnapshot() -> void;
		auto RebuildItems() -> void;
		auto NavigateToPhysical(std::string_view PhysicalPath, bool bAddHistory = true) -> bool;
		auto NavigateHistory(int32 Delta) -> void;
		auto RefreshMountSnapshot() -> void;
		auto PhysicalToVirtualDirectory(std::string_view PhysicalPath) const -> std::string;
		auto QueueTreeAction(std::function<void()> Action) -> void;
		auto QueueContentAction(std::function<void()> Action) -> void;
		auto ExecuteTreeAction() -> void;
		auto ExecuteContentAction() -> void;

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
		auto PrepareSelectionDetails() -> void;
		auto DrawSelectionDetails() -> void;
		auto DrawDialogs() -> void;
		auto BeginAssetDragDrop(const FContentBrowserItem& Item) -> void;
		auto AcceptAssetDrop(std::string_view DestinationDirectory, bool bPhysicalDirectory = false) -> void;

		auto SelectItem(size_t Index) -> void;
		auto OpenItem(const FContentBrowserItem& Item) -> void;
		auto BeginRename(const FContentBrowserItem& Item) -> void;
		auto DrawRenameEditor(const FContentBrowserItem& Item) -> void;
		auto CommitRename(const FContentBrowserItem& Item) -> bool;
		auto CreateFolder(std::string_view PhysicalDirectory) -> void;
		auto CreateLevelAsset(std::string_view VirtualDirectory) -> void;
		auto CreateMaterialAsset(std::string_view VirtualDirectory, bool bInstance) -> void;
		auto ReimportAsset(
			const FContentBrowserItem& Item,
			Asset::EImportRecordAction Action) -> void;
		auto SaveAssetPackage(const FAssetPath& Path) -> void;
		auto ResaveAssetPackages(std::vector<FAssetPath> Paths) -> void;
		auto FixUpRedirector(const FContentBrowserItem& Item) -> void;
		auto FixUpFolder(std::string_view VirtualDirectory) -> void;
		auto FixUpProject() -> void;
		auto FocusFolderInParent(std::string_view PhysicalDirectory) -> const FContentBrowserItem*;
		auto RequestDeleteSelection() -> void;
		auto DeleteSelection() -> void;
		auto SynchronizeMountedContentMutation() -> void;
		auto ShowInExplorer(std::string_view PhysicalPath) const -> void;
		auto CopyToClipboard(std::string_view Text) const -> void;

		auto SetError(std::string Message) -> void;
		auto SetWarning(std::string Message) -> void;
		auto RepairSelection() -> void;

		FLevelEditorSessionSettings& SessionSettings;
		FOpenAsset OpenAsset;
		FRequestImport RequestImport;
		FExecuteTransaction ExecuteTransaction;
		FGetMountedContentMutationRevision GetMountedContentMutationRevision;
		FNotifyMountedContentMutation NotifyMountedContentMutation;
		FContentBrowserRefreshCoordinator RefreshCoordinator;
		FContentBrowserModel Model;
		FContentBrowserOperations Operations;
		std::unordered_set<std::string> Selection;
		std::string SelectionAnchor;
		std::array<char, 256> SearchBuffer{};
		bool bFocusSearch = false;
		EContentBrowserViewMode ViewMode = EContentBrowserViewMode::Grid;
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
		bool bDeletionPlanRefreshed = false;
		bool bRefreshItemsOnFirstDraw = true;
		FContentDeletionPlanPtr PendingDeletionPlan;
		std::function<void()> DeferredTreeAction;
		std::function<void()> DeferredContentAction;
		std::string ErrorMessage;
		std::string WarningMessage;
		std::vector<FAssetPath> LastReimportOrphans;
		std::unique_ptr<FContentBrowserThumbnailCache> ThumbnailCache;
		ContentBrowserItemView::FTextureCubeDetailsCache TextureCubeDetailsCache;
		const ContentBrowserItemView::FTextureCubeDetailsSnapshot*
			TextureCubeDetailsSnapshot = nullptr;
	};
} // namespace Durin::Editor::Level
