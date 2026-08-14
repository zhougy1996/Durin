#include "Panels/ContentBrowserPanel.h"

#include "AssetImportCore.h"
#include "AssetCanonicalResave.h"
#include "AssetCompatibility.h"
#include "SceneImport.h"
#include "StaticMesh/StaticMesh.h"
#include "AssetSystem.h"
#include "Assets/ContentBrowserThumbnailCache.h"
#include "Misc/Paths.h"
#include "Panels/ContentBrowserItemView.h"
#include "Settings/LevelEditorSessionSettings.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif

namespace Durin::Editor::Level
{
	auto FContentBrowserPanel::SaveAssetPackage(const FAssetPath& Path) -> void
	{
		DPackage* Package = Asset::FAssetManager::Get().FindLoadedPackage(Path);
		if (!Package || !Package->IsDirty())
		{
			SetError("Save Package is available only for a loaded package with authored changes.");
			return;
		}
		const Asset::FAssetResult Save = Asset::SavePackage(Package);
		if (!Save) { SetError(Save.Message); return; }
		PublishMountedContentMutation();
	}

	auto FContentBrowserPanel::ResaveAssetPackages(std::vector<FAssetPath> Paths) -> void
	{
		std::ranges::sort(Paths, {}, &FAssetPath::ToString);
		Paths.erase(std::unique(Paths.begin(), Paths.end()), Paths.end());
		const Asset::FAssetPackageDiscoverySnapshot Snapshot =
			Asset::CaptureMountedAssetPackageSnapshot();
		if (Snapshot.Status != Asset::EAssetPackageSnapshotStatus::Completed)
		{
			SetError(Snapshot.Error.empty() ? "Canonical-resave discovery did not complete." : Snapshot.Error);
			return;
		}
		const Asset::FReflectionCompatibilityCatalog Catalog =
			Asset::FReflectionCompatibilityCatalog::Capture();
		std::vector<Asset::FAssetPackageCompatibilityRecord> Records;
		for (const FAssetPath& Path : Paths)
		{
			const auto Input = std::ranges::find(Snapshot.Packages, Path,
				&Asset::FAssetPackageCompatibilityProbeInput::PackagePath);
			if (Input == Snapshot.Packages.end())
			{
				SetError(std::format("Package {} is not in an authoring-mounted snapshot.", Path.ToString()));
				return;
			}
			auto Probe = Asset::ProbeAssetPackageCompatibility(*Input, Catalog);
			if (!Probe.Record)
			{
				SetError(std::format("Package {} could not be inspected.", Path.ToString()));
				return;
			}
			Records.push_back(std::move(*Probe.Record));
		}
		Asset::FAssetCanonicalResaveSelection Selection{
			.Packages = std::move(Paths), .bAllowPlainResave = true};
		auto Plan = Asset::PlanAssetCanonicalResaves(Records, Selection);
		auto Applied = Asset::ApplyAssetCanonicalResaves(std::move(Plan), Catalog);
		if (Applied.Status != Asset::EAssetCanonicalResaveApplyStatus::Succeeded)
		{
			SetError(Applied.Diagnostic.empty()
				? Asset::SerializeAssetCanonicalResaveApplyReport(Applied) : Applied.Diagnostic);
			return;
		}
		PublishMountedContentMutation();
	}

	FContentBrowserPanel::FContentBrowserPanel(
		FLevelEditorSessionSettings& InSessionSettings,
		FOpenAsset InOpenAsset,
		FRequestImport InRequestImport,
		FMoveAssets InMoveAssets,
		FExecuteTransaction InExecuteTransaction,
		FGetMountedContentMutationRevision InGetMountedContentMutationRevision,
		FNotifyMountedContentMutation InNotifyMountedContentMutation,
		std::shared_ptr<FMountedContentReconciliationState>
			InMountedContentReconciliationState,
		FTaskScopeToken InThumbnailTaskScope)
		: SessionSettings(InSessionSettings)
		, OpenAsset(std::move(InOpenAsset))
		, RequestImport(std::move(InRequestImport))
		, ExecuteTransaction(std::move(InExecuteTransaction))
		, GetMountedContentMutationRevision(
			std::move(InGetMountedContentMutationRevision))
		, NotifyMountedContentMutation(std::move(InNotifyMountedContentMutation))
		, RefreshCoordinator(
			GetMountedContentMutationRevision
				? GetMountedContentMutationRevision()
				: uint64{0},
			Asset::GetAssetRegistry().GetRevision(),
			std::move(InMountedContentReconciliationState))
		, Model()
		, Operations(Model, std::move(InMoveAssets))
		, IconSize(InSessionSettings.GetContentBrowserIconSize())
		, DirectoryTreeWidth(InSessionSettings.GetContentBrowserTreeWidth())
	{
		Model.RefreshMountSnapshot();
		ThumbnailCache = std::make_unique<FContentBrowserThumbnailCache>(
			std::move(InThumbnailTaskScope));
		ViewMode = static_cast<EContentBrowserViewMode>(
			SessionSettings.GetContentBrowserViewMode());
		bIconSizeLocked = SessionSettings.IsContentBrowserIconSizeLocked();
		Model.SetShowHiddenFiles(
			SessionSettings.GetContentBrowserShowHiddenFiles());
		if (!SessionSettings.GetContentBrowserLastDirectory().empty())
			NavigateToPhysical(
				SessionSettings.GetContentBrowserLastDirectory());
		if (Model.GetCurrentPhysicalPath().empty())
		{
			for (const FContentBrowserModel::FMountSnapshot& Mount :
				 Model.GetMounts())
				if (std::filesystem::is_directory(Mount.PhysicalRoot)
					&& NavigateToPhysical(Mount.PhysicalRoot))
					break;
		}
	}

