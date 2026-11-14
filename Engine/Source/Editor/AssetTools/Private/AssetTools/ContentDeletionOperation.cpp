#include "AssetTools/ContentDeletionOperation.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/ContentMountPaths.h"

#include "Misc/FileHelper.h"

namespace Durin::ContentStorage
{
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

	FContentDeletionOperation::FContentDeletionOperation(
		FContentDeletionPlanPtr InPlan,
		FAssetDeletionOperation InAssetOperation, bool InDeleteAssets, bool InDeleteFiles)
		: Plan(std::move(InPlan)), AssetOperation(std::move(InAssetOperation)), DeleteAssets(InDeleteAssets), DeleteFiles(InDeleteFiles)
	{
		ContributorRevision = GetAssetDeleteContributorRevision();
		if (!Hooks.RemoveEntry) Hooks.RemoveEntry = DefaultRemoveEntry;
	}

	auto FContentDeletionOperation::ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities, bool OwnedOnly, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (!Plan) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::InvalidData, "The deletion plan is unavailable."});
		const auto InspectionFailure = [OutError](const std::filesystem::path& Path, const std::error_code& Error) -> bool {
			const bool bMissing = Error == std::errc::no_such_file_or_directory
				|| Error == std::errc::not_a_directory;
			if (bMissing) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData,
				std::format("Deletion source disappeared: {}", Path.generic_string())});
			return Reject(OutError, ToError(FFileError{.Operation = EFileOperation::Inspect, .NativeError = Error, .Path = Path}));
		};
		const auto Changed = [OutError](const std::filesystem::path& Path) -> bool {
			return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, std::format("Deletion source changed: {}.", Path.generic_string())});
		};
		FAssetDeletionFileIdentities VerifiedFiles;
		std::unordered_set<std::string> ExpectedPaths;
		for (const FContentDeletionFingerprint& Entry : Plan->Entries)
		{
			const std::filesystem::path Path = Normalize(Entry.PhysicalPath);
			ExpectedPaths.insert(Path.generic_string());
			if (OwnedOnly && (Entry.Kind == EContentDeletionEntryKind::AssetPackage || Entry.Kind == EContentDeletionEntryKind::ManagedCompanion)) continue;
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (OwnedOnly && Entry.Kind == EContentDeletionEntryKind::Directory && (Error == std::errc::no_such_file_or_directory || Status.type() == std::filesystem::file_type::not_found)) continue;
			if (Error) return InspectionFailure(Path, Error);
			if (!std::filesystem::exists(Status) || std::filesystem::is_symlink(Status)) return Changed(Path);
#ifdef _WIN32
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES)
				return InspectionFailure(Path, std::error_code(static_cast<int>(GetLastError()), std::system_category()));
			if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return Changed(Path);
