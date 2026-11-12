#pragma once

#include "CoreMinimal.h"

#include "Panels/ContentBrowserSelection.h"

#include "ContentBrowser/ContentBrowserTool.h"
#include "Panels/ContentBrowserModel.h"
#include "ContentBrowserDataSubsystem.h"
#include "Panels/ContentBrowserItemView.h"
#include "Panels/ContentBrowserRefreshCoordinator.h"
#include "Threading/TaskRuntime.h"
#include "Settings/EditorSettings.h"

namespace Durin::ContentBrowser::Private
{
	class FContentBrowserThumbnailReferences;

	// Selects grid or list presentation for content-browser items.
	enum class EContentBrowserViewMode : uint8
	{
		Grid,
		Details
	};

	// Coordinates content-browser model, operation, thumbnail, and immediate UI state.
	class FContentBrowserPanel final
		: public IContentBrowserTool
	{
	public:
		FContentBrowserPanel(FPresentationSettings InSettings,
			FSavePresentationSettings InSaveSettings, FContentBrowserHostContext InHost);
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
			-> EAdmissionState override
		{
			return AdmissionState;
		}

	private:
		friend struct FContentBrowserPanelTestAccess;
		struct FDragSnapshot
		{
			uint64 Serial = 0;
			std::vector<FContentBrowserItem> Items;
		};
		std::shared_ptr<const FDragSnapshot> DragSnapshot;
		auto CanMutateContent() const -> bool;
		auto CanDeleteSelection() const -> bool;
		auto ResolveOperationItems(std::span<const FContentBrowserItem>) const -> std::vector<FContentBrowserItem>;

		auto MakeRequestHost(FContentBrowserHostContext Host) -> FContentBrowserHostContext;
		auto GetDataSubsystem() const -> DContentBrowserDataSubsystem*;
		auto FindItemAtPath(std::string_view VirtualPath) const -> FContentBrowserItem;
		static auto CollectItemData(std::span<const FContentBrowserItem>) -> std::vector<FContentBrowserItemData>;
		auto MoveItems(std::vector<FContentBrowserItem>, std::string Destination) -> bool;
		auto GetDeletionPreview() const -> std::shared_ptr<const FContentBrowserDeletePreview>;
		auto PrepareForDraw() -> void;
		auto DrawBrowserContents() -> void;
		auto Refresh(bool bRescanRegistry) -> void;
		auto RefreshPublishedContent() -> void;
		auto ApplyContentChanges(const FContentChangeBatch& Changes) -> void;
		auto ApplyItemDataUpdates(std::span<const FContentBrowserItemDataUpdate>) -> void;
		FDelegateHandle ItemDataUpdatedHandle;
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
		auto MakeExtensionContext(const FContentBrowserItem* PrimaryItem = nullptr,
			std::string_view TargetPhysicalDirectory = {}, std::string_view TargetVirtualDirectory = {}) const
			-> FExtensionContext;
		auto DrawExtensionMenu(EExtensionCategory Category, const FExtensionContext& Context) -> void;
		auto DrawCreateMenu(std::string_view PhysicalDirectory, std::string_view VirtualDirectory) -> void;
		auto DrawImportMenu(std::string_view PhysicalDirectory, std::string_view VirtualDirectory) -> void;
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
		auto CancelRename() -> void;
		auto CommitRename(const FContentBrowserItem& Item) -> bool;
		auto SyncToItem(const FContentBrowserItem& Item) -> bool;
		auto DuplicateItem(const FContentBrowserItem& Item) -> void;
		auto CopyContentSelection() -> void;
		auto PasteContent(std::string_view DestinationDirectory = {}) -> void;
		auto HasContentClipboard() const -> bool;
		auto CreateFolder(std::string_view PhysicalDirectory) -> void;
		auto SaveItem(const FContentBrowserItem& Item) -> void;
		auto ResaveItems(std::vector<FContentBrowserItem> Items) -> void;
		auto FixUpRedirector(const FContentBrowserItem& Item) -> void;
		auto FixUpFolder(std::string_view VirtualDirectory) -> void;
		auto FixUpProject() -> void;
		auto FocusFolderInParent(std::string_view PhysicalDirectory, bool bDelete) -> void;
		auto CompletePendingItemAction() -> void;
		// Deferred mutations are tied to navigation, not a replaceable capture request.
		struct FPendingItemAction
		{
			FContentBrowserItemKey ItemKey;
			uint64 NavigationRevision = 0;
			bool bDelete = false;
		};
		std::optional<FPendingItemAction> PendingItemAction;
		auto RequestDeleteSelection() -> void;
		auto DeleteSelection() -> void;
		auto SynchronizeMountedContentMutation() -> void;
		auto ShowInExplorer(std::string_view PhysicalPath) const -> void;
		auto CopyToClipboard(std::string_view Text) const -> void;

		auto SetError(std::string Message) -> void;
		auto SetWarning(std::string Message) -> void;
		auto RepairSelection() -> void;

		auto DrawPreferences(std::string& OutError) -> void;
		auto ResetPreferences() -> bool;
		auto ApplyPreferences(const FPresentationSettings& Next) -> bool;
		FEditorSettingsPageRegistration SettingsPage;
		FPresentationSettings PresentationSettings;
		FSavePresentationSettings SavePresentationSettings;
		std::shared_ptr<void> ExtensionLifetime = std::make_shared<uint8>(0);
		FContentBrowserHostContext HostContext;
		FContentBrowserRefreshCoordinator RefreshCoordinator;
		FContentBrowserDataQuery DataQuery;
		FContentBrowserModel Model;
		FContentBrowserSelection SelectionState;
		std::array<char, 256> SearchBuffer{};
		bool bFocusSearch = false;
		EContentBrowserViewMode ViewMode = EContentBrowserViewMode::Grid;
		bool bShowSelectionDetails = false;
		bool bIconSizeLocked = false;
		bool bContentItemHovered = false;
		bool bBackgroundContextPending = false;
		float IconSize;
		float DirectoryTreeWidth;
		struct FPendingFolder
		{
			FContentBrowserItemDataTemporaryContext Context;
			uint64 NavigationRevision;
			std::string ValidationError;
		};
		std::optional<FPendingFolder> PendingFolder;
		FContentBrowserItemKey RenameTarget;
		std::array<char, 256> RenameBuffer{};
		bool bFocusRename = false;
		bool bRenameEditorHovered = false;
		bool bDeletePopupRequested = false;
		bool bDeletionPlanRefreshed = false;
		bool bRefreshItemsOnFirstDraw = true;
		bool bResetContentScroll = false;
		FContentBrowserDeleteContext PendingDeletion;
		std::vector<FContentBrowserItemData> PendingDeletionItems;
		std::shared_ptr<const FContentBrowserDeletePreview> DeletionFallbackPreview;
		std::function<void()> DeferredTreeAction;
		std::function<void()> DeferredContentAction;
		std::string ErrorMessage;
		std::string WarningMessage;
		std::unique_ptr<FContentBrowserThumbnailReferences> ThumbnailReferences;
		std::vector<FDetailRow> SelectionDetails;
		uint64 PresentationRevision = 0;
		EAdmissionState AdmissionState =
			EAdmissionState::Accepting;
		bool bAllowAssetMutation = true;
	};
} // namespace Durin::ContentBrowser::Private
