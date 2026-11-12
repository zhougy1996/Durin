#include "Data/ContentBrowserItemAttributeHelpers.h"
#include "Panels/ContentBrowserPanel.h"
#include "ContentBrowserNativeDataSource.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "AssetRegistry/Catalog.h"
#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Panels/ContentBrowserItemView.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif

namespace Durin::ContentBrowser::Private
{
	namespace
	{
		// Shared by browser panels. The system clipboard token invalidates this
		// snapshot when another application or Copy Details replaces the clipboard.
		std::vector<FContentBrowserItem> CopiedContent;
		std::string CopiedContentToken;
		uint64 CopySerial = 0;
		auto HasCopiedContent() -> bool
		{
			const char* Text = ImGui::GetClipboardText();
			return Text && !CopiedContent.empty() && CopiedContentToken == Text;
		}

		auto ReadAssetClipboard(FTopLevelAssetPath& OutPath) -> bool
		{
			OutPath = {};
			const char* Clipboard = ImGui::GetClipboardText();
			if (!Clipboard) return false;
			return FTopLevelAssetPath::TryCreate(Clipboard, OutPath);
		}
	}

	auto FContentBrowserPanel::CanDeleteSelection() const -> bool
	{
		size_t Count = 0;
		for (const auto& Item : Model.GetItems())
			if (SelectionState.Selected.contains(Item.GetItemKey()))
			{
				if (!Item.CanDelete()) return false;
				++Count;
			}
		return Count > 0 && Count == SelectionState.Selected.size();
	}

	auto FContentBrowserPanel::CanMutateContent() const -> bool
	{
		return GetDataSubsystem() && bAllowAssetMutation && (!HostContext.CanMutateContent || HostContext.CanMutateContent());
	}
	auto FContentBrowserPanel::ResolveOperationItems(std::span<const FContentBrowserItem> Items) const -> std::vector<FContentBrowserItem>
	{
		std::vector<FContentBrowserItem> Result;
		for (const auto& Item : Items)
		{
			if (Item.InternalItems.empty()) return {};
			FContentBrowserItem Fresh;
			if (auto* Service = GetDataSubsystem()) Service->EnumerateItemsAtPath(Item.GetVirtualPath(), [&](FContentBrowserItem&& Candidate) {
				if (Candidate.GetItemKey() != Item.GetItemKey()) return true;
				Fresh = std::move(Candidate); return false;
			}, HostContext);
			if (Fresh.InternalItems.size() != Item.InternalItems.size()) return {};
			for (const auto& Data : Item.InternalItems)
				if (!Data.GetOwnerDataSource() || std::ranges::none_of(Fresh.InternalItems, [&](const auto& Current) {
					return Current.GetOwnerDataSource() && Current.GetContributionKey() == Data.GetContributionKey();
				})) return {};
			Result.push_back(std::move(Fresh));
		}
		return Result;
	}