#endif
			const bool bDirectory = Entry.Kind == EContentDeletionEntryKind::Directory;
			if (bDirectory != std::filesystem::is_directory(Status)) return Changed(Path);
			if (!bDirectory)
			{
				const uintmax_t Size = std::filesystem::file_size(Path, Error);
				if (Error) return InspectionFailure(Path, Error);
				if (Size != Entry.FileSize) return Changed(Path);
				auto Identity = FFileHelper::HashFileXx128(Path);
				if (!Identity)
				{
					auto Failure = InspectionFailure(Path, Identity.error().NativeError);
					if (OutError && *OutError) (*OutError)->PushContext("file failure", Identity.error().ToString());
					return Failure;
				}
				if (*Identity != Entry.ByteIdentity)
					return Reject(OutError, UnifiedError::FError{AssetWriteErrors::StaleData, std::format("Deletion source bytes changed: {}.", Path.generic_string())});
				VerifiedFiles.emplace(Path.generic_string(), *Identity);
			}
			const auto WriteTime = std::filesystem::last_write_time(Path, Error);
			if (Error) return InspectionFailure(Path, Error);
			if (!(OwnedOnly && bDirectory) && static_cast<int64>(WriteTime.time_since_epoch().count()) != Entry.LastWriteTimeTicks) return Changed(Path);
		}

		for (const FContentDeletionRoot& Root : Plan->MaximalRoots)
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

	auto FContentDeletionOperation::DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult
	{
		bStarted = true;
		FAssetWriteResult Status;
		auto Entries = Plan->Entries;
		std::ranges::sort(Entries, [](const auto& A, const auto& B) {
			const bool AD = A.Kind == EContentDeletionEntryKind::Directory, BD = B.Kind == EContentDeletionEntryKind::Directory;
			if (AD != BD) return !AD;
			const bool AC = A.Kind == EContentDeletionEntryKind::ManagedCompanion, BC = B.Kind == EContentDeletionEntryKind::ManagedCompanion;
			if (AC != BC) return !AC;
			if (AD && A.PhysicalPath.size() != B.PhysicalPath.size()) return A.PhysicalPath.size() > B.PhysicalPath.size();
			return A.PhysicalPath < B.PhysicalPath;
		});
		std::unordered_set<std::string> Removed;
		for (const auto& Entry : Entries)
		{
			if (!DeleteAssets && (Entry.Kind == EContentDeletionEntryKind::AssetPackage || Entry.Kind == EContentDeletionEntryKind::ManagedCompanion)) continue;
			if (!DeleteFiles && (Entry.Kind == EContentDeletionEntryKind::OrdinaryFile || Entry.Kind == EContentDeletionEntryKind::Directory)) continue;
			if (Hooks.ShouldCancel && Hooks.ShouldCancel()) Effects.bCancelled = true;
			if (Hooks.CanExecuteEffect && !Hooks.CanExecuteEffect()) Effects.bStopped = true;
			if (ContributorRevision != GetAssetDeleteContributorRevision() || (DeleteAssets && AssetOperation.GetRegistryRevision() != GetAssetCatalogRevision())) Effects.bStopped = true;
			if (Effects.bCancelled || Effects.bStopped)
			{ Effects.Add(Entry.PhysicalPath, EContentDeletionEffectStatus::Unattempted, "Deletion stopped before this effect."); Status = {.Error = UnifiedError::FError{AssetWriteErrors::Cancelled, "Deletion stopped after earlier committed effects."}, .Effect = EAssetWriteEffect::PartiallyWritten}; continue; }
			if (Entry.Kind == EContentDeletionEntryKind::ManagedCompanion)
			{
				const bool OwnerSurvives = std::ranges::any_of(AssetOperation.GetEntries(), [&](const auto& Asset) {
					return std::ranges::any_of(Asset.CompanionFiles, [&](const auto& Path) { return Normalize(Path).generic_string() == Entry.PhysicalPath; }) && !Removed.contains(Normalize(Asset.RegistryEntry.PhysicalPath).generic_string());
				});
				if (OwnerSurvives) { Effects.Add(Entry.PhysicalPath, EContentDeletionEffectStatus::Unattempted, "Companion retained because its package was not removed."); continue; }
			}
			const auto Mount = FContentMountPaths::ResolveMountPath(Entry.PhysicalPath);
			std::error_code Ec;
			std::filesystem::path Reparse;
			const bool Invalid = !Mount || !Mount.Mount->bContentWritable
				|| FindReparsePointInPath(Mount.Mount->GetContentDir(), Entry.PhysicalPath, Reparse, Ec) || Ec;
			const auto Current = std::filesystem::symlink_status(Entry.PhysicalPath, Ec);
			if (!DeleteAssets && Entry.Kind == EContentDeletionEntryKind::Directory && (Ec == std::errc::no_such_file_or_directory || Current.type() == std::filesystem::file_type::not_found)) { Effects.bNoChanges = true; continue; }
			if (!DeleteAssets && Entry.Kind == EContentDeletionEntryKind::OrdinaryFile)
			{
				FAssetCompanionOwnership Ownership;
				std::optional<UnifiedError::FError> OwnershipError;
				if (!QueryAssetCompanionOwnership(Entry.PhysicalPath, Ownership, &OwnershipError))
				{ Effects.Add(Entry.PhysicalPath, EContentDeletionEffectStatus::Unattempted, OwnershipError->ToString()); Effects.bStopped = true; Status = {.Error = std::move(OwnershipError), .Effect = EAssetWriteEffect::PartiallyWritten}; continue; }
				if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
				{ Effects.Add(Entry.PhysicalPath, EContentDeletionEffectStatus::Unattempted, "File ownership changed before deletion."); Effects.bStopped = true; Status = {.Error = UnifiedError::FError{AssetWriteErrors::StaleData, "File ownership changed."}, .Effect = EAssetWriteEffect::PartiallyWritten}; continue; }
			}
			bool Changed = Invalid || Ec || std::filesystem::is_symlink(Current) || !std::filesystem::exists(Current);
			if (!Changed && Entry.Kind != EContentDeletionEntryKind::Directory)
			{
				const auto Identity = FFileHelper::HashFileXx128(Entry.PhysicalPath);
				Changed = !Identity || *Identity != Entry.ByteIdentity;
			}
			if (Changed)
			{ Effects.Add(Entry.PhysicalPath, EContentDeletionEffectStatus::Unattempted, "Deletion source changed immediately before destruction."); Effects.bStopped = true; Status = {.Error = UnifiedError::FError{AssetWriteErrors::StaleData, "Deletion stopped because a source changed."}, .Effect = EAssetWriteEffect::PartiallyWritten}; continue; }
			const auto Error = Hooks.RemoveEntry(Entry.PhysicalPath);
			const auto After = std::filesystem::symlink_status(Entry.PhysicalPath, Ec);
			const bool Gone = (!Ec || Ec == std::errc::no_such_file_or_directory) && After.type() == std::filesystem::file_type::not_found;
			if (Gone)
			{
				Removed.insert(Entry.PhysicalPath);
				Effects.Add(Entry.PhysicalPath, EContentDeletionEffectStatus::Committed);
			}
			if (Error || !Gone)
			{
				const auto Message = "Could not permanently delete " + Entry.PhysicalPath + ": " + (Error ? Error.message() : "the entry remains");
				Effects.Add(Entry.PhysicalPath, EContentDeletionEffectStatus::Failed, Message);
				Status = {.Error = UnifiedError::FError{AssetWriteErrors::IoError, Message}, .Effect = EAssetWriteEffect::PartiallyWritten};
				if (Ec && Ec != std::errc::no_such_file_or_directory)
				{ Effects.bStopped = true; Effects.Effects.back().RecoveryLocations.push_back(Entry.PhysicalPath); }
			}
		}
		RemovedPaths.clear();
		for (const auto& Entry : Plan->Entries)
		{
			std::error_code Error;
			const auto State = std::filesystem::symlink_status(Entry.PhysicalPath, Error);
			if ((!Error || Error == std::errc::no_such_file_or_directory) && State.type() == std::filesystem::file_type::not_found) RemovedPaths.push_back(Normalize(Entry.PhysicalPath));
		}
		return Status;
	}

	auto FContentDeletionOperation::ValidateScope(std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (bStarted || !Plan || !Plan->CanExecute()) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::InvalidData, "Deletion is blocked or consumed."});
		if (const bool Native = AssetOperation.Validate(OutError); !Native) return Native;
		FAssetDeletionFileIdentities Identities;
		return ValidatePhysicalState(Identities, false, OutError);
	}

	auto FContentDeletionOperation::Execute(FContentDeletionHooks InHooks) -> bool
	{
		if (InHooks.RemoveEntry) Hooks.RemoveEntry = std::move(InHooks.RemoveEntry);
		Hooks.ShouldCancel = std::move(InHooks.ShouldCancel);
		Hooks.CanExecuteEffect = std::move(InHooks.CanExecuteEffect);
		Result = {};
		if (bStarted || !Plan || !Plan->CanExecute())
		{
			Result = {.Error = UnifiedError::FError{AssetWriteErrors::InvalidData, "Deletion is blocked or unavailable."}};
			return false;
		}
		if (!DeleteAssets)
		{
			FAssetDeletionFileIdentities Identities;
			if (!ValidatePhysicalState(Identities, bPeerScopeValidated, &Result.Error)) return false;
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
} // namespace Durin::ContentStorage
