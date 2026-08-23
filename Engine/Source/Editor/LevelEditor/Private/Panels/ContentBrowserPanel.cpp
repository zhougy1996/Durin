#include "Panels/ContentBrowserPanel.h"
#include "Texture2DSourceTranslation.h"
#include "StaticMeshSourceTranslation.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Terrain/TerrainHeightmap.h"
#include "TextureCubeSourceTranslation.h"
#include "VolumeTextureSourceTranslation.h"
#include "TerrainHeightmapSourceTranslation.h"
#include "Panels/ContentBrowserFilesystem.h"

#include "AssetImportCore.h"
#include "ImportService.h"
#include "AssetTools.h"
#include "SceneImport.h"
#include "StaticMesh/StaticMesh.h"
#include "Assets/ContentBrowserThumbnailCache.h"
#include "Misc/Paths.h"
#include "Panels/ContentBrowserItemView.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Workspace/LevelEditorPresentationPolicy.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif

namespace Durin::Editor::Level
{
	namespace
	{
		auto ReadAssetClipboard(FAssetPath& OutPath) -> bool
		{
			OutPath = {};
			const char* Clipboard = ImGui::GetClipboardText();
			if (!Clipboard) return false;
			return FAssetPath::TryCreate(Clipboard, OutPath);
		}
	}

