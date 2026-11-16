#include "AssetTools/FileDeletionState.h"
#include "Misc/FileError.h"
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

namespace Durin::AssetToolsPrivate
{
	using namespace ContentStorage;
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
	namespace
	{
		auto Reject(std::optional<UnifiedError::FError>* OutError, UnifiedError::FError Error) -> bool
		{ if (OutError) *OutError = std::move(Error); return false; }
		auto Normalize(const std::filesystem::path& Path) -> std::filesystem::path
		{
			return std::filesystem::absolute(Path).lexically_normal();
		}

		auto DefaultRemoveEntry(const std::filesystem::path& Path) -> std::error_code
		{
			std::error_code Error;
			std::filesystem::remove(Path, Error);
			return Error;
		}

	}

	FFileDeletionState::FFileDeletionState(const FContentMutationPolicy& InPolicy, bool Assets, bool Files)
		: Policy(InPolicy), DeleteAssets(Assets), DeleteFiles(Files) {}
	auto FFileDeletionState::Report(ELogLevel Level, std::string_view Message) const -> void
	{
		const auto Sink = Policy.ReportDiagnostic;
		if (Sink) Sink(Level, Message); else FFeedbackContext::Get().Serialize(Level, Message);
	}
	auto FFileDeletionState::Analyze(std::span<const FFileDeletionInput> Items, FAssetDeleteModel& Model, ObjectTools::EAllowCancelDuringDelete AllowCancel) -> void
	{
		auto& OutOperation = AssetOperation;

		auto AddBlocker = [&](EFileDeletionBlocker Kind,
			std::string DisplayName,
			std::string PhysicalPath,
			std::string RelatedAssetPath,
			std::string Details) {
			Snapshot.Blockers.push_back({
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
			return;
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
			Snapshot.DisplayName = MaximalRoots.front().Item->Name;
		else
			Snapshot.DisplayName = std::format("{} Items", MaximalRoots.size());

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
			Snapshot.Entries.push_back(std::move(Fingerprint));
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
			Snapshot.MaximalRoots.push_back({
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
			for (auto& Entry : Snapshot.Entries)
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
		for (const auto& Path : Request.AssetPaths)
		{
			const auto Entry = FindPackageExact(Path);
			if (!Entry) { bPrepared = false; continue; }
			for (const auto& Asset : Entry->TopLevelAssets)
			{
				auto* Package = FindResidentPackage(Asset.AssetPath.GetPackagePath());
				if (!Package) Package = LoadPackage(Asset.AssetPath.GetPackagePath());
				auto* Object = Package ? Package->FindTopLevelAsset(FName(Asset.AssetPath.GetAssetName())) : nullptr;
				if (Object) Model.AddObjectToDelete(Object); else bPrepared = false;
			}
		}
		if (!Model.GetPendingDeletedAssets().empty())
		{
			if (!ObjectTools::Private::ScanDeleteModel(Model, AllowCancel))
			{
				AddBlocker(EFileDeletionBlocker::InspectionFailed, "Assets", {}, {}, "Deletion canceled while analyzing assets.");
				return;
			}
			bPrepared &= Model.CanDelete();
			if (!bPrepared) AdmissionError += Model.GetProgressText();
			OutOperation = Model.TakeWholePackageOperation();
		}
		else bPrepared &= PrepareAssetDeletionWithEditorPolicy(Request, OutOperation, &Feedback);
		if (!bPrepared && OutOperation.GetBlockers().empty())
			AddBlocker(EFileDeletionBlocker::InspectionFailed, "Assets", {}, {}, AdmissionError);

		std::unordered_set<std::string> CompanionPaths;
		for (const FAssetDeletionEntry& Entry : OutOperation.GetEntries())
			for (const std::filesystem::path& Companion : Entry.CompanionFiles)
				CompanionPaths.insert(NormalizePath(Companion.generic_string()));
		for (FFileDeletionFingerprint& Entry : Snapshot.Entries)
			if (CompanionPaths.contains(Entry.PhysicalPath)
				&& Entry.Kind == EFileDeletionEntryKind::OrdinaryFile)
				Entry.Kind = EFileDeletionEntryKind::ManagedCompanion;

		for (const std::string& CompanionPath : CompanionPaths)
		{
			if (std::ranges::any_of(
					Snapshot.Entries,
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
			Snapshot.Entries.push_back(Fingerprint);
			Snapshot.MaximalRoots.push_back({
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
			Snapshot.Warnings.push_back({
				.DisplayName = Warning.AssetPath.ToString(),
				.Details = Warning.Details});

		std::ranges::sort(
			Snapshot.Entries,
			{},
			&FFileDeletionFingerprint::PhysicalPath);
		for (const FFileDeletionFingerprint& Entry : Snapshot.Entries)
			switch (Entry.Kind)
			{
			case EFileDeletionEntryKind::Directory:
				++Snapshot.Summary.FolderCount;
				break;
			case EFileDeletionEntryKind::AssetPackage:
				if (DeleteAssets) ++Snapshot.Summary.AssetCount;
				break;
			case EFileDeletionEntryKind::ManagedCompanion:
				if (DeleteAssets) ++Snapshot.Summary.CompanionCount;
				break;
			case EFileDeletionEntryKind::OrdinaryFile:
			case EFileDeletionEntryKind::UnknownPackage:
				if (DeleteFiles) ++Snapshot.Summary.FileCount;
				break;
			}
		std::ranges::sort(
			Snapshot.Blockers,
			[](const FFileDeletionBlocker& A, const FFileDeletionBlocker& B) {
				return std::tie(A.PhysicalPath, A.RelatedAssetPath, A.Kind, A.Details)
					< std::tie(B.PhysicalPath, B.RelatedAssetPath, B.Kind, B.Details);
			});
		Snapshot.Blockers.erase(
			std::unique(Snapshot.Blockers.begin(), Snapshot.Blockers.end(),
				[](const FFileDeletionBlocker& A,
					const FFileDeletionBlocker& B) {
					return A.Kind == B.Kind
						&& A.PhysicalPath == B.PhysicalPath
						&& A.RelatedAssetPath == B.RelatedAssetPath
						&& A.Details == B.Details;
				}),
			Snapshot.Blockers.end());
		return;
	}

	auto FFileDeletionState::ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities, bool OwnedOnly, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (!Snapshot.Blockers.empty()) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::InvalidData, "The deletion plan is unavailable."});
		const auto InspectionFailure = [OutError](const std::filesystem::path& Path, const std::error_code& Error,
			EFileOperation Operation = EFileOperation::Inspect) -> bool {
			const bool bMissing = Error == std::errc::no_such_file_or_directory
				|| Error == std::errc::not_a_directory;
			if (bMissing) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
				std::format("Deletion source disappeared: {}", Path.generic_string())});
			return Reject(OutError, ToError(FFileError{.Operation = Operation, .NativeError = Error, .Path = Path}));
		};
		const auto Changed = [OutError](const std::filesystem::path& Path) -> bool {
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, std::format("Deletion source changed: {}.", Path.generic_string())});
		};
		FAssetDeletionFileIdentities VerifiedFiles;
		std::unordered_set<std::string> ExpectedPaths;
		for (const FFileDeletionFingerprint& Entry : Snapshot.Entries)
		{
			const std::filesystem::path Path = Normalize(Entry.PhysicalPath);
			ExpectedPaths.insert(Path.generic_string());
			if (OwnedOnly && (Entry.Kind == EFileDeletionEntryKind::AssetPackage || Entry.Kind == EFileDeletionEntryKind::ManagedCompanion)) continue;
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (OwnedOnly && Entry.Kind == EFileDeletionEntryKind::Directory && (Error == std::errc::no_such_file_or_directory || Status.type() == std::filesystem::file_type::not_found)) continue;
			if (Error) return InspectionFailure(Path, Error);
			if (!std::filesystem::exists(Status) || std::filesystem::is_symlink(Status)) return Changed(Path);
#ifdef _WIN32
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES)
				return InspectionFailure(Path, std::error_code(static_cast<int>(GetLastError()), std::system_category()));
			if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return Changed(Path);
#endif
			const bool bDirectory = Entry.Kind == EFileDeletionEntryKind::Directory;
			if (bDirectory != std::filesystem::is_directory(Status)) return Changed(Path);
			if (!bDirectory)
			{
				const uintmax_t Size = std::filesystem::file_size(Path, Error);
				if (Error) return InspectionFailure(Path, Error, EFileOperation::QuerySize);
				if (Size != Entry.FileSize) return Changed(Path);
				auto Identity = FFileHelper::HashFileXx128(Path);
				if (!Identity)
				{
					return Reject(OutError, ToError(Identity.error()));
				}
				if (*Identity != Entry.ByteIdentity)
					return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, std::format("Deletion source bytes changed: {}.", Path.generic_string())});
				VerifiedFiles.emplace(Path.generic_string(), *Identity);
			}
			const auto WriteTime = std::filesystem::last_write_time(Path, Error);
			if (Error) return InspectionFailure(Path, Error, EFileOperation::QueryTime);
			if (!(OwnedOnly && bDirectory) && static_cast<int64>(WriteTime.time_since_epoch().count()) != Entry.LastWriteTimeTicks) return Changed(Path);
		}

		for (const FFileDeletionRoot& Root : Snapshot.MaximalRoots)
		{
			const std::filesystem::path Path = Normalize(Root.OriginalPath);
			std::error_code Error;
			const bool bDirectory = std::filesystem::is_directory(Path, Error);
			if (OwnedOnly && Error == std::errc::no_such_file_or_directory) continue;
			if (Error) return InspectionFailure(Path, Error);
			if (!bDirectory) continue;
			for (std::filesystem::recursive_directory_iterator It(
					 Path, std::filesystem::directory_options::none, Error), End;
				 !Error && It != End; It.increment(Error))
				if (!ExpectedPaths.contains(Normalize(It->path()).generic_string()))
					return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, std::format(
						"Deletion source gained an unconfirmed entry: {}.", It->path().generic_string())});
			if (Error) return InspectionFailure(Path, Error);
		}
		OutIdentities = std::move(VerifiedFiles);
		return true;
	}

	auto FFileDeletionState::DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult
	{
		Started = true;
		FAssetWriteResult Status;
		bool bUncertain = false;
		auto Fail = [&](UnifiedError::FError Error) {
			if (!Status.Error) Status.Error = std::move(Error);
			else
			{
				Status.Error->PushContext("additional failure domain", Error.GetDomain())
					.PushContext("additional failure code", Error.GetCode())
					.PushContext("additional failure message", Error.GetMessage());
				for (const auto& Context : Error.GetContexts())
					Status.Error->PushContext("additional " + Context.Key, Context.Value);
			}
		};
		auto Entries = Snapshot.Entries;
		std::ranges::sort(Entries, [](const auto& A, const auto& B) {
			const bool AD = A.Kind == EFileDeletionEntryKind::Directory, BD = B.Kind == EFileDeletionEntryKind::Directory;
			if (AD != BD) return !AD;
			const bool AC = A.Kind == EFileDeletionEntryKind::ManagedCompanion, BC = B.Kind == EFileDeletionEntryKind::ManagedCompanion;
			if (AC != BC) return !AC;
			if (AD && A.PhysicalPath.size() != B.PhysicalPath.size()) return A.PhysicalPath.size() > B.PhysicalPath.size();
			return A.PhysicalPath < B.PhysicalPath;
		});
		std::unordered_set<std::string> Removed;
		for (const auto& Entry : Entries)
		{
			if (!DeleteAssets && (Entry.Kind == EFileDeletionEntryKind::AssetPackage || Entry.Kind == EFileDeletionEntryKind::ManagedCompanion)) continue;
			if (!DeleteFiles && (Entry.Kind == EFileDeletionEntryKind::OrdinaryFile || Entry.Kind == EFileDeletionEntryKind::Directory)) continue;
			if (Hooks.ShouldCancel && Hooks.ShouldCancel()) Effects.bCancelled = true;
			if (Hooks.CanExecuteEffect && !Hooks.CanExecuteEffect()) Effects.bStopped = true;
			if (ContributorRevision != GetAssetDeleteContributorRevision() || (DeleteAssets && AssetOperation.GetRegistryRevision() != GetAssetCatalogRevision())) Effects.bStopped = true;
			if (Effects.bCancelled || Effects.bStopped)
			{ Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, "Deletion stopped before this effect."); if (!Status.Error) Fail(UnifiedError::FError{Effects.bCancelled ? AssetWriteErrors::Cancelled : AssetWriteErrors::StaleData, "Deletion stopped before this effect."}); continue; }
			if (Entry.Kind == EFileDeletionEntryKind::ManagedCompanion)
			{
				const bool OwnerSurvives = std::ranges::any_of(AssetOperation.GetEntries(), [&](const auto& Asset) {
					return std::ranges::any_of(Asset.CompanionFiles, [&](const auto& Path) { return Normalize(Path).generic_string() == Entry.PhysicalPath; }) && !Removed.contains(Normalize(Asset.RegistryEntry.PhysicalPath).generic_string());
				});
				if (OwnerSurvives) { Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, "Companion retained because its package was not removed."); continue; }
			}
			const auto Mount = FContentMountPaths::ResolveMountPath(Entry.PhysicalPath);
			std::error_code Ec;
			std::filesystem::path Reparse;
			const bool Invalid = !Mount || !Mount.Mount->bContentWritable
				|| FindReparsePointInPath(Mount.Mount->GetContentDir(), Entry.PhysicalPath, Reparse, Ec) || Ec;
			if (Ec && Ec != std::errc::no_such_file_or_directory && Ec != std::errc::not_a_directory)
			{ Fail(ToError(FFileError{.Operation = EFileOperation::Inspect, .NativeError = Ec, .Path = Entry.PhysicalPath})); Effects.bStopped = true; Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, Status.Error->ToString()); continue; }
			const auto Current = std::filesystem::symlink_status(Entry.PhysicalPath, Ec);
			if (!DeleteAssets && Entry.Kind == EFileDeletionEntryKind::Directory && (Ec == std::errc::no_such_file_or_directory || Current.type() == std::filesystem::file_type::not_found)) { Effects.bNoChanges = true; continue; }
			if (!DeleteAssets && Entry.Kind == EFileDeletionEntryKind::OrdinaryFile)
			{
				FAssetCompanionOwnership Ownership;
				std::optional<UnifiedError::FError> OwnershipError;
				if (!QueryAssetCompanionOwnership(Entry.PhysicalPath, Ownership, &OwnershipError))
				{ Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, OwnershipError->ToString()); Effects.bStopped = true; Fail(std::move(*OwnershipError)); continue; }
				if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
				{ Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, "File ownership changed before deletion."); Effects.bStopped = true; Fail(UnifiedError::FError{AssetWriteErrors::StaleData, "File ownership changed."}); continue; }
			}
			if (Ec && Ec != std::errc::no_such_file_or_directory && Ec != std::errc::not_a_directory)
			{ Fail(ToError(FFileError{.Operation = EFileOperation::Inspect, .NativeError = Ec, .Path = Entry.PhysicalPath})); Effects.bStopped = true; Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, Status.Error->ToString()); continue; }
			bool Changed = Invalid || Ec || std::filesystem::is_symlink(Current) || !std::filesystem::exists(Current);
			if (!Changed && Entry.Kind != EFileDeletionEntryKind::Directory)
			{
				const auto Identity = FFileHelper::HashFileXx128(Entry.PhysicalPath);
				if (!Identity)
				{ Fail(ToError(Identity.error())); Effects.bStopped = true; Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, Status.Error->ToString()); continue; }
				Changed = *Identity != Entry.ByteIdentity;
			}
			if (Changed)
			{ Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, "Deletion source changed immediately before destruction."); Effects.bStopped = true; Fail(UnifiedError::FError{AssetWriteErrors::StaleData, "Deletion stopped because a source changed."}); continue; }
			const auto Error = Hooks.RemoveEntry(Entry.PhysicalPath);
			const auto After = std::filesystem::symlink_status(Entry.PhysicalPath, Ec);
			const bool Gone = (!Ec || Ec == std::errc::no_such_file_or_directory) && After.type() == std::filesystem::file_type::not_found;
			if (Gone)
			{
				Removed.insert(Entry.PhysicalPath);
				Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Committed);
			}
			if (Error || !Gone)
			{
				const auto Message = "Could not permanently delete " + Entry.PhysicalPath + ": " + (Error ? Error.message() : "the entry remains");
				Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Failed, Message);
				if (Error) Fail(ToError(FFileError{.Operation = EFileOperation::Remove, .NativeError = Error, .Path = Entry.PhysicalPath}));
				else if (Ec) Fail(ToError(FFileError{.Operation = EFileOperation::Inspect, .NativeError = Ec, .Path = Entry.PhysicalPath}));
				else Fail(UnifiedError::FError{AssetWriteErrors::IoError, Message});
				if (Ec && Ec != std::errc::no_such_file_or_directory)
				{ bUncertain = true; Effects.bStopped = true; Effects.Effects.back().RecoveryLocations.push_back(Entry.PhysicalPath); }
			}
		}
		RemovedPaths.clear();
		for (const auto& Entry : Snapshot.Entries)
		{
			std::error_code Error;
			const auto State = std::filesystem::symlink_status(Entry.PhysicalPath, Error);
			if ((!Error || Error == std::errc::no_such_file_or_directory) && State.type() == std::filesystem::file_type::not_found) RemovedPaths.push_back(Normalize(Entry.PhysicalPath));
		}
		if (Status.Error) Status.Effect = bUncertain ? EAssetWriteEffect::ContentUncertain
			: Removed.empty() ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten;
		return Status;
	}

	auto FFileDeletionState::ValidateScope(std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (Attempted || !Snapshot.Blockers.empty()) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::InvalidData, "Deletion is blocked or consumed."});
		if (const bool Native = AssetOperation.Validate(OutError); !Native) return Native;
		FAssetDeletionFileIdentities Identities;
		return ValidatePhysicalState(Identities, false, OutError);
	}

	auto FFileDeletionState::Execute() -> bool
	{
		if (!Hooks.RemoveEntry) Hooks.RemoveEntry = DefaultRemoveEntry;
		Result = {};
		if (!Snapshot.Blockers.empty())
		{
			Result = {.Error = UnifiedError::FError{AssetWriteErrors::InvalidData, "Deletion is blocked or unavailable."}};
			return false;
		}
		if (!DeleteAssets)
		{
			FAssetDeletionFileIdentities Identities;
			if (!ValidatePhysicalState(Identities, PeerScopeValidated, &Result.Error)) return false;
			std::vector<std::filesystem::path> Removed;
			Result = DeletePhysicalRoots(Removed);
			return bool(Result);
		}
		const bool bSucceeded = AssetOperation.Delete({
			.Delete = [this](auto& RemovedPaths) { return DeletePhysicalRoots(RemovedPaths); },
			.ValidateFiles = [this](FAssetDeletionFileIdentities& Identities, std::optional<UnifiedError::FError>* OutError) {
				return ValidatePhysicalState(Identities, false, OutError);
			}});
		Result = AssetOperation.GetWriteResult();
		return bSucceeded;
	}
	auto FFileDeletionState::DoDelete() -> bool
	{
		if (!CanDelete()) return false;
		Attempted = true;
		auto RejectExecution = [&](std::string Message, bool Stale = false) {
			Effects.bStopped = true;
			for (const auto& Entry : Snapshot.Entries) Effects.Add(Entry.PhysicalPath, EFileDeletionEffectStatus::Unattempted, Message);
			Report(Stale ? ELogLevel::Warn : ELogLevel::Error, Message);
			return false;
		};
		std::string AdmissionError;
		if (!CanModifyContent(Policy, &AdmissionError)) return RejectExecution(AdmissionError);
		for (const auto& Root : Snapshot.MaximalRoots)
		{
			const auto Mount = FContentMountPaths::ResolveMountPath(Root.OriginalPath);
			if (!Mount || !Mount.Mount->bContentWritable || AreSamePath(Root.OriginalPath, Mount.Mount->GetContentDir().generic_string())) return RejectExecution("Deletion mount policy changed.");
			std::filesystem::path Reparse;
			std::error_code Error;
			auto Parent = std::filesystem::path(Root.OriginalPath).parent_path();
			while (!std::filesystem::exists(Parent, Error) && !Error && Parent != Parent.parent_path()) Parent = Parent.parent_path();
			if (Error || FindReparsePointInPath(Mount.Mount->GetContentDir().generic_string(), Parent, Reparse, Error) || Error) return RejectExecution("Deletion ancestor changed or cannot be inspected.");
		}
		const auto Admission = Hooks.CanExecuteEffect;
		Hooks.CanExecuteEffect = [this, Admission] { return CanModifyContent(Policy) && (!Admission || Admission()) && CanModifyContent(Policy); };
		const bool Succeeded = Execute();
		if (!Succeeded && !Started)
		{
			const bool Stale = Result.Error && (Result.Error->Is(AssetWriteErrors::StaleData) || Result.Error->Is({"AssetRegistry", "StaleData"}) || Result.Error->Is({"AssetLoad", "StaleData"}));
			return RejectExecution(Result.Error ? Result.Error->ToString() + (Stale ? " Analyze and confirm a new deletion model before retrying." : "") : "Deletion validation failed.", Stale);
		}
		FContentChangeBatch Changes;
		for (const auto& Effect : Effects.Effects)
			if (Effect.Status == EFileDeletionEffectStatus::Committed)
			{
				const auto Entry = std::ranges::find(Snapshot.Entries, Effect.PhysicalPath, &FFileDeletionFingerprint::PhysicalPath);
				Changes.Changes.push_back({EContentChangeKind::Removed, Effect.PhysicalPath, {}, {}, {}, Entry != Snapshot.Entries.end() && Entry->Kind == EFileDeletionEntryKind::Directory});
			}
		Changes.bFullRefresh = !Result;
		// Attempted is already set: feedback may create another model, never restart this one.
		for (const auto& Effect : Effects.Effects)
		{
			if (Effect.Status == EFileDeletionEffectStatus::Failed && !Effect.Diagnostic.empty()) Report(ELogLevel::Error, Effect.Diagnostic);
			for (const auto& Path : Effect.RecoveryLocations) Report(ELogLevel::Warn, "Recovery location: " + Path);
		}
		if (!Result && Result.Error && std::ranges::none_of(Effects.Effects, [&](const auto& Effect) { return Effect.Status == EFileDeletionEffectStatus::Failed && Effect.Diagnostic == Result.Error->GetMessage(); }))
			Report(Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending ? ELogLevel::Warn : ELogLevel::Error, Result.Error->ToString());
		NotifyContentMutation(Policy, std::move(Changes));
		return Started;
	}
}
