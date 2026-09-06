#include "Panels/ContentBrowserPanel.h"
#include "DObject/Package.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "AssetTools/IAssetTools.h"
#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Panels/ContentBrowserItemView.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		auto ReadAssetClipboard(FTopLevelAssetPath& OutPath) -> bool
		{
			OutPath = {};
			const char* Clipboard = ImGui::GetClipboardText();
			if (!Clipboard) return false;
			return FTopLevelAssetPath::TryCreate(Clipboard, OutPath);
		}
	}

	auto FContentBrowserPanel::SaveAssetPackage(const FPackagePath& Path) -> void
	{
		const FAssetOperationResult Save = IAssetTools::Get().SaveAssets({
			.AssetPaths = {Path},
			.Publish = [this](const FAssetOperationNotification&) {
				PublishMountedContentMutation();
			}});
		if (!Save) SetError(Save.Message);
	}

	auto FContentBrowserPanel::ResaveAssetPackages(std::vector<FPackagePath> Paths) -> void
	{
		std::ranges::sort(Paths, {}, &FPackagePath::ToString);
		Paths.erase(std::unique(Paths.begin(), Paths.end()), Paths.end());
		const FAssetOperationResult Result = IAssetTools::Get().SaveAssets({
			.AssetPaths = std::move(Paths),
			.Mode = EAssetSaveMode::CanonicalResave,
			.Publish = [this](const FAssetOperationNotification&) {
				PublishMountedContentMutation();
			}});
		if (!Result) SetError(Result.Message);
	}

	FContentBrowserPanel::FContentBrowserPanel(
		::Durin::Editor::ContentBrowser::FPresentationSettings InSettings,
		::Durin::Editor::ContentBrowser::FSavePresentationSettings InSaveSettings,
		FOpenAsset InOpenAsset,
		FMoveAssets InMoveAssets,
		FFixUpAssets InFixUpRedirectors,
		FGetMountedContentMutationRevision InGetMountedContentMutationRevision,
		FNotifyMountedContentMutation InNotifyMountedContentMutation,
		FQueryReimport InQueryReimport,
		FReimport InReimport,
		std::shared_ptr<FMountedContentReconciliationState>
			InMountedContentReconciliationState,
		FTaskScopeToken InThumbnailTaskScope)
		: PresentationSettings(std::move(InSettings))
		, SavePresentationSettings(std::move(InSaveSettings))
		, OpenAsset(std::move(InOpenAsset))
		, GetMountedContentMutationRevision(
			std::move(InGetMountedContentMutationRevision))
		, NotifyMountedContentMutation(InNotifyMountedContentMutation)
		, QueryReimport(std::move(InQueryReimport))
		, Reimport(std::move(InReimport))
		, RefreshCoordinator(
			GetMountedContentMutationRevision
				? GetMountedContentMutationRevision()
				: uint64{0},
			GetAssetCatalogRevision(),
			std::move(InMountedContentReconciliationState))
		, Model(true, InThumbnailTaskScope)
		, Operations(Model, std::move(InMoveAssets), {},
			std::move(InFixUpRedirectors), std::move(InNotifyMountedContentMutation))
		, IconSize(PresentationSettings.IconSize)
		, DirectoryTreeWidth(PresentationSettings.TreeWidth)
	{
		Model.RefreshMountSnapshot();
		ThumbnailReferences = std::make_unique<FContentBrowserThumbnailReferences>(
			std::move(InThumbnailTaskScope));
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
	}

	FContentBrowserPanel::~FContentBrowserPanel() = default;

	auto FContentBrowserPanel::NotifyMountedContentChanged() -> bool
	{
		if (AdmissionState != ::Durin::Editor::ContentBrowser::EAdmissionState::Accepting)
			return false;
		PublishMountedContentMutation();
		return true;
	}

	auto FContentBrowserPanel::RevealDirectory(
		std::string_view DirectoryPath) -> bool
	{
		if (AdmissionState != ::Durin::Editor::ContentBrowser::EAdmissionState::Accepting)
			return false;
		const FAssetPathResult Resolved =
			FMountPaths::ResolveAssetPath(DirectoryPath);
		if (!Resolved)
		{
			SetError(Resolved.Message);
			return false;
		}
		if (!NavigateToPhysical(Resolved.PhysicalPath.generic_string()))
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
		const std::string PreviousDirectory = Model.GetCurrentPhysicalPath();
		if (!Model.NavigateToPhysical(PhysicalPath, bAddHistory)) return false;
		if (Model.GetCurrentPhysicalPath() == PreviousDirectory) return true;
		ThumbnailReferences->CancelPendingRequests();
		Selection.clear();
		SelectionAnchor.clear();
		bResetContentScroll = true;
		RepairSelection();
		return true;
	}

	auto FContentBrowserPanel::NavigateHistory(int32 Delta) -> void
	{
		const std::string PreviousDirectory = Model.GetCurrentPhysicalPath();
		if (Model.NavigateHistory(Delta))
		{
			if (Model.GetCurrentPhysicalPath() == PreviousDirectory) return;
			ThumbnailReferences->CancelPendingRequests();
			Selection.clear();
			SelectionAnchor.clear();
			bResetContentScroll = true;
			RepairSelection();
		}
	}

	auto FContentBrowserPanel::Refresh(bool bRescanRegistry) -> void
	{
		if (bRescanRegistry)
		{
			const uint64 MountedContentRevision = GetMountedContentMutationRevision
				? GetMountedContentMutationRevision()
				: RefreshCoordinator.GetObservedMountedContentRevision();
			const FAssetResult Result =
				RefreshCoordinator.ReconcileExplicitly(
					MountedContentRevision,
					[this] { return Model.RescanRegistry(); },
					[this] { RefreshPublishedContent(); },
					[] { return GetAssetCatalogRevision(); });
			if (!Result) SetError(Result.Message);
			return;
		}
		RefreshCoordinator.RefreshRegistryView(
			GetAssetCatalogRevision(),
			[this] { RefreshPublishedContent(); });
	}

	auto FContentBrowserPanel::RefreshPublishedContent() -> void
	{
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

	auto FContentBrowserPanel::PublishMountedContentMutation() -> void
	{
		if (NotifyMountedContentMutation) NotifyMountedContentMutation();
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
		if (Model.IsLoading()) return;
		const auto Items = Model.GetItems();
		std::erase_if(
			Selection,
			[&](const std::string& Id) {
				return std::ranges::none_of(
					Items,
					[&](const FContentBrowserItem& Item) {
						return Item.StableId() == Id;
					});
			});
		if (!SelectionAnchor.empty() && !Selection.contains(SelectionAnchor))
			SelectionAnchor.clear();
	}

	auto FContentBrowserPanel::SelectItem(size_t Index) -> void
	{
		const auto Items = Model.GetItems();
		if (Index >= Items.size()) return;
		const std::string& Id = Items[Index].StableId();
		const ImGuiIO& IO = ImGui::GetIO();
		if (IO.KeyShift && !SelectionAnchor.empty())
		{
			auto AnchorIt = std::ranges::find_if(
				Items,
				[&](const FContentBrowserItem& Item) {
					return Item.StableId() == SelectionAnchor;
				});
			if (AnchorIt != Items.end())
			{
				const size_t AnchorIndex =
					static_cast<size_t>(std::distance(Items.begin(), AnchorIt));
				if (!IO.KeyCtrl) Selection.clear();
				for (size_t I = std::min(Index, AnchorIndex);
					 I <= std::max(Index, AnchorIndex);
					 ++I)
					Selection.insert(Items[I].StableId());
				return;
			}
		}
		if (IO.KeyCtrl)
		{
			if (!Selection.erase(Id)) Selection.insert(Id);
		}
		else
		{
			Selection.clear();
			Selection.insert(Id);
		}
		SelectionAnchor = Id;
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
			FObjectPath Path;
			if (!FObjectPath::TryCreate(Item.VirtualPath, Path))
			{
				SetError("The redirector path is invalid.");
				return;
			}
			const FObjectPathResolveResult Resolution =
				ResolveAssetObjectPath(Path);
			if (!Resolution || !Resolution.FinalAssetData
				|| !OpenAsset
				|| !OpenAsset(
					Resolution.FinalPath.ToString(),
					Resolution.FinalAssetData->AssetClassName))
				SetError("The redirector destination could not be opened.");
			return;
		}
		if (Item.Kind == EContentBrowserItemKind::Asset)
		{
			if (!OpenAsset || !OpenAsset(Item.VirtualPath, Item.AssetClassName))
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

	auto FContentBrowserPanel::CommitRename(const FContentBrowserItem& Item)
		-> bool
	{
		const std::string NewName = RenameBuffer.data();
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

		const FContentBrowserOperationResult Result =
			Operations.Rename(Item, NewName);
		if (!Result)
		{
			SetError(Result.Status.Message);
			return false;
		}
		RenameTarget.clear();
		if (!Result.Warning.empty()) SetWarning(Result.Warning);
		PublishMountedContentMutation();
		Selection.clear();
		if (!Result.RevealAssetPath.empty())
		{
			if (!RevealAsset(Result.RevealAssetPath))
				SetError("The asset was renamed, but it could not be shown in the content browser.");
		}
		else if (!Result.FocusPhysicalPath.empty())
			Selection.insert(Result.FocusPhysicalPath);
		return true;
	}

	auto FContentBrowserPanel::DuplicateAsset(
		const FContentBrowserItem& Item) -> void
	{
		const FContentBrowserOperationResult Result = Operations.Duplicate(Item);
		if (!Result)
		{
			SetError(Result.Status.Message);
			return;
		}
		RevealAsset(Result.RevealAssetPath);
	}

	auto FContentBrowserPanel::CopyAssetSelection() -> void
	{
		if (Selection.size() != 1) return;
		const auto It = std::ranges::find_if(
			Model.GetItems(),
			[&](const FContentBrowserItem& Item) {
				return Selection.contains(Item.StableId());
			});
		if (It == Model.GetItems().end()
			|| It->Kind != EContentBrowserItemKind::Asset)
			return;
		CopyToClipboard(It->VirtualPath);
	}

	auto FContentBrowserPanel::PasteAsset(
		std::string_view DestinationDirectory) -> void
	{
		FTopLevelAssetPath SourcePath;
		if (!ReadAssetClipboard(SourcePath)) return;
		const std::string_view Directory = DestinationDirectory.empty()
			? std::string_view(Model.GetCurrentVirtualPath())
			: DestinationDirectory;
		const FContentBrowserOperationResult Result = Operations.Duplicate(
			SourcePath, Directory);
		if (!Result)
		{
			SetError(Result.Status.Message);
			return;
		}
		RevealAsset(Result.RevealAssetPath);
	}

	auto FContentBrowserPanel::HasAssetClipboard() const -> bool
	{
		FTopLevelAssetPath SourcePath;
		if (!ReadAssetClipboard(SourcePath)) return false;
		const FTopLevelAssetCatalogEntry Entry =
			FindTopLevelAssetExact(SourcePath);
		return Entry && !Entry->IsRedirector();
	}

	auto FContentBrowserPanel::CreateFolder(
		std::string_view PhysicalDirectory) -> void
	{
		const std::string Directory(PhysicalDirectory);
		const FContentBrowserOperationResult Result =
			Operations.CreateFolder(Directory);
		if (!Result)
		{
			SetError(Result.Status.Message);
			return;
		}
		const std::string PreviousDirectory = Model.GetCurrentPhysicalPath();
		const std::string FocusPhysicalPath =
			Model.RevealPhysicalItem(Result.FocusPhysicalPath);
		SearchBuffer.fill('\0');
		if (FocusPhysicalPath.empty())
		{
			SetError("The folder was created, but it could not be shown in the content browser.");
			return;
		}
		if (Model.GetCurrentPhysicalPath() != PreviousDirectory)
		{
			ThumbnailReferences->CancelPendingRequests();
			bResetContentScroll = true;
		}

		PendingItemAction = FPendingItemAction{FocusPhysicalPath, Model.GetNavigationRevision(), false};
		CompletePendingItemAction();
	}

	auto FContentBrowserPanel::FixUpRedirector(
		const FContentBrowserItem& Item) -> void
	{
		std::vector<FPackagePath> Redirectors;
		for (const FContentBrowserItem& Candidate : Model.GetItems())
		{
			if (Candidate.Kind != EContentBrowserItemKind::Redirector
				|| !Selection.contains(Candidate.StableId()))
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
		const FAssetResult Result =
			Operations.FixUpRedirectors(Redirectors);
		if (!Result)
		{
			SetError(Result.Message);
			return;
		}
	}

	auto FContentBrowserPanel::FixUpFolder(
		std::string_view VirtualDirectory) -> void
	{
		const FAssetResult Result =
			Operations.FixUpRedirectorsInFolder(VirtualDirectory);
		if (!Result)
		{
			SetError(Result.Message);
			return;
		}
	}

	auto FContentBrowserPanel::FixUpProject() -> void
	{
		const FAssetResult Result = Operations.FixUpAllRedirectors();
		if (!Result)
		{
			SetError(Result.Message);
			return;
		}
		PublishMountedContentMutation();
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
		Selection.clear();
		Selection.insert(It->StableId());
		SelectionAnchor = It->StableId();
		if (Action.bDelete) RequestDeleteSelection();
		else BeginRename(*It);
	}

	auto FContentBrowserPanel::RequestDeleteSelection() -> void
	{
		if (Selection.empty()) return;
		PendingDeletionPlan = Operations.BuildDeletionPlan(
			ContentBrowserQuery::CopySelection(Model.GetItems(), Selection), Selection);
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
		if (!Operations.IsDeletionPlanCurrent(*PendingDeletionPlan))
		{
			PendingDeletionPlan = Operations.BuildDeletionPlan(
				ContentBrowserQuery::CopySelection(Model.GetItems(), Selection), Selection);
			bDeletionPlanRefreshed = true;
			return;
		}
		if (!PendingDeletionPlan->CanExecute()) return;

		FContentDeletionOperation Deletion(PendingDeletionPlan);
		if (!Deletion.Execute())
		{
			SetError(Deletion.GetDetails());
			return;
		}
		Selection.clear();
		SelectionAnchor.clear();
		PendingDeletionPlan.reset();
		bDeletionPlanRefreshed = false;
		SynchronizeMountedContentMutation();
	}

	auto FContentBrowserPanel::SynchronizeMountedContentMutation() -> void
	{
		const uint64 MountedContentRevision = GetMountedContentMutationRevision
			? GetMountedContentMutationRevision()
			: RefreshCoordinator.GetObservedMountedContentRevision();
		const FAssetResult Result = RefreshCoordinator.Synchronize(
			MountedContentRevision,
			GetAssetCatalogRevision(),
			[this] { return Model.RescanRegistry(); },
			[this] { RefreshPublishedContent(); },
			[] { return GetAssetCatalogRevision(); });
		if (!Result) SetError(Result.Message);
	}

	auto FContentBrowserPanel::RevealAsset(std::string_view AssetPath) -> bool
	{
		if (AdmissionState != ::Durin::Editor::ContentBrowser::EAdmissionState::Accepting)
			return false;
		const std::string PreviousDirectory = Model.GetCurrentPhysicalPath();
		PendingItemAction.reset();
		const std::string PhysicalPath = Model.RevealAsset(AssetPath);
		if (PhysicalPath.empty()) return false;
		SearchBuffer.fill('\0');
		if (Model.GetCurrentPhysicalPath() != PreviousDirectory)
		{
			ThumbnailReferences->CancelPendingRequests();
			bResetContentScroll = true;
		}
		Selection.clear();
		Selection.insert(PhysicalPath);
		SelectionAnchor.clear();
		return true;
	}

	auto FContentBrowserPanel::RequestFocus() -> bool
	{
		if (AdmissionState != ::Durin::Editor::ContentBrowser::EAdmissionState::Accepting)
			return false;
		bFocusSearch = true;
		return true;
	}

	auto FContentBrowserPanel::StopRequestAdmission() -> void
	{
		if (AdmissionState == ::Durin::Editor::ContentBrowser::EAdmissionState::Stopped)
			return;
		AdmissionState = ::Durin::Editor::ContentBrowser::EAdmissionState::Stopping;
		Model.CancelPendingSnapshots();
		PendingItemAction.reset();
		if (ThumbnailReferences) ThumbnailReferences->CancelPendingRequests();
		AdmissionState = ::Durin::Editor::ContentBrowser::EAdmissionState::Stopped;
	}

	auto FContentBrowserPanel::ShowInExplorer(
		std::string_view PhysicalPath) const -> void
	{
		Operations.ShowInExplorer(PhysicalPath);
	}

	auto FContentBrowserPanel::CopyToClipboard(std::string_view Text) const
		-> void
	{
		Operations.CopyToClipboard(Text);
	}

	auto FContentBrowserPanel::SetError(std::string Message) -> void
	{
		ErrorMessage = std::move(Message);
	}

	auto FContentBrowserPanel::SetWarning(std::string Message) -> void
	{
		WarningMessage = std::move(Message);
	}
} // namespace Durin::Editor::ContentBrowser::Private