	auto FContentBrowserPanel::MakeRequestHost(FContentBrowserHostContext InHost) -> FContentBrowserHostContext
	{
		const std::weak_ptr<void> Lifetime = ExtensionLifetime;
		InHost.CanMutateContent = [this, Lifetime, Allow = InHost.CanMutateContent] {
			return !Lifetime.expired() && AdmissionState == EAdmissionState::Accepting && bAllowAssetMutation && (!Allow || Allow());
		};
		InHost.ReportDiagnostic = [this, Lifetime](ELogLevel Level, std::string_view Message) {
			DURIN_LOG(Level, "Content Browser: {}", Message);
			if (Lifetime.expired()) return;
			if (Level >= ELogLevel::Error)
				SetError("Content Browser operation failed. See Console for details.");
			else if (Level == ELogLevel::Warn)
				SetWarning("Content Browser operation reported warnings. See Console for details.");
		};
		if (InHost.OpenAsset) InHost.OpenAsset = [Lifetime, Open = InHost.OpenAsset](const std::string& Path, const std::string& Class) { return !Lifetime.expired() && Open(Path, Class); };
		return InHost;
	}
	auto FContentBrowserPanel::GetDataSubsystem() const -> DContentBrowserDataSubsystem*
	{
		auto* Data = HostContext.DataSubsystem.Get();
		return Data && Data->IsActive() && AdmissionState == EAdmissionState::Accepting ? Data : nullptr;
	}
	auto FContentBrowserPanel::CollectItemData(std::span<const FContentBrowserItem> Items) -> std::vector<FContentBrowserItemData>
	{
		std::vector<FContentBrowserItemData> Result;
		for (const auto& Item : Items)
			Result.insert(Result.end(), Item.InternalItems.begin(), Item.InternalItems.end());
		return Result;
	}
	auto FContentBrowserPanel::FindItemAtPath(std::string_view Path) const -> FContentBrowserItem
	{
		FContentBrowserItem Result;
		if (auto* Data = GetDataSubsystem()) Data->EnumerateItemsAtPath(Path, [&](FContentBrowserItem&& Item) {Result=std::move(Item);return false; }, HostContext);
		return Result;
	}
	auto FContentBrowserPanel::MoveItems(std::vector<FContentBrowserItem> Items, std::string Destination) -> bool
	{
		if (!CanMutateContent()) return false;
		auto Current = ResolveOperationItems(Items);
		auto* Data = GetDataSubsystem();
		return Data && CanMutateContent() && Current.size() == Items.size() && Data->BulkMoveItems(CollectItemData(Current), Destination);
	}
	auto FContentBrowserPanel::GetDeletionPreview() const -> std::shared_ptr<const FContentBrowserDeletePreview>
	{
		if (!PendingDeletion.Request) return {};
		auto Preview = PendingDeletion.Request->GetPreview();
		return Preview ? Preview : DeletionFallbackPreview;
	}

	auto FContentBrowserPanel::SaveItem(const FContentBrowserItem& Item) -> void
	{
		if (!CanMutateContent()) return;
		Item.Save(EContentBrowserItemSaveFlags::SaveOnlyIfDirty | EContentBrowserItemSaveFlags::SaveOnlyIfLoaded);
	}

	auto FContentBrowserPanel::ResaveItems(std::vector<FContentBrowserItem> Items) -> void
	{
		if (!CanMutateContent()) return;
		if (auto* Data = GetDataSubsystem()) Data->BulkSaveItems(CollectItemData(Items), EContentBrowserItemSaveFlags::None);
	}

	FContentBrowserPanel::FContentBrowserPanel(FPresentationSettings InSettings, FSavePresentationSettings InSaveSettings, FContentBrowserHostContext InHost)
		: PresentationSettings(std::move(InSettings))
		, SavePresentationSettings(std::move(InSaveSettings))
		, HostContext(MakeRequestHost(std::move(InHost)))
		, RefreshCoordinator(
			  HostContext.GetMountedContentMutationRevision ? HostContext.GetMountedContentMutationRevision() : uint64{0},
			  GetAssetQueryRevision(),
			  HostContext.ReconciliationState
		  )
		, DataQuery(HostContext)
		, Model(DataQuery, true)
		, IconSize(PresentationSettings.IconSize)
		, DirectoryTreeWidth(PresentationSettings.TreeWidth)
	{
		if (auto* Data = GetDataSubsystem())
		{
			const std::weak_ptr<void> Lifetime = ExtensionLifetime;
			ItemDataUpdatedHandle = Data->OnItemDataUpdated().AddLambda([this, Lifetime](std::span<const FContentBrowserItemDataUpdate> Updates) {
				if (!Lifetime.expired() && GetDataSubsystem()) ApplyItemDataUpdates(Updates);
			});
		}

		RefreshCoordinator.SetChangeSources(HostContext.CaptureMountedContentChanges, CaptureAssetQueryChanges);
		RefreshCoordinator.SetPublicationSuspension([this](bool bSuspended) { Model.SetSnapshotPublicationSuspended(bSuspended); });
		Model.RefreshMountSnapshot();
		ThumbnailReferences = std::make_unique<FContentBrowserThumbnailReferences>();
		ViewMode = static_cast<EContentBrowserViewMode>(
			PresentationSettings.ViewMode);
		bIconSizeLocked = PresentationSettings.bIconSizeLocked;
		Model.SetShowHiddenFiles(
			PresentationSettings.bShowHiddenFiles);
		if (!PresentationSettings.LastDirectory.empty())
			NavigateToPhysical(
				PresentationSettings.LastDirectory);
		if (Model.GetCurrentVirtualPath().empty())
		{
			for (const FContentBrowserModel::FMountSnapshot& Mount :
				 Model.GetMounts())
				if (NavigateToPhysical(Mount.PhysicalRoot))
					break;
		}
		SettingsPage = FEditorSettingsPageRegistration({
			.Id = "editor.content-browser", .Label = "Content Browser",
			.Keywords = "grid details view thumbnail icon size lock hidden files folders",
			.Description = "Browser presentation preferences. Changes take effect immediately and stay in sync with the browser toolbar.",
			.Draw = [this](std::string& Error) { DrawPreferences(Error); },
			.Reset = [this] { return ResetPreferences(); },
		});
	}