	FContentBrowserPanel::~FContentBrowserPanel() = default;

	auto FContentBrowserPanel::NotifyMountedContentChanged() -> void
	{
		PublishMountedContentMutation();
	}

	auto FContentBrowserPanel::RevealDirectory(
		std::string_view DirectoryPath) -> void
	{
		const PathUtilities::FAssetPathResult Resolved =
			PathUtilities::ResolveAssetPath(DirectoryPath);
		if (!Resolved)
		{
			SetError(Resolved.Message);
			return;
		}
		if (!NavigateToPhysical(Resolved.PhysicalPath.generic_string()))
			SetError(
				"The requested directory is not part of an automatically scanned Content Browser mount.");
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
		ThumbnailCache->CancelPendingRequests();
		if (!Model.NavigateToPhysical(PhysicalPath, bAddHistory)) return false;
		Selection.clear();
		SelectionAnchor.clear();
		RepairSelection();
		return true;
	}

	auto FContentBrowserPanel::NavigateHistory(int32 Delta) -> void
	{
		ThumbnailCache->CancelPendingRequests();
		if (Model.NavigateHistory(Delta))
		{
			Selection.clear();
			SelectionAnchor.clear();
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
			const Asset::FAssetResult Result =
				RefreshCoordinator.ReconcileExplicitly(
					MountedContentRevision,
					[this] { return Model.RescanRegistry(); },
					[this] { RefreshPublishedContent(); },
					[] { return Asset::GetAssetRegistry().GetRevision(); });
			if (!Result) SetError(Result.Message);
			return;
		}
		RefreshCoordinator.RefreshRegistryView(
			Asset::GetAssetRegistry().GetRevision(),
			[this] { RefreshPublishedContent(); });
	}

