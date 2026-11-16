#include "Data/FileDeletionRequest.h"
#include "AssetTools/DeleteItemsHost.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/ContentFilesystem.h"
#include "Asset/Load.h"
#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/References.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::ContentStorage
{
	using Filesystem::NormalizePath;
	namespace
	{
		auto MakeFingerprint(
			const std::filesystem::path& Path,
			EFileDeletionEntryKind Kind,
			FFileDeletionFingerprint& OutFingerprint,
			std::error_code& OutError) -> bool
		{
			OutError.clear();
			OutFingerprint = {};
			OutFingerprint.PhysicalPath = NormalizePath(Path.generic_string());
			OutFingerprint.Kind = Kind;
			if (Kind != EFileDeletionEntryKind::Directory)
			{
				OutFingerprint.FileSize = std::filesystem::file_size(Path, OutError);
				if (OutError) return false;
			}
			const auto WriteTime = std::filesystem::last_write_time(Path, OutError);
			if (OutError) return false;
			OutFingerprint.LastWriteTimeTicks =
				static_cast<int64>(WriteTime.time_since_epoch().count());
			if (Kind != EFileDeletionEntryKind::Directory)
			{
				auto Identity = FFileHelper::HashFileXx128(Path);
				if (!Identity)
				{
					OutError = Identity.error().NativeError;
					return false;
				}
				OutFingerprint.ByteIdentity = *Identity;
				const uintmax_t FinalSize = std::filesystem::file_size(Path, OutError);
				if (OutError) return false;
				const auto FinalWriteTime =
					std::filesystem::last_write_time(Path, OutError);
				if (OutError) return false;
				if (FinalSize != OutFingerprint.FileSize
					|| static_cast<int64>(FinalWriteTime.time_since_epoch().count())
						!= OutFingerprint.LastWriteTimeTicks)
				{
					OutError = std::make_error_code(std::errc::state_not_recoverable);
					return false;
				}
			}
			return true;
		}

	}
	auto FFileDeletionRequest::AnalyzeDeletion(
		std::span<const FFileDeletionInput> Items,
		FAssetDeletionOperation& OutOperation, ObjectTools::EAllowCancelDuringDelete AllowCancel)
		-> FFileDeletionPlanPtr
	{
		auto Plan = std::make_shared<FFileDeletionPlan>();

		auto AddBlocker = [&](EFileDeletionBlocker Kind,
			std::string DisplayName,
			std::string PhysicalPath,
			std::string RelatedAssetPath,
			std::string Details) {
			Plan->Blockers.push_back({
				.Kind = Kind,
				.DisplayName = std::move(DisplayName),
				.PhysicalPath = std::move(PhysicalPath),
				.RelatedAssetPath = std::move(RelatedAssetPath),
				.Details = std::move(Details)});
		};

		if (Items.empty())
		{
			AddBlocker(
				EFileDeletionBlocker::InvalidSelection,
				"Selection", {}, {}, "No content is selected.");
			return Plan;
		}

		struct FSelectedRoot
		{
			const FFileDeletionInput* Item = nullptr;
			std::string PhysicalPath;
			const FMountPoint* Mount = nullptr;
		};
		std::vector<FSelectedRoot> SelectedRoots;
		for (const FFileDeletionInput& Item : Items)
		{
			const std::string PhysicalPath = NormalizePath(Item.PhysicalPath);
			const FContentMountPaths::FMountPath Resolved =
				FContentMountPaths::ResolveMountPath(PhysicalPath);
			if (!Resolved)
			{
				AddBlocker(
					EFileDeletionBlocker::OutsideMount,
					Item.Name,
					PhysicalPath,
					{},
					"Selected path is outside every mounted content root.");
				continue;
			}
			const FMountPoint* Mount = Resolved.Mount;
			if (AreSamePath(PhysicalPath, Mount->GetContentDir().generic_string()))
				AddBlocker(
					EFileDeletionBlocker::MountRoot,
					Item.Name,
					PhysicalPath,
					{},
					"A mounted content root cannot be deleted.");
			if (!Mount->bContentWritable)
				AddBlocker(
					EFileDeletionBlocker::ReadOnlyMount,
					Item.Name,
					PhysicalPath,
					{},
					"The selected mount is not content-writable.");
			SelectedRoots.push_back({&Item, PhysicalPath, Mount});
		}

		if (SelectedRoots.size() != Items.size())
			AddBlocker(
				EFileDeletionBlocker::InvalidSelection,
				"Selection",
				{},
				{},
				"One or more selected items are absent from the analyzed item set.");
		std::ranges::sort(
			SelectedRoots,
			[](const FSelectedRoot& A, const FSelectedRoot& B) {
				if (A.PhysicalPath.size() != B.PhysicalPath.size())
					return A.PhysicalPath.size() < B.PhysicalPath.size();
				return A.PhysicalPath < B.PhysicalPath;
			});
		std::vector<FSelectedRoot> MaximalRoots;
		for (const FSelectedRoot& Candidate : SelectedRoots)
		{
			if (!MaximalRoots.empty()
				&& Candidate.Mount != MaximalRoots.front().Mount)
				AddBlocker(
					EFileDeletionBlocker::UnsupportedMount,
					Candidate.Item->Name,
					Candidate.PhysicalPath,
					{},
					"One deletion plan cannot span multiple content mounts.");
			if (std::ranges::any_of(
					MaximalRoots,
					[&](const FSelectedRoot& Existing) {
						return FPaths::IsLexicalDescendantPath(
							Candidate.PhysicalPath, Existing.PhysicalPath, true);
					}))
				continue;
			MaximalRoots.push_back(Candidate);
		}
		if (MaximalRoots.size() == 1)
			Plan->DisplayName = MaximalRoots.front().Item->Name;
		else
			Plan->DisplayName = std::format("{} Items", MaximalRoots.size());

		const FAssetCatalogSnapshot Catalog =
			CaptureAssetCatalogSnapshot();
		std::unordered_map<std::string, const FPackageData*> AssetsByPhysicalPath;
		for (const auto& [Path, Data] : Catalog.Assets)
			AssetsByPhysicalPath.emplace(NormalizePath(Data.PhysicalPath), &Data);
		std::vector<FPackagePath> AssetPaths;
		std::vector<std::filesystem::path> PhysicalRoots;

		auto AddPhysicalEntry = [&](const std::filesystem::path& Physical,
			bool bDirectory) {
			const std::string Normalized = NormalizePath(Physical.generic_string());
			EFileDeletionEntryKind Kind = bDirectory
				? EFileDeletionEntryKind::Directory
				: EFileDeletionEntryKind::OrdinaryFile;
			if (!bDirectory && Physical.extension() == ".dasset")
			{
				if (const auto Asset = AssetsByPhysicalPath.find(Normalized);
					Asset != AssetsByPhysicalPath.end())
				{
					Kind = EFileDeletionEntryKind::AssetPackage;
					AssetPaths.push_back(Asset->second->PackagePath);
				}
				else
				{
					Kind = EFileDeletionEntryKind::UnknownPackage;
					AddBlocker(
						EFileDeletionBlocker::UnknownPackage,
						Physical.filename().generic_string(),
						Normalized,
						{},
						"Package file is not registered or has invalid metadata.");
				}
			}
			FFileDeletionFingerprint Fingerprint;
			std::error_code Ec;
			if (!MakeFingerprint(Physical, Kind, Fingerprint, Ec))
			{
				AddBlocker(
					EFileDeletionBlocker::InspectionFailed,
					Physical.filename().generic_string(),
					Normalized,
					{},
					std::format("Could not fingerprint content: {}", Ec.message()));
				return;
			}
			Plan->Entries.push_back(std::move(Fingerprint));
		};

		for (const FSelectedRoot& Root : MaximalRoots)
		{
			const std::filesystem::path Physical(Root.PhysicalPath);
			PhysicalRoots.push_back(Physical);
			std::error_code Ec;
			const bool bReparse = IsReparsePoint(Physical, Ec);
			if (Ec)
			{
				AddBlocker(
					EFileDeletionBlocker::InspectionFailed,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					std::format("Could not inspect selected path: {}", Ec.message()));
				continue;
			}
			if (bReparse)
			{
				AddBlocker(
					EFileDeletionBlocker::ReparsePoint,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					"Reparse points cannot be traversed or staged.");
				continue;
			}
			const bool bDirectory = std::filesystem::is_directory(Physical, Ec);
			if (Ec || (!bDirectory && !std::filesystem::is_regular_file(Physical, Ec)))
			{
				AddBlocker(
					EFileDeletionBlocker::InvalidSelection,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					"Selected content no longer exists as a supported file or directory.");
				continue;
			}
			AddPhysicalEntry(Physical, bDirectory);
			Plan->MaximalRoots.push_back({
				.OriginalPath = Root.PhysicalPath});
			if (!bDirectory) continue;

			for (std::filesystem::recursive_directory_iterator It(
					 Physical, std::filesystem::directory_options::none, Ec),
				 End;
				 !Ec && It != End;
				 It.increment(Ec))
			{
				std::error_code EntryEc;
				if (IsReparsePoint(It->path(), EntryEc))
				{
					AddBlocker(
						EFileDeletionBlocker::ReparsePoint,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						"Reparse points cannot be traversed or staged.");
					It.disable_recursion_pending();
					continue;
				}
				if (EntryEc)
				{
					AddBlocker(
						EFileDeletionBlocker::InspectionFailed,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						std::format("Could not inspect descendant: {}", EntryEc.message()));
					It.disable_recursion_pending();
					continue;
				}
				const bool bChildDirectory = It->is_directory(EntryEc);
				if (!EntryEc && (bChildDirectory || It->is_regular_file(EntryEc)))
					AddPhysicalEntry(It->path(), bChildDirectory);
				else if (EntryEc)
					AddBlocker(
						EFileDeletionBlocker::InspectionFailed,
						It->path().filename().generic_string(),
						NormalizePath(It->path().generic_string()),
						{},
						std::format("Could not classify descendant: {}", EntryEc.message()));
			}
			if (Ec)
				AddBlocker(
					EFileDeletionBlocker::InspectionFailed,
					Root.Item->Name,
					Root.PhysicalPath,
					{},
					std::format("Could not enumerate folder contents: {}", Ec.message()));
		}

		if (!DeleteAssets) AssetPaths.clear();
		if (!DeleteAssets || !DeleteFiles)
		{
			PhysicalRoots.clear();
			for (auto& Entry : Plan->Entries)
			{
				if (Entry.Kind == EFileDeletionEntryKind::OrdinaryFile)
				{
					FAssetCompanionOwnership Ownership;
					const bool Inspected = QueryAssetCompanionOwnership(Entry.PhysicalPath, Ownership);
					if (!Inspected || Ownership.State == EAssetCompanionOwnershipState::Ambiguous)
						AddBlocker(EFileDeletionBlocker::CompanionOwnershipConflict, Entry.PhysicalPath, Entry.PhysicalPath, {}, "Companion ownership cannot be established.");
					if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed) Entry.Kind = EFileDeletionEntryKind::ManagedCompanion;
				}
				if ((DeleteAssets && Entry.Kind == EFileDeletionEntryKind::AssetPackage) || (DeleteFiles && Entry.Kind == EFileDeletionEntryKind::OrdinaryFile)) PhysicalRoots.emplace_back(Entry.PhysicalPath);
			}
		}
		std::ranges::sort(AssetPaths, [](const FPackagePath& A, const FPackagePath& B) {
			return A.GetView() < B.GetView();
		});
		AssetPaths.erase(std::unique(AssetPaths.begin(), AssetPaths.end()), AssetPaths.end());
		std::string AdmissionError;
		FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) { if (Level >= ELogLevel::Error) AdmissionError += Message; });
		FAssetDeletionRequest Request{.AssetPaths = AssetPaths, .PhysicalRoots = PhysicalRoots};
		bool bPrepared = true;
		Model = std::make_shared<FAssetDeleteModel>(std::span<DObject* const>{});
		for (const auto& Path : Request.AssetPaths)
		{
			const auto Entry = FindPackageExact(Path);
			if (!Entry) { bPrepared = false; continue; }
			for (const auto& Asset : Entry->TopLevelAssets)
			{
				auto* Package = FindResidentPackage(Asset.AssetPath.GetPackagePath());
				if (!Package) Package = LoadPackage(Asset.AssetPath.GetPackagePath());
				auto* Object = Package ? Package->FindTopLevelAsset(FName(Asset.AssetPath.GetAssetName())) : nullptr;
				if (Object) Model->AddObjectToDelete(Object); else bPrepared = false;
			}
		}
		if (!Model->GetPendingDeletedAssets().empty())
		{
			if (!ObjectTools::Private::ScanDeleteModel(*Model, AllowCancel))
			{
				AddBlocker(EFileDeletionBlocker::InspectionFailed, "Assets", {}, {}, "Deletion canceled while analyzing assets.");
				return Plan;
			}
			bPrepared &= Model->CanDelete();
			if (!bPrepared) AdmissionError += Model->GetProgressText();
			OutOperation = Model->TakeWholePackageOperation();
		}
		else bPrepared &= PrepareAssetDeletionWithEditorPolicy(Request, OutOperation, &Feedback);
		if (!bPrepared && OutOperation.GetBlockers().empty())
			AddBlocker(EFileDeletionBlocker::InspectionFailed, "Assets", {}, {}, AdmissionError);

		std::unordered_set<std::string> CompanionPaths;
		for (const FAssetDeletionEntry& Entry : OutOperation.GetEntries())
			for (const std::filesystem::path& Companion : Entry.CompanionFiles)
				CompanionPaths.insert(NormalizePath(Companion.generic_string()));
		for (FFileDeletionFingerprint& Entry : Plan->Entries)
			if (CompanionPaths.contains(Entry.PhysicalPath)
				&& Entry.Kind == EFileDeletionEntryKind::OrdinaryFile)
				Entry.Kind = EFileDeletionEntryKind::ManagedCompanion;

		for (const std::string& CompanionPath : CompanionPaths)
		{
			if (std::ranges::any_of(
					Plan->Entries,
					[&](const FFileDeletionFingerprint& Entry) {
						return Entry.PhysicalPath == CompanionPath;
					}))
				continue;
			std::error_code Ec;
			if (!std::filesystem::is_regular_file(CompanionPath, Ec))
			{
				if (Ec
					&& Ec != std::errc::no_such_file_or_directory
					&& Ec != std::errc::not_a_directory)
					AddBlocker(
						EFileDeletionBlocker::InspectionFailed,
						std::filesystem::path(CompanionPath).filename().generic_string(),
						CompanionPath,
						{},
						std::format("Could not classify asset companion: {}", Ec.message()));
				continue;
			}

			const FMountPoint* SelectedMount =
				MaximalRoots.empty() ? nullptr : MaximalRoots.front().Mount;
			std::filesystem::path SelectedReparsePoint;
			if (SelectedMount
				&& FPaths::IsLexicalDescendantPath(
					CompanionPath, SelectedMount->GetContentDir().generic_string(), true)
				&& FindReparsePointInPath(
					SelectedMount->GetContentDir().generic_string(),
					CompanionPath,
					SelectedReparsePoint,
					Ec))
			{
				AddBlocker(
					EFileDeletionBlocker::ReparsePoint,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format(
						"Asset companion path traverses reparse point {}.",
						SelectedReparsePoint.generic_string()));
				continue;
			}
			if (Ec)
			{
				AddBlocker(
					EFileDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not inspect asset companion: {}", Ec.message()));
				continue;
			}

			const FContentMountPaths::FMountPath Resolved =
				FContentMountPaths::ResolveMountPath(CompanionPath);
			if (!Resolved)
			{
				AddBlocker(
					EFileDeletionBlocker::OutsideMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"Asset companion is outside every mounted content root.");
				continue;
			}
			const FMountPoint* CompanionMount = Resolved.Mount;
			bool bCompanionRootSafe = true;
			if (!SelectedMount || CompanionMount != SelectedMount)
			{
				AddBlocker(
					EFileDeletionBlocker::UnsupportedMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"An external asset companion must belong to the selected content mount.");
				bCompanionRootSafe = false;
			}
			if (!CompanionMount->bContentWritable)
			{
				AddBlocker(
					EFileDeletionBlocker::ReadOnlyMount,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					"The asset companion's content mount is not content-writable.");
				bCompanionRootSafe = false;
			}
			std::filesystem::path ReparsePoint;
			const bool bReparse = FindReparsePointInPath(
				CompanionMount->GetContentDir().generic_string(), CompanionPath, ReparsePoint, Ec);
			if (Ec)
			{
				AddBlocker(
					EFileDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not inspect asset companion: {}", Ec.message()));
				continue;
			}
			if (bReparse)
			{
				AddBlocker(
					EFileDeletionBlocker::ReparsePoint,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format(
						"Asset companion path traverses reparse point {}.",
						ReparsePoint.generic_string()));
				bCompanionRootSafe = false;
			}
			if (!bCompanionRootSafe) continue;
			FFileDeletionFingerprint Fingerprint;
			if (!MakeFingerprint(
					CompanionPath,
					EFileDeletionEntryKind::ManagedCompanion,
					Fingerprint,
					Ec))
			{
				AddBlocker(
					EFileDeletionBlocker::InspectionFailed,
					std::filesystem::path(CompanionPath).filename().generic_string(),
					CompanionPath,
					{},
					std::format("Could not fingerprint companion: {}", Ec.message()));
				continue;
			}
			Plan->Entries.push_back(Fingerprint);
			Plan->MaximalRoots.push_back({
				.OriginalPath = CompanionPath});
		}

		for (const FAssetDeletionBlocker& Blocker : OutOperation.GetBlockers())
		{
			EFileDeletionBlocker Kind = EFileDeletionBlocker::InspectionFailed;
			switch (Blocker.Kind)
			{
			case EAssetDeletionBlocker::ExternalPersistentReference:
			case EAssetDeletionBlocker::ExternalLoadedReference:
				Kind = EFileDeletionBlocker::ExternalReference;
				break;
			case EAssetDeletionBlocker::RedirectorTargetNotSelected:
				Kind = EFileDeletionBlocker::RedirectorTargetNotSelected;
				break;
			case EAssetDeletionBlocker::TargetRedirectorsNotSelected:
				Kind = EFileDeletionBlocker::TargetRedirectorsNotSelected;
				break;
			case EAssetDeletionBlocker::LoadingPackage:
				Kind = EFileDeletionBlocker::LoadingPackage;
				break;
			case EAssetDeletionBlocker::DirtyPackage:
				Kind = EFileDeletionBlocker::DirtyPackage;
				break;
			case EAssetDeletionBlocker::ReferenceStoreInspectionFailed:
				Kind = EFileDeletionBlocker::ReferenceStoreInspectionFailed;
				break;
			case EAssetDeletionBlocker::CompanionInspectionFailed:
				Kind = EFileDeletionBlocker::CompanionInspectionFailed;
				break;
			case EAssetDeletionBlocker::CompanionOwnershipConflict:
				Kind = EFileDeletionBlocker::CompanionOwnershipConflict;
				break;
			case EAssetDeletionBlocker::ExternalCompanionOwner:
				Kind = EFileDeletionBlocker::ExternalCompanionOwner;
				break;
			default:
				break;
			}
			AddBlocker(
				Kind,
				Blocker.AssetPath.ToString(),
				NormalizePath(Blocker.PhysicalPath.generic_string()),
				Blocker.RelatedAssetPath.ToString(),
				Blocker.Details);
		}
		for (const FAssetDeletionWarning& Warning : OutOperation.GetWarnings())
			Plan->Warnings.push_back({
				.DisplayName = Warning.AssetPath.ToString(),
				.Details = Warning.Details});

		std::ranges::sort(
			Plan->Entries,
			{},
			&FFileDeletionFingerprint::PhysicalPath);
		for (const FFileDeletionFingerprint& Entry : Plan->Entries)
			switch (Entry.Kind)
			{
			case EFileDeletionEntryKind::Directory:
				++Plan->Summary.FolderCount;
				break;
			case EFileDeletionEntryKind::AssetPackage:
				if (DeleteAssets) ++Plan->Summary.AssetCount;
				break;
			case EFileDeletionEntryKind::ManagedCompanion:
				if (DeleteAssets) ++Plan->Summary.CompanionCount;
				break;
			case EFileDeletionEntryKind::OrdinaryFile:
			case EFileDeletionEntryKind::UnknownPackage:
				if (DeleteFiles) ++Plan->Summary.FileCount;
				break;
			}
		std::ranges::sort(
			Plan->Blockers,
			[](const FFileDeletionBlocker& A, const FFileDeletionBlocker& B) {
				return std::tie(A.PhysicalPath, A.RelatedAssetPath, A.Kind, A.Details)
					< std::tie(B.PhysicalPath, B.RelatedAssetPath, B.Kind, B.Details);
			});
		Plan->Blockers.erase(
			std::unique(Plan->Blockers.begin(), Plan->Blockers.end(),
				[](const FFileDeletionBlocker& A,
					const FFileDeletionBlocker& B) {
					return A.Kind == B.Kind
						&& A.PhysicalPath == B.PhysicalPath
						&& A.RelatedAssetPath == B.RelatedAssetPath
						&& A.Details == B.Details;
				}),
			Plan->Blockers.end());
		return Plan;
	}

	auto FFileDeletionRequest::DeleteObjectsAndFolders(const ObjectTools::FDeleteItemsParameters& Parameters) -> ObjectTools::FDeleteItemsResult
	{
		std::vector<FFileDeletionInput> Inputs;
		for (const auto& Folder : Parameters.FoldersToDelete)
		{
			const auto Mapped = FContentMountPaths::VirtualToPhysical(Folder);
			Inputs.push_back({std::filesystem::path(Folder).filename().generic_string(), Mapped.empty() ? Folder : Mapped});
		}
		FAssetDeleteModel Objects(Parameters.ObjectsToDelete);
		const auto Snapshot = CaptureAssetQuerySnapshot();
		for (const auto& [Identity, Asset] : Snapshot.Assets)
		{
			const auto Resolved = FMountPaths::ResolveAssetPath(Identity.GetPackagePath().GetView());
			if (!Resolved) continue;
			const auto Physical = Resolved->PhysicalPath.generic_string() + ".dasset";
			const bool Covered = std::ranges::any_of(Inputs, [&](const auto& Folder) {
				std::filesystem::path Relative;
				return FPaths::TryMakeLexicalRelativePath(Physical, Folder.PhysicalPath, Relative);
			});
			if (!Covered) continue;
			auto* Package = FindResidentPackage(Identity.GetPackagePath());
			if (!Package) Package = LoadPackage(Identity.GetPackagePath());
			auto* Object = Package ? Package->FindTopLevelAsset(FName(Identity.GetAssetName())) : nullptr;
			if (!Object) return ObjectTools::FDeleteItemsResult{};
			Objects.AddObjectToDelete(Object);
		}
		if (!ObjectTools::Private::ScanDeleteModel(Objects, Parameters.AllowCancelDuringDelete)) return {};
		const bool HasObjects = !Objects.GetPendingDeletedAssets().empty();
		if (HasObjects && !Objects.CanDelete()) return ObjectTools::FDeleteItemsResult{};
		// File retains the immutable ordinary-file scope while this one model
		// retires the selected assets. Only confirmed asset removals may differ.
		FFileDeletionRequest Files({}, false, true);
		auto Plan = Files.PrepareDeleteItems(Inputs);
		if (!Plan || !Plan->CanExecute()) return ObjectTools::FDeleteItemsResult{};
		std::vector<std::string> Paths;
		std::unordered_set<std::string> Folders;
		for (const auto& Entry : Plan->Entries)
		{
			Paths.push_back(Entry.PhysicalPath);
			if (Entry.Kind == EFileDeletionEntryKind::Directory) Folders.insert(Entry.PhysicalPath);
		}
		for (const auto& Warning : Plan->Warnings) Paths.push_back(Warning.Details);
		if (Parameters.bShowConfirmation && !ObjectTools::Private::ConfirmDelete(Objects, Paths, Parameters.AllowCancelDuringDelete)) return ObjectTools::FDeleteItemsResult{};
		if (!Files.ValidateForExecution()) return ObjectTools::FDeleteItemsResult{};
		if (HasObjects && !Objects.DoDelete()) return ObjectTools::FDeleteItemsResult{};
		const auto Effects = Files.DeleteItems(Plan);
		ObjectTools::FDeleteItemsResult Result{.ObjectsDeleted = Objects.GetDeletedObjectCount()};
		for (const auto& Effect : Effects.Effects)
			if (Effect.Status == EFileDeletionEffectStatus::Committed && Folders.contains(Effect.PhysicalPath)) ++Result.FoldersDeleted;
		return Result;
	}

	FFileDeletionRequest::FFileDeletionRequest(const FContentMutationPolicy& Host, bool InDeleteAssets, bool InDeleteFiles)
		: Policy(Host), Diagnostics(Host.ReportDiagnostic), DeleteAssets(InDeleteAssets), DeleteFiles(InDeleteFiles) {}
	auto FFileDeletionRequest::DeleteWithConfirmation(std::span<const FFileDeletionInput> Inputs, bool ShowConfirmation, ObjectTools::EAllowCancelDuringDelete AllowCancel) -> ObjectTools::FDeleteItemsResult
	{
		if (bPresenting) return {};
		bPresenting = true;
		struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bPresenting};
		auto Plan = PrepareDeleteItems(Inputs, AllowCancel);
		if (!Plan || !Plan->CanExecute())
		{
			if (Plan) for (const auto& Blocker : Plan->Blockers) Report(ELogLevel::Error, Blocker.Details);
			return {};
		}
		std::vector<std::string> Paths;
		for (const auto& Entry : Plan->Entries) Paths.push_back(Entry.PhysicalPath);
		for (const auto& Warning : Plan->Warnings) Paths.push_back(Warning.Details);
		if (ShowConfirmation && !ObjectTools::Private::ConfirmDelete(*Model, Paths, AllowCancel)) return {};
		std::unordered_set<std::string> Folders;
		for (const auto& Entry : Plan->Entries) if (Entry.Kind == EFileDeletionEntryKind::Directory) Folders.insert(Entry.PhysicalPath);
		const auto Effects = DeleteItems(Plan);
		bCommitted = Effects.HasCommittedEffects();
		ObjectTools::FDeleteItemsResult Result{.ObjectsDeleted = LastDeletedObjects};
		for (const auto& Effect : Effects.Effects)
			if (Effect.Status == EFileDeletionEffectStatus::Committed)
			{

				if (Folders.contains(Effect.PhysicalPath)) ++Result.FoldersDeleted;
			}
		return Result;
	}
	auto FFileDeletionRequest::CanModify(std::string* Error) const -> bool
	{
		if (Policy.CanMutateContent && !Policy.CanMutateContent()) { if (Error) *Error = "Content mutation is currently disabled."; return false; }
		return true;
	}
	auto FFileDeletionRequest::Report(ELogLevel Level, std::string_view Message) const -> void
	{
		const auto Notify = Diagnostics;
		if (Notify) Notify(Level, Message); else FFeedbackContext::Get().Serialize(Level, Message);
	}
	auto FFileDeletionRequest::NotifyItemsUpdated(FContentChangeBatch Changes) -> void
	{
		if (!Changes.bFullRefresh && Changes.Changes.empty()) return;
		if (Policy.NotifyContentMutation) Policy.NotifyContentMutation(std::move(Changes));
	}
	auto FFileDeletionRequest::PrepareDeleteItems(std::span<const FFileDeletionInput> Items, ObjectTools::EAllowCancelDuringDelete AllowCancel) -> FFileDeletionPlanPtr
	{
		if (bExecuting) return {};
		CurrentPlan.reset(); Execution.reset(); Model.reset(); CapturedRequest.clear();
		FAssetDeletionOperation AssetOperation;
		auto Plan = std::make_shared<FFileDeletionPlan>(*AnalyzeDeletion(Items, AssetOperation, AllowCancel));
		Plan->SessionId = ++NextSession;
		CurrentPlan = Plan;
		CapturedRequest.assign(Items.begin(), Items.end());
		Execution = std::shared_ptr<FFileDeletionOperation>(new FFileDeletionOperation(Plan, std::move(AssetOperation), DeleteAssets, DeleteFiles, Model));
		return Plan;
	}
	auto FFileDeletionRequest::DismissDeletion(FFileDeletionPlanPtr Plan) -> void
	{
		if (!bExecuting && Plan == CurrentPlan) { CurrentPlan.reset(); Execution.reset(); Model.reset(); CapturedRequest.clear(); }
	}
	auto FFileDeletionRequest::ValidateForExecution() -> FAssetWriteResult
	{
		if (!CanModify()) return {.Error = UnifiedError::FError{AssetWriteErrors::ReadOnlyMode, "Content mutation is currently disabled."}};
		if (!Execution) return {.Error = UnifiedError::FError{AssetWriteErrors::StaleData, "Deletion confirmation is unavailable."}};
		std::optional<UnifiedError::FError> Error;
		if (!Execution->ValidateScope(&Error)) return {.Error = std::move(Error)};
		Execution->AllowPeerScopeChanges();
		return {};
	}

	auto FFileDeletionRequest::DeleteItems(FFileDeletionPlanPtr& Confirmation, FFileDeletionHooks Hooks) -> FFileDeletionResult
	{
		FFileDeletionResult Result;
		if (bExecuting) { Result.bStopped = true; return Result; }
		bExecuting = true;
		struct FReset { bool& Flag; ~FReset() { Flag = false; } } Reset{bExecuting};
		auto Reject = [&](std::string Error) {
			if (Confirmation) for (const auto& Entry : Confirmation->Entries) Result.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, Error);
			Result.bStopped = true;
			Report(ELogLevel::Error, Error);
			return Result;
		};
		std::string Error;
		if (!CanModify(&Error)) return Reject(Error);
		if (!Confirmation || Confirmation != CurrentPlan || !Execution) return Reject("Deletion confirmation was retired.");
		if (!Confirmation->CanExecute()) return Reject("Deletion is blocked.");
		for (const auto& Root : Confirmation->MaximalRoots)
		{
			const auto Mount = FContentMountPaths::ResolveMountPath(Root.OriginalPath);
			if (!Mount || !Mount.Mount->bContentWritable || AreSamePath(Root.OriginalPath, Mount.Mount->GetContentDir().generic_string())) return Reject("Deletion mount policy changed.");
			std::filesystem::path Reparse;
			std::error_code Ec;
			auto Parent = std::filesystem::path(Root.OriginalPath).parent_path();
			while (!std::filesystem::exists(Parent, Ec) && !Ec && Parent != Parent.parent_path()) Parent = Parent.parent_path();
			if (Ec || FindReparsePointInPath(Mount.Mount->GetContentDir().generic_string(), Parent, Reparse, Ec) || Ec) return Reject("Deletion ancestor changed or cannot be inspected.");
		}
		const auto CallerAdmission = Hooks.CanExecuteEffect;
		Hooks.CanExecuteEffect = [this, CallerAdmission] { return CanModify() && (!CallerAdmission || CallerAdmission()) && CanModify(); };
		const bool Succeeded = Execution->Execute(std::move(Hooks));
		const auto Native = Execution->GetResult();
		LastDeletedObjects = Execution->HasStarted() && Model ? Model->GetDeletedObjectCount() : 0;
		if (!Succeeded && !Execution->HasStarted())
		{
			if ((!Native.Error || !(Native.Error->Is(AssetWriteErrors::StaleData) || Native.Error->Is({"AssetRegistry", "StaleData"}) || Native.Error->Is({"AssetLoad", "StaleData"})))) return Reject((Native.Error ? Native.Error->ToString() : std::string{}));
			const auto Request = CapturedRequest;
			bExecuting = false;
			Confirmation = PrepareDeleteItems(Request);
			Result.bStopped = true;
			for (const auto& Entry : Confirmation->Entries) Result.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, "Confirm the replacement deletion plan before retrying.");
			Report(ELogLevel::Warn, (Native.Error ? Native.Error->ToString() : std::string{}) + " Review and confirm the updated deletion plan.");
			return Result;
		}
		Result = Execution->GetEffects();
		FContentChangeBatch Changes;
		for (const auto& Effect : Result.Effects)
			if (Effect.Status == EFileDeletionEffectStatus::Committed)
			{
				const auto Entry = std::ranges::find(Confirmation->Entries, Effect.PhysicalPath, &FFileDeletionFingerprint::PhysicalPath);
				Changes.Changes.push_back({EContentChangeKind::Removed, Effect.PhysicalPath, {}, {}, {}, Entry != Confirmation->Entries.end() && Entry->Kind == EFileDeletionEntryKind::Directory});
			}
		Changes.bFullRefresh |= !Native;
		// Retire this attempt before feedback/notifications can prepare another one.
		CurrentPlan.reset(); Execution.reset(); Model.reset(); CapturedRequest.clear(); Confirmation.reset();
		bExecuting = false;
		for (const auto& Effect : Result.Effects)
		{
			if (Effect.Status == EFileDeletionEffectStatus::Failed && !Effect.Diagnostic.empty())
				Report(ELogLevel::Error, Effect.Diagnostic);
			for (const auto& Path : Effect.RecoveryLocations) Report(ELogLevel::Warn, "Recovery location: " + Path);
		}
		if (!Native && Native.Error && std::ranges::none_of(Result.Effects, [&](const auto& Effect) {
			return Effect.Status == EFileDeletionEffectStatus::Failed && Effect.Diagnostic == Native.Error->GetMessage();
		}))
			Report(Native.Effect == EAssetWriteEffect::ContentCommittedProjectionPending ? ELogLevel::Warn : ELogLevel::Error, Native.Error->ToString());
		NotifyItemsUpdated(std::move(Changes));
		return Result;
	}
}