	FContentBrowserPanel::~FContentBrowserPanel()
	{
		StopRequestAdmission();
		SettingsPage = {};
	}

	auto FContentBrowserPanel::NotifyMountedContentChanged() -> bool
	{
		if (AdmissionState != EAdmissionState::Accepting)
			return false;
		PublishMountedContentMutation();
		return true;
	}

	auto FContentBrowserPanel::RevealDirectory(
		std::string_view DirectoryPath) -> bool
	{
		if (AdmissionState != EAdmissionState::Accepting)
			return false;
		const auto Resolved =
			FMountPaths::ResolveAssetPath(DirectoryPath);
		if (!Resolved)
		{
			SetError((Resolved ? std::string{} : ToString(Resolved.error())));
			return false;
		}
		if (!NavigateToPhysical(Resolved->PhysicalPath.generic_string()))
		{
			SetError(
				"The requested directory is not part of an automatically scanned Content Browser mount.");
			return false;
		}
		return true;
	}

	auto FContentBrowserPanel::RefreshMountSnapshot() -> void
	{
		Model.RefreshMountSnapshot();
	}

	auto FContentBrowserPanel::PhysicalToVirtualDirectory(
		std::string_view PhysicalPath) const -> std::string
	{
		return Model.PhysicalToVirtualDirectory(PhysicalPath);
	}

	auto FContentBrowserPanel::NavigateToPhysical(
		std::string_view PhysicalPath,
		bool bAddHistory) -> bool
	{
		if (HostContext.GetMountedContentMutationRevision)
			RefreshCoordinator.ObserveMountedContent(HostContext.GetMountedContentMutationRevision());
		const auto PreviousNavigation = Model.GetNavigationRevision();
		if (!Model.NavigateToPhysical(PhysicalPath, bAddHistory) && !Model.NavigateToVirtual(PhysicalPath, bAddHistory)) return false;
		if (Model.GetNavigationRevision() == PreviousNavigation) return true;
		CancelRename();
		ThumbnailReferences->CancelPendingRequests();
		SelectionState.Selected.clear();
		SelectionState.Anchor = {};
		bResetContentScroll = true;
		RepairSelection();
		return true;
	}

	auto FContentBrowserPanel::NavigateHistory(int32 Delta) -> void
	{
		if (HostContext.GetMountedContentMutationRevision)
			RefreshCoordinator.ObserveMountedContent(HostContext.GetMountedContentMutationRevision());
		const auto PreviousNavigation = Model.GetNavigationRevision();
		if (Model.NavigateHistory(Delta))
		{
			if (Model.GetNavigationRevision() == PreviousNavigation) return;
			CancelRename();
			ThumbnailReferences->CancelPendingRequests();
			SelectionState.Selected.clear();
			SelectionState.Anchor = {};
			bResetContentScroll = true;
			RepairSelection();
		}
	}

	auto FContentBrowserPanel::Refresh(bool bRescanRegistry) -> void
	{
		if (HostContext.GetMountedContentMutationRevision)
			RefreshCoordinator.ObserveMountedContent(HostContext.GetMountedContentMutationRevision());
		if (bRescanRegistry)
		{
			const uint64 MountedContentRevision = HostContext.GetMountedContentMutationRevision
				? HostContext.GetMountedContentMutationRevision()
				: RefreshCoordinator.GetObservedMountedContentRevision();
			const auto Result =
				RefreshCoordinator.ReconcileExplicitly(
					MountedContentRevision,
					[this] { return Model.RescanRegistry(); },
					[this](const FContentChangeBatch&) { RefreshPublishedContent(); },
					[] { return GetAssetQueryRevision(); });
			if (!Result) SetError(Result.Message);
			return;
		}
		RefreshCoordinator.RefreshRegistryView(
			GetAssetQueryRevision(),
			[this](const FContentChangeBatch&) { RefreshPublishedContent(); });
	}