	auto FContentBrowserPanel::RefreshPublishedContent() -> void
	{
		ThumbnailCache->CancelPendingRequests();
		std::filesystem::path Directory = Model.GetCurrentPhysicalPath();
		while (!Directory.empty() && !std::filesystem::is_directory(Directory))
		{
			const std::filesystem::path Parent = Directory.parent_path();
			if (Parent == Directory) break;
			Directory = Parent;
		}
		Model.RefreshMountSnapshot();
		if (!Directory.empty()
			&& Directory.generic_string() != Model.GetCurrentPhysicalPath()
			&& NavigateToPhysical(Directory.generic_string()))
			return;
		if (Model.GetCurrentPhysicalPath().empty()
			|| !std::filesystem::is_directory(Model.GetCurrentPhysicalPath())
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
		ThumbnailCache->CancelPendingRequests();
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
		const std::span<const FContentBrowserItem> Items = Model.GetItems();
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
		const std::span<const FContentBrowserItem> Items = Model.GetItems();
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
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
			{
				SetError("The redirector path is invalid.");
				return;
			}
			const Asset::FAssetPathResolveResult Resolution =
				Asset::GetAssetRegistry().ResolveAssetPath(Path);
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
					ItemTypeLabel(Item)));
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
		std::memcpy(
			RenameBuffer.data(),
			Item.Name.data(),
			std::min(Item.Name.size(), RenameBuffer.size() - 1));
		bFocusRename = true;
	}

	auto FContentBrowserPanel::CommitRename(const FContentBrowserItem& Item)
		-> bool
	{
		const std::string NewName = RenameBuffer.data();
		if (NewName == Item.Name)
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
		Selection.clear();
		if (!Result.FocusPhysicalPath.empty())
			Selection.insert(Result.FocusPhysicalPath);
		PublishMountedContentMutation();
		return true;
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
		PublishMountedContentMutation();
		const std::string NormalizedDirectory =
			std::filesystem::path(Result.FocusPhysicalPath)
				.parent_path()
				.generic_string();
		if (Model.GetCurrentPhysicalPath() != NormalizedDirectory)
			NavigateToPhysical(NormalizedDirectory);
		else
			Refresh(false);

		const std::span<const FContentBrowserItem> Items = Model.GetItems();
		auto It = std::ranges::find(
			Items, Result.FocusPhysicalPath, &FContentBrowserItem::PhysicalPath);
		if (It == Items.end()) return;
		Selection.clear();
		Selection.insert(It->StableId());
		BeginRename(*It);
	}

	auto FContentBrowserPanel::CreateLevelAsset(
		std::string_view VirtualDirectory) -> void
	{
		const FContentBrowserOperationResult Result =
			Operations.CreateLevelAsset(VirtualDirectory);
		if (!Result)
		{
			SetError(Result.Status.Message);
			return;
		}
		PublishMountedContentMutation();
		RevealAsset(Result.RevealAssetPath);
	}

	auto FContentBrowserPanel::CreateMaterialAsset(
		std::string_view VirtualDirectory,
		bool bInstance) -> void
	{
		const FContentBrowserOperationResult Result =
			Operations.CreateMaterialAsset(VirtualDirectory, bInstance);
		if (!Result)
		{
			SetError(Result.Status.Message);
			return;
		}
		PublishMountedContentMutation();
		RevealAsset(Result.RevealAssetPath);
		if (OpenAsset
			&& !OpenAsset(Result.RevealAssetPath, Result.OpenAssetClassName))
			SetError(
				"The material was created, but its editor could not be opened.");
	}

	auto FContentBrowserPanel::ReimportAsset(
		const FContentBrowserItem& Item,
		Asset::Import::EImportRecordAction Action) -> void
	{
		const bool bRecreateMissingAssets =
			Action != Asset::Import::EImportRecordAction::Reimport;
		FAssetPath Path;
		if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
		{
			SetError("The selected static-mesh asset path is invalid.");
			return;
		}
		DObject* AssetObject = nullptr;
		const Asset::FAssetResult Load =
			Asset::FAssetManager::Get().LoadAsset(Path, AssetObject);
		if (!Load || !AssetObject)
		{
			SetError(Load ? "The selected asset could not be loaded." : Load.Message);
			return;
		}
		Asset::Import::FImportRecordInspection Inspection =
			Cast<Asset::Import::DImportRecord>(AssetObject)
				? Asset::Import::InspectImportRecord(
					Path, Asset::Import::GetImportRecordIndex())
				: Asset::Import::InspectImportRecordForOutput(
					Path, Asset::Import::GetImportRecordIndex());
		if (Inspection && Inspection.Record)
		{
			const Asset::Import::FImportRecordActionResult Executed =
				Asset::Import::ExecuteImportRecordAction(
					*Inspection.Record, Action,
					Asset::Import::GetImportRecordHandlerRegistry());
			if (!Executed) { SetError(Executed.Message); return; }
			LastReimportOrphans = Executed.Orphans;
			PublishMountedContentMutation();
			RevealAsset(Path.ToString());
			return;
		}
		const Asset::Import::FSingleAssetCapabilitySet Capabilities =
			Asset::Import::QuerySingleAssetCapabilities(
				*AssetObject, Asset::Import::GetProviderRegistry(),
				Asset::Import::GetSingleAssetHandlerRegistry());
		const Asset::Import::FSingleAssetCapability* Reimport = Capabilities.Find(
			Asset::Import::ESingleAssetImportCapability::ReimportCurrentSource);
		if (Reimport && Reimport->bAvailable && !bRecreateMissingAssets)
		{
			const Asset::Import::FSingleAssetPlanResult Planned =
				Asset::Import::CreateSingleAssetReimportPlan(
					{.Asset = AssetObject}, Asset::Import::GetProviderRegistry(),
					Asset::Import::GetSingleAssetHandlerRegistry());
			if (!Planned)
			{
				SetError(Planned.Message);
				return;
			}
			const Asset::Import::FSingleAssetExecutionResult Executed =
				Asset::Import::ExecuteSingleAssetImport(Planned.Plan);
			if (!Executed)
			{
				SetError(Executed.Message);
				return;
			}
			LastReimportOrphans.clear();
			PublishMountedContentMutation();
			RevealAsset(Path.ToString());
			return;
		}

		SetError(Reimport && !Reimport->Diagnostics.empty()
			? Reimport->Diagnostics.back().Message
			: Inspection.Message.empty()
				? "The selected asset has no available reimport capability."
				: Inspection.Message);
	}

	auto FContentBrowserPanel::FixUpRedirector(
		const FContentBrowserItem& Item) -> void
	{
		std::vector<FAssetPath> Redirectors;
		for (const FContentBrowserItem& Candidate : Model.GetItems())
		{
			if (Candidate.Kind != EContentBrowserItemKind::Redirector
				|| !Selection.contains(Candidate.StableId()))
				continue;
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Candidate.VirtualPath, Path))
			{
				SetError("A selected redirector path is invalid.");
				return;
			}
			Redirectors.push_back(std::move(Path));
		}
		if (Redirectors.empty())
		{
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
			{
				SetError("The redirector path is invalid.");
				return;
			}
			Redirectors.push_back(std::move(Path));
		}
		const Asset::FAssetResult Result =
			Operations.FixUpRedirectors(Redirectors);
		if (!Result)
		{
			SetError(Result.Message);
			return;
		}
		PublishMountedContentMutation();
	}

	auto FContentBrowserPanel::FixUpFolder(
		std::string_view VirtualDirectory) -> void
	{
		const Asset::FAssetResult Result =
			Operations.FixUpRedirectorsInFolder(VirtualDirectory);
		if (!Result)
		{
			SetError(Result.Message);
			return;
		}
		PublishMountedContentMutation();
	}

	auto FContentBrowserPanel::FixUpProject() -> void
	{
		const Asset::FAssetResult Result = Operations.FixUpAllRedirectors();
		if (!Result)
		{
			SetError(Result.Message);
			return;
		}
		PublishMountedContentMutation();
	}

	auto FContentBrowserPanel::FocusFolderInParent(
		std::string_view PhysicalDirectory) -> const FContentBrowserItem*
	{
		const std::string NormalizedDirectory =
			std::filesystem::absolute(std::filesystem::path(PhysicalDirectory))
				.lexically_normal()
				.generic_string();
		const std::filesystem::path Parent =
			std::filesystem::path(NormalizedDirectory).parent_path();
		if (!NavigateToPhysical(Parent.generic_string()))
		{
			SetError("The folder's parent is outside the mounted content roots.");
			return nullptr;
		}
		const std::span<const FContentBrowserItem> Items = Model.GetItems();
		auto It = std::ranges::find_if(
			Items,
			[&](const FContentBrowserItem& Item) {
				return Item.Kind == EContentBrowserItemKind::Folder
					&& Item.PhysicalPath == NormalizedDirectory;
			});
		if (It == Items.end())
		{
			SetError(
				"The folder could not be found after refreshing its parent directory.");
			return nullptr;
		}
		Selection.clear();
		Selection.insert(It->StableId());
		SelectionAnchor = It->StableId();
		return &*It;
	}

	auto FContentBrowserPanel::RequestDeleteSelection() -> void
	{
		if (Selection.empty()) return;
		PendingDeletionPlan = Operations.BuildDeletionPlan(
			Model.GetItems(), Selection);
		bDeletionPlanRefreshed = false;
		bDeletePopupRequested = true;
	}

	auto FContentBrowserPanel::DeleteSelection() -> void
	{
		if (!PendingDeletionPlan || !ExecuteTransaction)
		{
			SetError("Content deletion is unavailable because editor history is not active.");
			return;
		}
		if (!Operations.IsDeletionPlanCurrent(*PendingDeletionPlan))
		{
			PendingDeletionPlan = Operations.BuildDeletionPlan(
				Model.GetItems(), Selection);
			bDeletionPlanRefreshed = true;
			return;
		}
		if (!PendingDeletionPlan->CanExecute()) return;

		if (!ExecuteTransaction(
				std::make_unique<FContentDeletionTransaction>(PendingDeletionPlan)))
			return;
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
		const Asset::FAssetResult Result = RefreshCoordinator.Synchronize(
			MountedContentRevision,
			Asset::GetAssetRegistry().GetRevision(),
			[this] { return Model.RescanRegistry(); },
			[this] { RefreshPublishedContent(); },
			[] { return Asset::GetAssetRegistry().GetRevision(); });
		if (!Result) SetError(Result.Message);
	}

	auto FContentBrowserPanel::RevealAsset(std::string_view AssetPath) -> void
	{
		ThumbnailCache->CancelPendingRequests();
		const std::string PhysicalPath = Model.RevealAsset(AssetPath);
		if (PhysicalPath.empty()) return;
		Selection.clear();
		Selection.insert(PhysicalPath);
		SelectionAnchor.clear();
	}

	auto FContentBrowserPanel::ItemTypeLabel(
		const FContentBrowserItem& Item) const -> std::string
	{
		return ContentBrowserItemView::TypeLabel(Item);
	}

	auto FContentBrowserPanel::ItemIcon(
		const FContentBrowserItem& Item) const -> const char*
	{
		return ContentBrowserItemView::Icon(Item);
	}

	auto FContentBrowserPanel::FormatFileSize(uintmax_t Bytes) const
		-> std::string
	{
		return ContentBrowserItemView::FormatFileSize(Bytes);
	}

	auto FContentBrowserPanel::FormatFileTime(
		const std::filesystem::file_time_type& Time) const -> std::string
	{
		return ContentBrowserItemView::FormatFileTime(Time);
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
} // namespace Durin::Editor::Level
