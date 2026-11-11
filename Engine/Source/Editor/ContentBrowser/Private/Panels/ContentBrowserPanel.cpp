#include "Asset/RegistryOperations.h"
#ifdef _WIN32
#include <shellapi.h>
#endif
#include "Panels/ContentBrowserPanel.h"
#include "DObject/Package.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
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

	auto FContentBrowserPanel::SaveAssetPackage(const FContentBrowserItem& Item) -> void
	{
		DataSource.BulkSaveItems(std::span(&Item, 1));
	}

	auto FContentBrowserPanel::ResaveAssetPackages(std::vector<FContentBrowserItem> Items) -> void
	{
		DataSource.BulkSaveItems(Items, EAssetSaveMode::CanonicalResave);
	}

	FContentBrowserPanel::FContentBrowserPanel(FPresentationSettings InSettings,
		FSavePresentationSettings InSaveSettings, FContentBrowserHostContext InHost)
		: PresentationSettings(std::move(InSettings))
		, SavePresentationSettings(std::move(InSaveSettings))
		, HostContext(std::move(InHost))
		, RefreshCoordinator(
			HostContext.GetMountedContentMutationRevision ? HostContext.GetMountedContentMutationRevision() : uint64{0},
			GetAssetQueryRevision(), HostContext.ReconciliationState)
		, DataSource(HostContext)
		, Model(DataSource, true)
		, IconSize(PresentationSettings.IconSize)
		, DirectoryTreeWidth(PresentationSettings.TreeWidth)
	{
		RefreshCoordinator.SetChangeSources(HostContext.CaptureMountedContentChanges, CaptureAssetQueryChanges);
		RefreshCoordinator.SetPublicationSuspension([this](bool bSuspended) { Model.SetSnapshotPublicationSuspended(bSuspended); });
		DataSource.SetDiagnosticSink([this](ELogLevel Level, std::string_view Message) {
			DURIN_LOG(Level, "Content Browser: {}", Message);
			if (Level >= ELogLevel::Error) SetError("Content Browser operation failed. See Console for details.");
			else if (Level == ELogLevel::Warn) SetWarning("Content Browser operation reported warnings. See Console for details.");
		});
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
		if (Model.GetCurrentPhysicalPath().empty())
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
		const std::string PreviousDirectory = Model.GetCurrentPhysicalPath();
		if (!Model.NavigateToPhysical(PhysicalPath, bAddHistory)) return false;
		if (Model.GetCurrentPhysicalPath() == PreviousDirectory) return true;
		CancelRename();
		ThumbnailReferences->CancelPendingRequests();
		SelectionState.Selected.clear();
		SelectionState.Anchor.clear();
		bResetContentScroll = true;
		RepairSelection();
		return true;
	}

	auto FContentBrowserPanel::NavigateHistory(int32 Delta) -> void
	{
		if (HostContext.GetMountedContentMutationRevision)
			RefreshCoordinator.ObserveMountedContent(HostContext.GetMountedContentMutationRevision());
		const std::string PreviousDirectory = Model.GetCurrentPhysicalPath();
		if (Model.NavigateHistory(Delta))
		{
			if (Model.GetCurrentPhysicalPath() == PreviousDirectory) return;
			CancelRename();
			ThumbnailReferences->CancelPendingRequests();
			SelectionState.Selected.clear();
			SelectionState.Anchor.clear();
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
		SelectionState.Apply(Mapped);
		for (const auto& Change : Mapped.Changes)
		{
			FContentBrowserSelection::Migrate(RenameTarget, Change);
			if (PendingItemAction) FContentBrowserSelection::Migrate(PendingItemAction->PhysicalPath, Change);
		}
		ThumbnailReferences->ApplyContentChanges(Mapped);
		const auto Navigation = Model.GetNavigationRevision();
		if (Model.ApplyContentChanges(Mapped)) RepairSelection();
		if (PendingItemAction && PendingItemAction->NavigationRevision == Navigation)
		{
			if (PendingItemAction->PhysicalPath.empty()) PendingItemAction.reset();
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
		if (Item.Kind == EContentBrowserItemKind::Folder)
		{
			NavigateToPhysical(Item.PhysicalPath);
			return;
		}
		if (Item.Kind == EContentBrowserItemKind::Redirector)
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Item.VirtualPath, Path))
			{
				SetError("The redirector path is invalid.");
				return;
			}
			const FObjectPathResolveResult Resolution =
				ResolveAssetObjectPathForOperation(Path);
			if (!Resolution || !Resolution.FinalAssetData
				|| !HostContext.OpenAsset
				|| !HostContext.OpenAsset(
					Resolution.FinalPath.ToString(),
					Resolution.FinalAssetData->AssetClassName))
				SetError("The redirector destination could not be opened.");
			return;
		}
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			if (!HostContext.OpenAsset || !HostContext.OpenAsset(Item.VirtualPath, Item.AssetClassName))
				SetError(std::format(
					"No editor is registered for {} assets.",
					ContentBrowserItemView::TypeLabel(Item)));
			return;
		}
