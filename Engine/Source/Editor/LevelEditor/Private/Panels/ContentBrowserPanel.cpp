#include "Panels/ContentBrowserPanel.h"

#include "AssetImportCore.h"
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

namespace Durin
{
	FContentBrowserPanel::FContentBrowserPanel(
		FLevelEditorSessionSettings& InSessionSettings,
		FOpenAsset InOpenAsset,
		FRequestImport InRequestImport,
		FMoveAssets InMoveAssets)
		: SessionSettings(InSessionSettings)
		, OpenAsset(std::move(InOpenAsset))
		, RequestImport(std::move(InRequestImport))
		, Model()
		, Operations(Model, std::move(InMoveAssets))
		, IconSize(InSessionSettings.GetContentBrowserIconSize())
		, DirectoryTreeWidth(InSessionSettings.GetContentBrowserTreeWidth())
	{
		Model.RefreshMountSnapshot();
		ThumbnailCache = std::make_unique<FContentBrowserThumbnailCache>();
		ViewMode = static_cast<EContentBrowserViewMode>(
			SessionSettings.GetContentBrowserViewMode());
		bIconSizeLocked = SessionSettings.IsContentBrowserIconSizeLocked();
		Model.SetShowSourceFiles(
			SessionSettings.GetContentBrowserShowSourceFiles());
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

	auto FContentBrowserPanel::RevealDirectory(
		std::string_view DirectoryPath) -> void
	{
		const PathUtilities::FContentPathResult Resolved =
			PathUtilities::ResolveContentPath(DirectoryPath);
		if (!Resolved)
		{
			SetError(Resolved.Message);
			return;
		}
		NavigateToPhysical(Resolved.PhysicalPath.generic_string());
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
		Model.RefreshMountSnapshot();
		if (bRescanRegistry)
		{
			const Asset::FAssetResult Result = Model.RescanRegistry();
			if (!Result) SetError(Result.Message);
		}
		if (!Model.GetCurrentPhysicalPath().empty()
			&& !std::filesystem::is_directory(Model.GetCurrentPhysicalPath()))
		{
			for (const FContentBrowserModel::FMountSnapshot& Mount :
				 Model.GetMounts())
				if (NavigateToPhysical(Mount.PhysicalRoot)) return;
		}
		RefreshItemsSnapshot();
	}

	auto FContentBrowserPanel::RefreshItemsSnapshot() -> void
	{
		ThumbnailCache->CancelPendingRequests();
		Model.RefreshItemsSnapshot();
		RepairSelection();
	}

	auto FContentBrowserPanel::RebuildItems() -> void
	{
		ThumbnailCache->CancelPendingRequests();
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
		Refresh(true);
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
		Refresh(true);
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
		Refresh(true);
		RevealAsset(Result.RevealAssetPath);
		if (OpenAsset
			&& !OpenAsset(Result.RevealAssetPath, Result.OpenAssetClassName))
			SetError(
				"The material was created, but its editor could not be opened.");
	}

	auto FContentBrowserPanel::ReimportAsset(
		const FContentBrowserItem& Item,
		AssetImport::EImportRecordAction Action) -> void
	{
		const bool bRecreateMissingAssets =
			Action != AssetImport::EImportRecordAction::Reimport;
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
		AssetImport::FImportRecordInspection Inspection =
			Cast<AssetImport::DImportRecord>(AssetObject)
				? AssetImport::InspectImportRecord(
					Path, AssetImport::GetImportRecordIndex())
				: AssetImport::InspectImportRecordForOutput(
					Path, AssetImport::GetImportRecordIndex());
		if (Inspection && Inspection.Record)
		{
			const AssetImport::FImportRecordActionResult Executed =
				AssetImport::ExecuteImportRecordAction(
					*Inspection.Record, Action,
					AssetImport::GetImportRecordHandlerRegistry());
			if (!Executed) { SetError(Executed.Message); return; }
			LastReimportOrphans = Executed.Orphans;
			Refresh(true);
			RevealAsset(Path.ToString());
			return;
		}
		const AssetImport::FSingleAssetCapabilitySet Capabilities =
			AssetImport::QuerySingleAssetCapabilities(
				*AssetObject, AssetImport::GetProviderRegistry(),
				AssetImport::GetSingleAssetHandlerRegistry());
		const AssetImport::FSingleAssetCapability* Reimport = Capabilities.Find(
			AssetImport::ESingleAssetImportCapability::ReimportCurrentSource);
		if (Reimport && Reimport->bAvailable && !bRecreateMissingAssets)
		{
			const AssetImport::FSingleAssetPlanResult Planned =
				AssetImport::CreateSingleAssetReimportPlan(
					{.Asset = AssetObject}, AssetImport::GetProviderRegistry(),
					AssetImport::GetSingleAssetHandlerRegistry());
			if (!Planned)
			{
				SetError(Planned.Message);
				return;
			}
			const AssetImport::FSingleAssetExecutionResult Executed =
				AssetImport::ExecuteSingleAssetImport(Planned.Plan);
			if (!Executed)
			{
				SetError(Executed.Message);
				return;
			}
			LastReimportOrphans.clear();
			Refresh(true);
			RevealAsset(Path.ToString());
			return;
		}

		SetError(Reimport && !Reimport->Diagnostics.empty()
			? Reimport->Diagnostics.back().Message
			: Inspection.Message.empty()
				? "The selected asset has no available reimport capability."
				: Inspection.Message);
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
		AnalyzeDeleteSelection();
		bDeletePopupRequested = true;
	}

	auto FContentBrowserPanel::AnalyzeDeleteSelection() -> void
	{
		Operations.AnalyzeDeletion(
			Model.GetItems(),
			Selection,
			DeleteAnalysis,
			DeleteAnalysisErrors);
	}

	auto FContentBrowserPanel::DeleteSelection() -> void
	{
		const Asset::FAssetResult Result =
			Operations.Delete(Model.GetItems(), Selection);
		if (!Result) SetError(Result.Message);
		Selection.clear();
		Refresh(true);
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
} // namespace Durin
