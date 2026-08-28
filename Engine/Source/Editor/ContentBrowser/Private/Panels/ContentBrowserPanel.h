#pragma once

#include "ContentBrowser/ContentBrowserTool.h"
#include "Panels/ContentBrowserModel.h"
#include "Panels/ContentBrowserOperations.h"
#include "Panels/ContentBrowserItemView.h"
#include "Panels/ContentBrowserRefreshCoordinator.h"
#include "Threading/Task.h"

#include <array>
#include <unordered_set>

namespace Durin::Editor::ContentBrowser::Private
{
	class FContentBrowserThumbnailCache;

	// Selects grid or list presentation for content-browser items.
	enum class EContentBrowserViewMode : uint8
	{
		Grid,
		Details
	};

	// Coordinates content-browser model, operation, thumbnail, and immediate UI state.
	class FContentBrowserPanel final
		: public ::Durin::Editor::ContentBrowser::IContentBrowserTool
	{
	public:
		using FOpenAsset = std::function<bool(const std::string&, const std::string&)>;
		using FMoveAssets = std::function<Asset::FAssetResult(std::span<const FEditorAssetMove>)>;
		using FExecuteTransaction =
			std::function<bool(std::unique_ptr<::Durin::Editor::ITransaction>)>;
		using FGetMountedContentMutationRevision = std::function<uint64()>;
		using FNotifyMountedContentMutation = std::function<void()>;
		using FOpenImport = std::function<void(
			::Durin::Editor::EBuiltinImportFamily, std::string)>;
		using FQueryReimport = std::function<
			::Durin::Editor::ContentBrowser::FReimportAvailability(std::string_view)>;
		using FReimport = std::function<void(
			bool, std::string, std::function<void(std::string)>)>;
		using FDrawImportDialogs = std::function<void(bool)>;

		FContentBrowserPanel(
			::Durin::Editor::ContentBrowser::FPresentationSettings InSettings,
			::Durin::Editor::ContentBrowser::FSavePresentationSettings InSaveSettings,
			FOpenAsset InOpenAsset,
			FMoveAssets InMoveAssets,
			FExecuteTransaction InExecuteTransaction,
			FGetMountedContentMutationRevision InGetMountedContentMutationRevision,
			FNotifyMountedContentMutation InNotifyMountedContentMutation,
			FOpenImport InOpenImport,
			FQueryReimport InQueryReimport,
			FReimport InReimport,
			FDrawImportDialogs InDrawImportDialogs,
			std::shared_ptr<FMountedContentReconciliationState>
				InMountedContentReconciliationState,
			FTaskScopeToken InThumbnailTaskScope);
		~FContentBrowserPanel() override;

		auto TickWhenHidden() -> void override;
		auto DrawContents(bool bAllowAssetMutation) -> void override;
		auto DrawHostPresenters(bool bAllowAssetMutation) -> void override;
		auto RevealAsset(std::string_view AssetPath) -> bool override;
		auto RevealDirectory(std::string_view DirectoryPath) -> bool override;
		auto RequestFocus() -> bool override;
		auto NotifyMountedContentChanged() -> bool override;
		auto StopRequestAdmission() -> void override;
		auto GetAdmissionState() const
			-> ::Durin::Editor::ContentBrowser::EAdmissionState override
		{
			return AdmissionState;
		}

	private:
		auto PrepareForDraw() -> void;
		auto DrawBrowserContents() -> void;
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
		auto DuplicateAsset(const FContentBrowserItem& Item) -> void;
		auto CopyAssetSelection() -> void;
		auto PasteAsset(std::string_view DestinationDirectory = {}) -> void;
		auto HasAssetClipboard() const -> bool;
		auto CreateFolder(std::string_view PhysicalDirectory) -> void;
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

		::Durin::Editor::ContentBrowser::FPresentationSettings PresentationSettings;
		::Durin::Editor::ContentBrowser::FSavePresentationSettings SavePresentationSettings;
		FOpenAsset OpenAsset;
		FExecuteTransaction ExecuteTransaction;
		FGetMountedContentMutationRevision GetMountedContentMutationRevision;
		FNotifyMountedContentMutation NotifyMountedContentMutation;
		FOpenImport OpenImport;
		FQueryReimport QueryReimport;
		FReimport Reimport;
		FDrawImportDialogs DrawImportDialogs;
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
		bool bResetContentScroll = false;
		FContentDeletionPlanPtr PendingDeletionPlan;
		std::function<void()> DeferredTreeAction;
		std::function<void()> DeferredContentAction;
		std::string ErrorMessage;
		std::string WarningMessage;
		std::unique_ptr<FContentBrowserThumbnailCache> ThumbnailCache;
		ContentBrowserItemView::FTextureCubeDetailsCache TextureCubeDetailsCache;
		const ContentBrowserItemView::FTextureCubeDetailsSnapshot*
			TextureCubeDetailsSnapshot = nullptr;
		::Durin::Editor::ContentBrowser::EAdmissionState AdmissionState =
			::Durin::Editor::ContentBrowser::EAdmissionState::Accepting;
		bool bAllowAssetMutation = true;
	};
} // namespace Durin::Editor::ContentBrowser::Private