	auto FContentBrowserPanel::RefreshPublishedContent() -> void
	{
		if (Model.GetCurrentPhysicalPath().empty() && !Model.GetCurrentVirtualPath().empty())
		{
			Model.RefreshMountSnapshot();
			RefreshItemsSnapshot();
			return;
		}
		ThumbnailReferences->ApplyContentChanges({.bFullRefresh = true});
		ThumbnailReferences->CancelPendingRequests();
		const std::string AvailableDirectory =
			Model.FindNearestAvailableDirectory(Model.GetCurrentPhysicalPath());
		Model.RefreshMountSnapshot();
		if (!AvailableDirectory.empty()
			&& AvailableDirectory != Model.GetCurrentPhysicalPath()
			&& NavigateToPhysical(AvailableDirectory))
			return;
		if (Model.GetCurrentPhysicalPath().empty()
			|| AvailableDirectory.empty()
			|| !Model.ResolveMountPath(Model.GetCurrentPhysicalPath()))
		{
			for (const FContentBrowserModel::FMountSnapshot& Mount :
				 Model.GetMounts())
				if (NavigateToPhysical(Mount.PhysicalRoot)) return;
		}
		RefreshItemsSnapshot();
	}

	auto FContentBrowserPanel::ApplyItemDataUpdates(std::span<const FContentBrowserItemDataUpdate> Updates) -> void
	{
		for (const auto& Update : Updates)
		{
			SelectionState.Apply(Update, Model.GetItemsSnapshot());
			FContentBrowserSelection::Migrate(RenameTarget, Update);
			if (PendingItemAction) FContentBrowserSelection::Migrate(PendingItemAction->ItemKey, Update);
		}
	}

	auto FContentBrowserPanel::ApplyContentChanges(const FContentChangeBatch& Changes) -> void
	{
		if (Changes.bFullRefresh) { RefreshPublishedContent(); return; }
		auto Mapped = Changes;
		for (auto& Change : Mapped.Changes)
			if (Change.bDirectory)
			{
				if (!Change.OldPhysicalPath.empty()) Change.OldAssetPath = Model.PhysicalToVirtualDirectory(Change.OldPhysicalPath);
				if (!Change.NewPhysicalPath.empty()) Change.NewAssetPath = Model.PhysicalToVirtualDirectory(Change.NewPhysicalPath);
			}
		ThumbnailReferences->ApplyContentChanges(Mapped);
		const auto Navigation = Model.GetNavigationRevision();
		if (Model.ApplyContentChanges(Mapped)) RepairSelection();
		if (PendingItemAction && PendingItemAction->NavigationRevision == Navigation)
		{
			if (!PendingItemAction->ItemKey.IsValid()) PendingItemAction.reset();
			else PendingItemAction->NavigationRevision = Model.GetNavigationRevision();
		}
	}

	auto FContentBrowserPanel::PublishMountedContentMutation() -> void
	{
		if (HostContext.NotifyMountedContentMutation) HostContext.NotifyMountedContentMutation();
		SynchronizeMountedContentMutation();
	}

	auto FContentBrowserPanel::RefreshItemsSnapshot() -> void
	{
		ThumbnailReferences->CancelPendingRequests();
		Model.RefreshItemsSnapshot();
		RepairSelection();
	}

	auto FContentBrowserPanel::RebuildItems() -> void
	{
		Model.SetSearch(SearchBuffer.data());
		RepairSelection();
	}

	auto FContentBrowserPanel::RepairSelection() -> void
	{
		if (!Model.IsLoading()) SelectionState.Repair(Model.GetItems());
	}

	auto FContentBrowserPanel::SelectItem(size_t Index) -> void
	{
		const ImGuiIO& IO = ImGui::GetIO();
		SelectionState.Select(Model.GetItems(), Index, IO.KeyShift, IO.KeyCtrl);
	}