#ifdef _WIN32
		const std::wstring WidePath =
			std::filesystem::path(Item.PhysicalPath).make_preferred().wstring();
		ShellExecuteW(
			nullptr, L"open", WidePath.c_str(), nullptr, nullptr, SW_SHOW);
#endif
	}

	auto FContentBrowserPanel::BeginRename(const FContentBrowserItem& Item) -> void
	{
		std::string Reason;
		const auto Name = Item.Kind == EContentBrowserItemKind::Asset ? Item.PackagePath.GetPackageName() : std::string_view(Item.Name);
		if (!DataSource.CanRenameItem(Item, Name, &Reason)) { SetError(std::move(Reason)); return; }
		CancelRename();
		RenameTarget = Item.StableId();
		RenameBuffer.fill(0);
		const std::string_view InitialName =
			Item.Kind == EContentBrowserItemKind::Asset
				|| Item.Kind == EContentBrowserItemKind::Redirector
			? Item.PackagePath.GetPackageName()
			: std::string_view(Item.Name);
		std::memcpy(
			RenameBuffer.data(),
			InitialName.data(),
			std::min(InitialName.size(), RenameBuffer.size() - 1));
		bFocusRename = true;
	}

	auto FContentBrowserPanel::CancelRename() -> void
	{
		PendingFolder.reset();
		RenameTarget.clear();
		bFocusRename = false;
	}

	auto FContentBrowserPanel::CommitRename(const FContentBrowserItem& Item)
		-> bool
	{
		const std::string NewName = RenameBuffer.data();
		if (PendingFolder)
		{
			if (PendingFolder->NavigationRevision != Model.GetNavigationRevision())
			{
				CancelRename();
				return false;
			}
			if (!DataSource.CanCreateFolder(PendingFolder->Directory, NewName, &PendingFolder->ValidationError))
			{
				bFocusRename = true;
				return false;
			}
			FContentBrowserItem Created;
			if (!DataSource.CreateFolder(PendingFolder->Directory, NewName, Created))
			{
				bFocusRename = true;
				return false;
			}
			CancelRename();
			SearchBuffer.fill(0);
			if (!SyncToItem(Created)) SetError("The folder was created, but it could not be shown in the content browser.");
			return true;
		}
		const std::string_view CurrentName =
			Item.Kind == EContentBrowserItemKind::Asset
				|| Item.Kind == EContentBrowserItemKind::Redirector
			? Item.PackagePath.GetPackageName()
			: std::string_view(Item.Name);
		if (NewName == CurrentName)
		{
			RenameTarget.clear();
			return true;
		}

		FContentBrowserItem NewItem;
		const bool Result = DataSource.RenameItem(Item, NewName, NewItem);
		if (!Result)
		{
			return false;
		}
		RenameTarget.clear();
		if (!SyncToItem(NewItem)) SetError("The item was renamed, but it could not be shown in the content browser.");
		return true;
	}

	auto FContentBrowserPanel::SyncToItem(const FContentBrowserItem& Item) -> bool
	{
		if (Item.Kind == EContentBrowserItemKind::Asset || Item.Kind == EContentBrowserItemKind::Redirector)
			return RevealAsset(Item.VirtualPath);
		if (Item.PhysicalPath.empty() || Model.RevealPhysicalItem(Item.PhysicalPath).empty()) return false;
		SelectionState.Selected.clear();
		SelectionState.Selected.insert(Item.PhysicalPath);
		return true;
	}

	auto FContentBrowserPanel::DuplicateAsset(
		const FContentBrowserItem& Item) -> void
	{
		FContentBrowserItem NewItem;
		DataSource.DuplicateItem(Item, NewItem);
		if (!NewItem.StableId().empty()) SyncToItem(NewItem);
	}

	auto FContentBrowserPanel::CopyContentSelection() -> void
	{
		std::vector<FContentBrowserItem> Items;
		for (const auto& Item : Model.GetItems())
			if (SelectionState.Selected.contains(Item.StableId()))
			{
				if (Item.Kind == EContentBrowserItemKind::Redirector)
				{
					SetError("Redirectors cannot be copied.");
					return;
				}
				Items.push_back(Item);
			}
		if (Items.empty()) return;
		CopiedContent.clear();
		CopiedContentToken.clear();
		if (Items.size() == 1 && Items.front().Kind == EContentBrowserItemKind::Asset)
		{
			CopyToClipboard(Items.front().VirtualPath);
			return;
		}
		CopiedContent = std::move(Items);
		CopiedContentToken = "Durin Content Selection " + std::to_string(++CopySerial);
		CopyToClipboard(CopiedContentToken);
	}

	auto FContentBrowserPanel::PasteContent(
		std::string_view DestinationDirectory) -> void
	{
		FTopLevelAssetPath SourcePath;
		const bool bContent = HasCopiedContent();
		if (!bContent && !ReadAssetClipboard(SourcePath)) return;
		const std::string_view Directory = DestinationDirectory.empty()
			? std::string_view(Model.GetCurrentVirtualPath())
			: DestinationDirectory;
		FContentBrowserItem NewItem;
		if (bContent)
		{
			const auto PhysicalDirectory = FContentBrowserPaths::VirtualToPhysical(Directory);
			if (DataSource.BulkCopyItems(CopiedContent, PhysicalDirectory)) NavigateToPhysical(PhysicalDirectory);
		}
		else
		{
			FContentBrowserItem Source;
			Source.Kind = EContentBrowserItemKind::Asset;
			Source.VirtualPath = SourcePath.ToString();
			Source.PackagePath = SourcePath.GetPackagePath();
			DataSource.DuplicateItem(Source, FContentBrowserPaths::VirtualToPhysical(Directory), NewItem);
		}
		if (!NewItem.StableId().empty()) SyncToItem(NewItem);

	}

	auto FContentBrowserPanel::HasContentClipboard() const -> bool
	{
		if (HasCopiedContent()) return static_cast<bool>(DataSource.CanModify());
		FTopLevelAssetPath SourcePath;
		if (!ReadAssetClipboard(SourcePath)) return false;
		FContentBrowserItem Source;
		Source.Kind = EContentBrowserItemKind::Asset;
		Source.VirtualPath = SourcePath.ToString();
		Source.PackagePath = SourcePath.GetPackagePath();
		return DataSource.CanDuplicateItem(Source);
	}

	auto FContentBrowserPanel::CreateFolder(std::string_view PhysicalDirectory) -> void
	{
		const std::string Directory(PhysicalDirectory);
		FContentBrowserItem Item;
		if (!DataSource.PrepareFolder(Directory, Item)) return;
		if (!NavigateToPhysical(Directory)) return;
		CancelRename();
		PendingItemAction.reset();
		SelectionState.Selected.clear();
		PendingFolder = FPendingFolder{Item, Directory, Model.GetNavigationRevision()};
		RenameTarget = "##CreateFolder";
		RenameBuffer.fill(0);
		std::memcpy(RenameBuffer.data(), Item.Name.data(), std::min(Item.Name.size(), RenameBuffer.size() - 1));
		bFocusRename = true;
		bResetContentScroll = true;
	}

	auto FContentBrowserPanel::FixUpRedirector(
		const FContentBrowserItem& Item) -> void
	{
		std::vector<FPackagePath> Redirectors;
		for (const FContentBrowserItem& Candidate : Model.GetItems())
		{
			if (Candidate.Kind != EContentBrowserItemKind::Redirector
				|| !SelectionState.Selected.contains(Candidate.StableId()))
				continue;
			if (!Candidate.PackagePath.IsValid())
			{
				SetError("A selected redirector path is invalid.");
				return;
			}
			Redirectors.push_back(Candidate.PackagePath);
		}
		if (Redirectors.empty())
		{
			if (!Item.PackagePath.IsValid())
			{
				SetError("The redirector path is invalid.");
				return;
			}
			Redirectors.push_back(Item.PackagePath);
		}
		DataSource.FixUpRedirectors(Redirectors);
	}

	auto FContentBrowserPanel::FixUpFolder(
		std::string_view VirtualDirectory) -> void
	{
		DataSource.FixUpRedirectorsInFolder(VirtualDirectory);
	}

	auto FContentBrowserPanel::FixUpProject() -> void
	{
		DataSource.FixUpAllRedirectors();
	}

	auto FContentBrowserPanel::FocusFolderInParent(
		std::string_view PhysicalDirectory, bool bDelete) -> void
	{
		const std::string Physical = Model.RevealPhysicalItem(PhysicalDirectory);
		SearchBuffer.fill('\0');
		if (Physical.empty())
		{
			SetError("The folder could not be shown in its mounted parent directory.");
			return;
		}
		ThumbnailReferences->CancelPendingRequests();
		bResetContentScroll = true;
		PendingItemAction = FPendingItemAction{Physical, Model.GetNavigationRevision(), bDelete};
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
		const auto It = std::ranges::find(Items, Action.PhysicalPath, &FContentBrowserItem::PhysicalPath);
		if (It == Items.end())
		{
			SetError("The folder could not be found after refreshing its parent directory.");
			return;
		}
		SelectionState.Selected.clear();
		SelectionState.Selected.insert(It->StableId());
		SelectionState.Anchor = It->StableId();
		if (Action.bDelete) RequestDeleteSelection();
		else BeginRename(*It);
	}

	auto FContentBrowserPanel::RequestDeleteSelection() -> void
	{
		if (SelectionState.Selected.empty()) return;
		DataSource.DismissDeletion(PendingDeletionPlan);
		PendingDeletionPlan = DataSource.PrepareDeleteItems(
			ContentBrowserQuery::CopySelection(Model.GetItems(), SelectionState.Selected));
		bDeletionPlanRefreshed = false;
		bDeletePopupRequested = true;
	}

	auto FContentBrowserPanel::DeleteSelection() -> void
	{
		if (!PendingDeletionPlan)
		{
			SetError("Content deletion is unavailable.");
			return;
		}
		const auto PreviousPlan = PendingDeletionPlan;
		const bool bDeleted = DataSource.DeleteItems(PendingDeletionPlan);
		if (PendingDeletionPlan && PendingDeletionPlan != PreviousPlan)
		{
			bDeletionPlanRefreshed = true;
			return;
		}
		if (bDeleted)
		{
			SelectionState.Selected.clear();
			SelectionState.Anchor.clear();
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
		const std::string PreviousDirectory = Model.GetCurrentPhysicalPath();
		PendingItemAction.reset();
		CancelRename();
		const std::string PhysicalPath = Model.RevealAsset(AssetPath);
		if (PhysicalPath.empty()) return false;
		SearchBuffer.fill('\0');
		if (Model.GetCurrentPhysicalPath() != PreviousDirectory)
		{
			ThumbnailReferences->CancelPendingRequests();
			bResetContentScroll = true;
		}
		SelectionState.Selected.clear();
		SelectionState.Selected.insert(PhysicalPath);
		SelectionState.Anchor.clear();
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
		const std::weak_ptr<void> Lifetime = ExtensionLifetime;
		ExtensionLifetime.reset();
		StopHostPresentations(Lifetime);
		DataSource.StopRequestAdmission();
		Model.CancelPendingSnapshots();
		PendingDeletionPlan.reset();
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