	auto FContentBrowserPanel::SaveAssetPackage(const FAssetPath& Path) -> void
	{
		DPackage* Package = Asset::FindResidentPackage(Path);
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
		FNotifyImportStarted InNotifyImportStarted,
		std::shared_ptr<FMountedContentReconciliationState>
			InMountedContentReconciliationState,
		FTaskScopeToken InThumbnailTaskScope)
		: ILevelEditorPanel(IsLevelEditorPanelOpenByDefault(
			ELevelEditorPanelRole::DrawerTool))
		, SessionSettings(InSessionSettings)
		, OpenAsset(std::move(InOpenAsset))
		, RequestImport(std::move(InRequestImport))
		, ExecuteTransaction(std::move(InExecuteTransaction))
		, GetMountedContentMutationRevision(
			std::move(InGetMountedContentMutationRevision))
		, NotifyMountedContentMutation(std::move(InNotifyMountedContentMutation))
		, NotifyImportStarted(std::move(InNotifyImportStarted))
		, RefreshCoordinator(
			GetMountedContentMutationRevision
				? GetMountedContentMutationRevision()
				: uint64{0},
			Asset::GetAssetCatalogRevision(),
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
				if (NavigateToPhysical(Mount.PhysicalRoot))
					break;
		}
	}

	FContentBrowserPanel::~FContentBrowserPanel()
	{
		if (PendingSingleAssetReimport && PendingSingleAssetReimport->Interchange)
			Asset::GetImportService().CancelAndDrainImportOperation(
				PendingSingleAssetReimport->Interchange->GetOperationHandle());
	}

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
					[] { return Asset::GetAssetCatalogRevision(); });
			if (!Result) SetError(Result.Message);
			return;
		}
		RefreshCoordinator.RefreshRegistryView(
			Asset::GetAssetCatalogRevision(),
			[this] { RefreshPublishedContent(); });
	}

	auto FContentBrowserPanel::RefreshPublishedContent() -> void
	{
		ThumbnailCache->CancelPendingRequests();
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
				Asset::ResolveAssetPath(Path);
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
		if (PendingSingleAssetReimport) return;
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
		if (PendingSingleAssetReimport) return false;
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
		if (!Result.Warning.empty()) SetWarning(Result.Warning);
		PublishMountedContentMutation();
		return true;
	}

	auto FContentBrowserPanel::DuplicateAsset(
		const FContentBrowserItem& Item) -> void
	{
		if (PendingSingleAssetReimport) return;
		const FContentBrowserOperationResult Result = Operations.Duplicate(Item);
		if (!Result)
		{
			SetError(Result.Status.Message);
			return;
		}
		PublishMountedContentMutation();
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
		if (PendingSingleAssetReimport) return;
		FAssetPath SourcePath;
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
		PublishMountedContentMutation();
		RevealAsset(Result.RevealAssetPath);
	}

	auto FContentBrowserPanel::HasAssetClipboard() const -> bool
	{
		FAssetPath SourcePath;
		if (!ReadAssetClipboard(SourcePath)) return false;
		const Asset::FAssetCatalogEntry Entry =
			Asset::FindAssetExact(SourcePath);
		return Entry
			&& Entry->EntryKind == Asset::EAssetRegistryEntryKind::Asset;
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
		Asset::EImportRecordAction Action) -> void
	{
		if (PendingSingleAssetReimport)
		{
			SetError("Another single-asset reimport is already active in this Content Browser.");
			return;
		}
		const bool bRecreateMissingAssets =
			Action != Asset::EImportRecordAction::Reimport;
		FAssetPath Path;
		if (!FAssetPath::TryCreate(Item.VirtualPath, Path))
		{
			SetError("The selected static-mesh asset path is invalid.");
			return;
		}
		DObject* AssetObject = nullptr;
		const Asset::FAssetResult Load =
			Asset::LoadAsset(Path, AssetObject);
		if (!Load || !AssetObject)
		{
			SetError(Load ? "The selected asset could not be loaded." : Load.Message);
			return;
		}
		Asset::FImportRecordInspection Inspection =
			Cast<Asset::DImportRecord>(AssetObject)
				? Asset::InspectImportRecord(
					Path, Asset::GetImportRecordIndex())
				: Asset::InspectImportRecordForOutput(
					Path, Asset::GetImportRecordIndex());
		if (Inspection && Inspection.Record)
		{
			Asset::FInterchangeImportRequest Request;
			std::string Error;
			if (!Asset::Forge::MakeSceneRecordInterchangeRequest(
				*Inspection.Record, Action,
				{.OwnerId = std::format("ContentBrowser.RecordReimport:{}", Path.ToString()),
					.ConflictIdentities = {Inspection.RecordPath.ToString()}},
				Request, Error))
			{
				SetError(std::move(Error));
				return;
			}
			Asset::FInterchangeImportHandle Handle =
				Asset::GetImportService().SubmitInterchangeImport(
					std::move(Request), "Reimport Scene graph");
			if (!Handle)
			{
				SetError("The Scene Interchange record action could not be submitted.");
				return;
			}
			LastReimportOrphans.clear();
			PendingSingleAssetReimport = FPendingSingleAssetReimport{
				.Interchange = std::move(Handle), .AssetPath = Path,
				.PreviousRecordOutputs = std::vector<Asset::FImportRecordOutput>(
					Inspection.Record->GetOutputs().begin(), Inspection.Record->GetOutputs().end())};
			if (NotifyImportStarted)
				NotifyImportStarted(PendingSingleAssetReimport->Interchange->GetOperationHandle(),
					"Reimport Scene graph");
			return;
		}
		if (auto* Texture = Cast<DTexture2D>(AssetObject);
			Texture && !bRecreateMissingAssets)
		{
			Asset::FInterchangeProvenance Existing;
			std::string Error;
			if (!Asset::Forge::InspectTexture2DInterchangeProvenance(
				*Texture, Existing, Error))
			{
				SetError(std::move(Error));
				return;
			}
			const FTextureSourceDiagnostic Source = Texture->InspectSource();
			if (Source.Status != ETextureSourceStatus::Available)
			{
				SetError(Source.Message.empty()
					? "The Texture2D source is unavailable." : Source.Message);
				return;
			}
			FTexture2DImportSettings Settings{
				.Usage = Texture->GetUsage(),
				.CompressionQuality = Texture->GetCompressionQuality(),
				.AlphaMipMode = Texture->GetAlphaMipMode(),
				.AlphaCoverageThreshold = Texture->GetAlphaCoverageThreshold(),
				.MaxResolution = Texture->GetMaxResolution(),
				.bSRGB = Texture->IsSRGB()};
			Asset::FInterchangeImportRequest Request;
			if (!Asset::Forge::MakeTexture2DInterchangeRequest(
				Texture->GetSourceImportData().Source.SourcePath, Path, Settings,
				Asset::EInterchangeImportMode::Reimport,
				{.OwnerId = std::format("ContentBrowser.Reimport:{}", Path.ToString()),
					.ConflictIdentities = {Path.ToString()}},
				std::move(Existing), Request, Error))
			{
				SetError(std::move(Error));
				return;
			}
			Asset::FInterchangeImportHandle Handle =
				Asset::GetImportService().SubmitInterchangeImport(
					std::move(Request), std::format("Reimport {}", Path.GetAssetName()));
			if (!Handle)
			{
				SetError("The Texture2D Interchange reimport could not be submitted.");
				return;
			}
			LastReimportOrphans.clear();
			if (NotifyImportStarted)
				NotifyImportStarted(Handle.GetOperationHandle(),
					std::format("Reimport {}", Path.GetAssetName()));
			PendingSingleAssetReimport = FPendingSingleAssetReimport{
				.Interchange = std::move(Handle), .AssetPath = Path};
			return;
		}
		if (auto* Mesh = Cast<DStaticMesh>(AssetObject);
			Mesh && !bRecreateMissingAssets)
		{
			Asset::FInterchangeProvenance Existing;
			std::string Error;
			if (!Asset::Forge::InspectStaticMeshInterchangeProvenance(
				*Mesh, Existing, Error))
			{
				SetError(std::move(Error));
				return;
			}
			const FStaticMeshSourceDiagnostic Source =
				Asset::Forge::InspectStaticMeshSource(*Mesh);
			if (Source.Status != EStaticMeshSourceStatus::Available)
			{
				SetError(Source.Message.empty()
					? "The StaticMesh source is unavailable." : Source.Message);
				return;
			}
			Asset::FInterchangeImportRequest Request;
			if (!Asset::Forge::MakeStaticMeshInterchangeRequest(
				Mesh->GetSourceImportData().SourcePath, Path, Mesh->GetImportSettings(),
				Asset::EInterchangeImportMode::Reimport,
				{.OwnerId = std::format("ContentBrowser.Reimport:{}", Path.ToString()),
					.ConflictIdentities = {Path.ToString()}},
				std::move(Existing), Request, Error))
			{
				SetError(std::move(Error));
				return;
			}
			Asset::FInterchangeImportHandle Handle =
				Asset::GetImportService().SubmitInterchangeImport(
					std::move(Request), std::format("Reimport {}", Path.GetAssetName()));
			if (!Handle)
			{
				SetError("The StaticMesh Interchange reimport could not be submitted.");
				return;
			}
			LastReimportOrphans.clear();
			if (NotifyImportStarted)
				NotifyImportStarted(Handle.GetOperationHandle(),
					std::format("Reimport {}", Path.GetAssetName()));
			PendingSingleAssetReimport = FPendingSingleAssetReimport{
				.Interchange = std::move(Handle), .AssetPath = Path};
			return;
		}
		auto SubmitInterchange = [&](Asset::FInterchangeImportRequest Request,
			std::string_view Family) -> bool {
			Asset::FInterchangeImportHandle Handle =
				Asset::GetImportService().SubmitInterchangeImport(
					std::move(Request), std::format("Reimport {}", Path.GetAssetName()));
			if (!Handle)
			{
				SetError(std::format("The {} Interchange reimport could not be submitted.", Family));
				return false;
			}
			LastReimportOrphans.clear();
			if (NotifyImportStarted)
				NotifyImportStarted(Handle.GetOperationHandle(),
					std::format("Reimport {}", Path.GetAssetName()));
			PendingSingleAssetReimport = FPendingSingleAssetReimport{
				.Interchange = std::move(Handle), .AssetPath = Path};
			return true;
		};
		if (auto* Cube = Cast<DTextureCube>(AssetObject);
			Cube && !bRecreateMissingAssets)
		{
			Asset::FInterchangeProvenance Existing;
			std::string Error;
			if (!Asset::Forge::InspectTextureCubeInterchangeProvenance(*Cube, Existing, Error))
			{
				SetError(std::move(Error));
				return;
			}
			std::array<FSourcePath, TextureCubeFaceCount> Sources;
			const size_t SourceCount = Cube->GetSourceLayout()
				== ETextureCubeSourceLayout::SixFaces ? TextureCubeFaceCount : 1;
			if (SourceCount == 1) Sources[0] = Cube->GetSourceImportData().Panorama.SourcePath;
			else for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
				Sources[Index] = Cube->GetSourceImportData().GetFace(
					static_cast<ETextureCubeFace>(Index)).SourcePath;
			Asset::FInterchangeImportRequest Request;
			if (!Asset::Forge::MakeTextureCubeInterchangeRequest(
				std::span(Sources).first(SourceCount), Cube->GetSourceLayout(), Path,
				{.bSRGB = Cube->IsSRGB()},
				{.FaceDimension = Cube->GetPanoramaFaceDimension(),
					.ExposureEV = Cube->GetPanoramaExposureEV()},
				Asset::EInterchangeImportMode::Reimport,
				{.OwnerId = std::format("ContentBrowser.Reimport:{}", Path.ToString()),
					.ConflictIdentities = {Path.ToString()}},
				std::move(Existing), Request, Error))
			{
				SetError(std::move(Error));
				return;
			}
			(void)SubmitInterchange(std::move(Request), "TextureCube");
			return;
		}
		if (auto* Volume = Cast<DVolumeTexture>(AssetObject);
			Volume && !bRecreateMissingAssets)
		{
			Asset::FInterchangeProvenance Existing;
			std::string Error;
			if (!Asset::Forge::InspectVolumeTextureInterchangeProvenance(*Volume, Existing, Error))
			{
				SetError(std::move(Error));
				return;
			}
			const FVolumeTextureSourceImportData& Source = Volume->GetSourceImportData();
			Asset::Forge::FVolumeTextureImportSettings Settings{
				.ImportFormat = Source.ImportFormat, .Channels = Source.Channels,
				.SliceWidth = Source.SliceWidth, .SliceHeight = Source.SliceHeight,
				.Depth = Source.Depth, .TilesX = Source.TilesX, .TilesY = Source.TilesY};
			Asset::FInterchangeImportRequest Request;
			if (!Asset::Forge::MakeVolumeTextureInterchangeRequest(Source.Source.SourcePath,
				Path, Settings, Asset::EInterchangeImportMode::Reimport,
				{.OwnerId = std::format("ContentBrowser.Reimport:{}", Path.ToString()),
					.ConflictIdentities = {Path.ToString()}},
				std::move(Existing), Request, Error))
			{
				SetError(std::move(Error));
				return;
			}
			(void)SubmitInterchange(std::move(Request), "VolumeTexture");
			return;
		}
		if (auto* Terrain = Cast<DTerrainHeightmap>(AssetObject);
			Terrain && !bRecreateMissingAssets)
		{
			Asset::FInterchangeProvenance Existing;
			Asset::FInterchangeImportRequest Request;
			std::string Error;
			if (!Asset::Forge::InspectTerrainHeightmapInterchangeProvenance(*Terrain, Existing, Error)
				|| !Asset::Forge::MakeTerrainHeightmapInterchangeRequest(
					Terrain->GetSourceImportData().SourcePath, Path,
					Asset::EInterchangeImportMode::Reimport,
					{.OwnerId = std::format("ContentBrowser.Reimport:{}", Path.ToString()),
						.ConflictIdentities = {Path.ToString()}},
					std::move(Existing), Request, Error))
			{
				SetError(std::move(Error));
				return;
			}
			(void)SubmitInterchange(std::move(Request), "TerrainHeightmap");
			return;
		}
		SetError(Inspection.Message.empty()
			? "The selected asset has no Interchange reimport capability."
			: Inspection.Message);
	}

	auto FContentBrowserPanel::PollSingleAssetReimport() -> void
	{
		if (!PendingSingleAssetReimport || !PendingSingleAssetReimport->Interchange) return;
		Asset::FInterchangeImportResult Result;
		if (!PendingSingleAssetReimport->Interchange->TryGetResult(Result)) return;
		const FAssetPath AssetPath = PendingSingleAssetReimport->AssetPath;
		const std::vector<Asset::FImportRecordOutput> PreviousOutputs =
			std::move(PendingSingleAssetReimport->PreviousRecordOutputs);
		PendingSingleAssetReimport.reset();
		if (Result.Outcome.State != Asset::EImportOperationState::Succeeded)
		{
			if (Result.Outcome.State != Asset::EImportOperationState::Canceled)
				SetError(Result.Outcome.Diagnostic.empty()
					? "Interchange reimport failed." : Result.Outcome.Diagnostic);
			return;
		}
		for (const Asset::FImportRecordOutput& Previous : PreviousOutputs)
			if (std::ranges::none_of(Result.Provenance.OutputMappings,
				[&](const Asset::FInterchangeOutputMapping& Mapping) {
					return Mapping.OutputIdentity == Previous.StableIdentity; }))
				LastReimportOrphans.push_back(Previous.AssetPath);
		PublishMountedContentMutation();
		RevealAsset(AssetPath.ToString());
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
			ContentBrowserFilesystem::NormalizePath(PhysicalDirectory);
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
		if (PendingSingleAssetReimport) return;
		if (Selection.empty()) return;
		PendingDeletionPlan = Operations.BuildDeletionPlan(
			Model.GetItems(), Selection);
		bDeletionPlanRefreshed = false;
		bDeletePopupRequested = true;
	}

	auto FContentBrowserPanel::DeleteSelection() -> void
	{
		if (PendingSingleAssetReimport) return;
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
			Asset::GetAssetCatalogRevision(),
			[this] { return Model.RescanRegistry(); },
			[this] { RefreshPublishedContent(); },
			[] { return Asset::GetAssetCatalogRevision(); });
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
} // namespace Durin::Editor::Level