	auto FContentBrowserPanel::OpenItem(const FContentBrowserItem& Item) -> void
	{
		if (!GetDataSubsystem()) return;
		if (Item.IsFolder())
		{
			NavigateToPhysical(Item.GetVirtualPath());
			return;
		}
		if (!Item.Edit()) SetError("The item could not be opened by its data source.");
	}

	auto FContentBrowserPanel::BeginRename(const FContentBrowserItem& Item) -> void
	{
		std::string Reason;
		const auto Name = ItemAttributes::RenameName(Item);
		if (!Item.CanRename(Name, &Reason))
		{
			SetError(std::move(Reason));
			return;
		}
		CancelRename();
		RenameTarget = Item.GetItemKey();
		RenameBuffer.fill(0);
		const std::string_view InitialName = Name;
		std::memcpy(
			RenameBuffer.data(),
			InitialName.data(),
			std::min(InitialName.size(), RenameBuffer.size() - 1));
		bFocusRename = true;
	}

	auto FContentBrowserPanel::CancelRename() -> void
	{
		PendingFolder.reset();
		RenameTarget = {};
		bFocusRename = false;
	}

	auto FContentBrowserPanel::CommitRename(const FContentBrowserItem& Item)
		-> bool
	{
		if (!CanMutateContent()) return false;
		const std::string NewName = RenameBuffer.data();
		if (PendingFolder)
		{
			if (PendingFolder->NavigationRevision != Model.GetNavigationRevision())
			{
				CancelRename();
				return false;
			}
			if (!PendingFolder->Context.ValidateItem(NewName, &PendingFolder->ValidationError))
			{
				bFocusRename = true;
				return false;
			}
			FContentBrowserItem Created;
			if (!PendingFolder->Context.FinalizeItem(NewName, Created))
			{
				bFocusRename = true;
				return false;
			}
			CancelRename();
			SearchBuffer.fill(0);
			if (!SyncToItem(Created)) SetError("The folder was created, but it could not be shown in the content browser.");
			return true;
		}
		const auto CurrentName = ItemAttributes::RenameName(Item);
		if (NewName == CurrentName)
		{
			RenameTarget = {};
			return true;
		}

		FContentBrowserItem NewItem;
		const bool Result = Item.Rename(NewName, NewItem);
		if (!Result)
		{
			return false;
		}
		RenameTarget = {};
		if (!SyncToItem(NewItem)) SetError("The item was renamed, but it could not be shown in the content browser.");
		return true;
	}

	auto FContentBrowserPanel::SyncToItem(const FContentBrowserItem& Item) -> bool
	{
		if (Item.GetVirtualPath().empty()) return false;
		auto Path = Item.GetVirtualPath();
		while (Path.size() > 1 && Path.back() == '/')
			Path.pop_back();
		const auto Parent = std::filesystem::path(Path).parent_path().generic_string();
		if (!NavigateToPhysical(Parent)) return false;
		SearchBuffer.fill(0);
		Model.SetSearch("");
		Model.SetTypeFilter(EContentBrowserTypeFilter::All);
		Model.RefreshItemsSnapshot(false);
		SelectionState.Selected.clear();
		SelectionState.Selected.insert(Item.GetItemKey());
		return true;
	}

	auto FContentBrowserPanel::DuplicateItem(
		const FContentBrowserItem& Item
	) -> void
	{
		if (!CanMutateContent()) return;
		FContentBrowserItem NewItem;
		Item.Duplicate(NewItem);
		if (NewItem.GetItemKey().IsValid()) SyncToItem(NewItem);
	}

	auto FContentBrowserPanel::CopyContentSelection() -> void
	{
		std::vector<FContentBrowserItem> Items;
		for (const auto& Item : Model.GetItems())
			if (SelectionState.Selected.contains(Item.GetItemKey()))
			{
				if (Item.InternalItems.empty() || std::ranges::any_of(Item.InternalItems, [](const auto& Data) { return !Data.GetOwnerDataSource(); })) return;
				Items.push_back(Item);
			}
		if (Items.empty()) return;
		CopiedContent.clear();
		CopiedContentToken.clear();
		if (Items.size() == 1 && (Items.front().IsAsset() && !Items.front().IsRedirector()))
		{
			CopiedContentToken = Items.front().GetVirtualPath();
			CopiedContent = std::move(Items);
			CopyToClipboard(CopiedContentToken);
			return;
		}
		CopiedContent = std::move(Items);
		CopiedContentToken = "Durin Content Selection " + std::to_string(++CopySerial);
		CopyToClipboard(CopiedContentToken);
	}

	auto FContentBrowserPanel::PasteContent(
		std::string_view DestinationDirectory) -> void
	{
		if (!CanMutateContent()) return;
		FTopLevelAssetPath SourcePath;
		const bool bContent = HasCopiedContent();
		if (!bContent && !ReadAssetClipboard(SourcePath)) return;
		const std::string_view Directory = DestinationDirectory.empty()
			? std::string_view(Model.GetCurrentVirtualPath())
			: DestinationDirectory;
		FContentBrowserItem NewItem;
		if (bContent)
		{
			auto Current = ResolveOperationItems(CopiedContent);
			if (auto* Data = GetDataSubsystem(); Data && CanMutateContent() && Current.size() == CopiedContent.size() && Data->BulkCopyItems(CollectItemData(Current), Directory)) NavigateToPhysical(Directory);
		}
		else
		{
			const auto Source = FindItemAtPath(SourcePath.ToString());
			if (auto* Data = GetDataSubsystem(); Data && CanMutateContent()) Data->BulkCopyItems(Source.GetInternalItems(), Directory);
		}
		if (NewItem.GetItemKey().IsValid()) SyncToItem(NewItem);

	}

	auto FContentBrowserPanel::HasContentClipboard() const -> bool
	{
		if (!GetDataSubsystem()) return false;
		if (HasCopiedContent()) return std::ranges::all_of(CopiedContent, [&](const auto& Item) { return Item.CanCopy(Model.GetCurrentVirtualPath()); });
		FTopLevelAssetPath SourcePath;
		return ReadAssetClipboard(SourcePath) && FindItemAtPath(SourcePath.ToString()).CanCopy(Model.GetCurrentVirtualPath());
	}

	auto FContentBrowserPanel::CreateFolder(std::string_view PhysicalDirectory) -> void
	{
		if (!CanMutateContent()) return;
		const std::string Directory(PhysicalDirectory);
		auto* Data = GetDataSubsystem();
		if (!Data) return;
		const auto Virtual = Model.PhysicalToVirtualDirectory(Directory);
		FContentBrowserItemDataTemporaryContext Context;
		bool Prepared = false;
		for (int Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const auto Name = Suffix == 0 ? std::string("New Folder") : std::format("New Folder ({})", Suffix + 1);
			const auto Path = Virtual + (Virtual.ends_with('/') ? "" : "/") + Name;
			if (Data->CanCreateFolder(Path) && Data->CreateFolder(Path, Context, HostContext))
			{
				Prepared = true;
				break;
			}
		}
		if (!Prepared || !NavigateToPhysical(Directory)) return;
		CancelRename();
		PendingItemAction.reset();
		SelectionState.Selected.clear();
		PendingFolder = FPendingFolder{std::move(Context), Model.GetNavigationRevision()};
		const auto& Item = PendingFolder->Context.GetItem();
		RenameTarget = {};
		RenameBuffer.fill(0);
		std::memcpy(RenameBuffer.data(), Item.GetItemName().data(), std::min(Item.GetItemName().size(), RenameBuffer.size() - 1));
		bFocusRename = true;
		bResetContentScroll = true;
	}

	auto FContentBrowserPanel::FixUpRedirector(
		const FContentBrowserItem& Item) -> void
	{
		if (!CanMutateContent()) return;
		std::vector<FPackagePath> Redirectors;
		for (const FContentBrowserItem& Candidate : Model.GetItems())
		{
			if (!(Candidate.IsRedirector())
				|| !SelectionState.Selected.contains(Candidate.GetItemKey()))
				continue;
			if (!ItemAttributes::PackagePath(Candidate).IsValid())
			{
				SetError("A selected redirector path is invalid.");
				return;
			}
			Redirectors.push_back(ItemAttributes::PackagePath(Candidate));
		}
		if (Redirectors.empty())
		{
			if (!ItemAttributes::PackagePath(Item).IsValid())
			{
				SetError("The redirector path is invalid.");
				return;
			}
			Redirectors.push_back(ItemAttributes::PackagePath(Item));
		}
		if (auto* Data = GetDataSubsystem())
			if (auto* Source = Cast<DContentBrowserNativeDataSource>(Data->GetActiveDataSource(FName("Asset")))) Source->FixUpRedirectors(Redirectors, HostContext);
	}

	auto FContentBrowserPanel::FixUpFolder(
		std::string_view VirtualDirectory) -> void
	{
		if (!CanMutateContent()) return;
		if (auto* Data = GetDataSubsystem())
			if (auto* Source = Cast<DContentBrowserNativeDataSource>(Data->GetActiveDataSource(FName("Asset")))) Source->FixUpRedirectorsInFolder(VirtualDirectory, HostContext);
	}

	auto FContentBrowserPanel::FixUpProject() -> void
	{
		if (!CanMutateContent()) return;
		if (auto* Data = GetDataSubsystem())
			if (auto* Source = Cast<DContentBrowserNativeDataSource>(Data->GetActiveDataSource(FName("Asset")))) Source->FixUpAllRedirectors(HostContext);
	}

	auto FContentBrowserPanel::FocusFolderInParent(
		std::string_view PhysicalDirectory, bool bDelete) -> void
	{
		const auto Key = Model.RevealPhysicalItem(PhysicalDirectory);
		SearchBuffer.fill('\0');
		if (!Key.IsValid())
		{
			SetError("The folder could not be shown in its mounted parent directory.");
			return;
		}
		ThumbnailReferences->CancelPendingRequests();
		bResetContentScroll = true;
		PendingItemAction = FPendingItemAction{Key, Model.GetNavigationRevision(), bDelete};
		CompletePendingItemAction();
	}

	auto FContentBrowserPanel::CompletePendingItemAction() -> void
	{
		if (PendingFolder && PendingFolder->NavigationRevision != Model.GetNavigationRevision()) CancelRename();
		if (!PendingItemAction) return;
		if (PendingItemAction->NavigationRevision != Model.GetNavigationRevision())
		{
			PendingItemAction.reset();
			return;
		}
		if (Model.IsLoading()) return;
		auto Action = std::move(*PendingItemAction);
		PendingItemAction.reset();
		const auto Items = Model.GetItems();
		const auto It = std::ranges::find_if(Items, [&](const auto& Item) { return Item.GetItemKey() == Action.ItemKey; });
		if (It == Items.end())
		{
			SetError("The folder could not be found after refreshing its parent directory.");
			return;
		}
		SelectionState.Selected.clear();
		SelectionState.Selected.insert(It->GetItemKey());
		SelectionState.Anchor = It->GetItemKey();
		if (Action.bDelete) RequestDeleteSelection();
		else BeginRename(*It);
	}

	auto FContentBrowserPanel::RequestDeleteSelection() -> void
	{
		if (!CanMutateContent() || !CanDeleteSelection()) return;
		PendingDeletion = {};
		PendingDeletionItems = CollectItemData(ContentBrowserQuery::CopySelection(Model.GetItems(), SelectionState.Selected));
		auto* Data = GetDataSubsystem();
		if (!Data) return;
		PendingDeletion = Data->PrepareDeleteItems(PendingDeletionItems);
		auto Preview = std::make_shared<FContentBrowserDeletePreview>();
		Preview->DisplayName = "selected items";
		DeletionFallbackPreview = std::move(Preview);
		bDeletionPlanRefreshed = false;
		bDeletePopupRequested = true;
	}

	auto FContentBrowserPanel::DeleteSelection() -> void
	{
		if (!CanMutateContent()) return;
		if (!PendingDeletion.Request)
		{
			SetError("Content deletion is unavailable.");
			return;
		}
		const auto PreviousPlan = GetDeletionPreview();
		auto* Data = GetDataSubsystem();
		const bool bDeleted = Data && Data->BulkDeleteItems(PendingDeletionItems, PendingDeletion);
		if (PendingDeletion.Request && GetDeletionPreview() != PreviousPlan)
		{
			bDeletionPlanRefreshed = true;
			return;
		}
		if (bDeleted)
		{
			SelectionState.Selected.clear();
			SelectionState.Anchor = {};
		}
		bDeletionPlanRefreshed = false;
		SynchronizeMountedContentMutation();
	}

	auto FContentBrowserPanel::SynchronizeMountedContentMutation() -> void
	{
		const uint64 MountedContentRevision = HostContext.GetMountedContentMutationRevision
			? HostContext.GetMountedContentMutationRevision()
			: RefreshCoordinator.GetObservedMountedContentRevision();
		const auto Result = RefreshCoordinator.Synchronize(
			MountedContentRevision,
			GetAssetQueryRevision(),
			[this] { return Model.RescanRegistry(); },
			[this](const FContentChangeBatch& Changes) { ApplyContentChanges(Changes); },
			[] { return GetAssetQueryRevision(); });
		if (!Result) SetError(Result.Message);
	}

	auto FContentBrowserPanel::RevealAsset(std::string_view AssetPath) -> bool
	{
		if (HostContext.GetMountedContentMutationRevision)
			RefreshCoordinator.ObserveMountedContent(HostContext.GetMountedContentMutationRevision());
		if (AdmissionState != EAdmissionState::Accepting)
			return false;
		const auto PreviousNavigation = Model.GetNavigationRevision();
		PendingItemAction.reset();
		CancelRename();
		const auto Key = Model.RevealAsset(AssetPath);
		if (!Key.IsValid()) return false;
		SearchBuffer.fill('\0');
		if (Model.GetNavigationRevision() != PreviousNavigation)
		{
			ThumbnailReferences->CancelPendingRequests();
			bResetContentScroll = true;
		}
		SelectionState.Selected.clear();
		SelectionState.Selected.insert(Key);
		SelectionState.Anchor = {};
		return true;
	}

	auto FContentBrowserPanel::RequestFocus() -> bool
	{
		if (AdmissionState != EAdmissionState::Accepting)
			return false;
		bFocusSearch = true;
		return true;
	}

	auto FContentBrowserPanel::StopRequestAdmission() -> void
	{
		if (AdmissionState == EAdmissionState::Stopped)
			return;
		AdmissionState = EAdmissionState::Stopping;
		if (auto* Data = HostContext.DataSubsystem.Get()) Data->OnItemDataUpdated().Remove(ItemDataUpdatedHandle);
		ItemDataUpdatedHandle = {};
		const std::weak_ptr<void> Lifetime = ExtensionLifetime;
		ExtensionLifetime.reset();
		StopHostPresentations(Lifetime);
		DataQuery.StopRequestAdmission();
		DragSnapshot.reset();
		Model.CancelPendingSnapshots();
		PendingDeletion = {};
		PendingDeletionItems.clear();
		DeferredTreeAction = {};
		DeferredContentAction = {};
		PendingItemAction.reset();
		CancelRename();
		if (ThumbnailReferences) ThumbnailReferences->CancelPendingRequests();
		AdmissionState = EAdmissionState::Stopped;
	}

	auto FContentBrowserPanel::ShowInExplorer(
		std::string_view PhysicalPath) const -> void
	{
#ifdef _WIN32
		const std::filesystem::path Path(PhysicalPath);
		std::filesystem::path PreferredPath = Path;
		const std::wstring WidePath = PreferredPath.make_preferred().wstring();
		if (ContentBrowserFilesystem::Probe(Path).IsDirectory())
			ShellExecuteW(
				nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOW);
		else
		{
			const std::wstring Args = L"/select,\"" + WidePath + L"\"";
			ShellExecuteW(
				nullptr,
				L"open",
				L"explorer.exe",
				Args.c_str(),
				nullptr,
				SW_SHOW);
		}
#endif
	}

	auto FContentBrowserPanel::CopyToClipboard(std::string_view Text) const
		-> void
	{
		ImGui::SetClipboardText(std::string(Text).c_str());
	}

	auto FContentBrowserPanel::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
	}

	auto FContentBrowserPanel::SetWarning(std::string Message) -> void
	{
		WarningMessage = std::move(Message);
	}
} // namespace Durin::ContentBrowser::Private
