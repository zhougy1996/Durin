#include "AssetTools/FileDeletionOperation.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/ContentMountPaths.h"

#include "Misc/FileHelper.h"
#include "Misc/FileError.h"

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

	FFileDeletionOperation::FFileDeletionOperation(
		FFileDeletionPlanPtr InPlan,
		FAssetDeletionOperation InAssetOperation, bool InDeleteAssets, bool InDeleteFiles, std::shared_ptr<FAssetDeleteModel> InModel)
		: Plan(std::move(InPlan)), Model(std::move(InModel)), AssetOperation(std::move(InAssetOperation)), DeleteAssets(InDeleteAssets), DeleteFiles(InDeleteFiles)
	{
		ContributorRevision = GetAssetDeleteContributorRevision();
		if (!Hooks.RemoveEntry) Hooks.RemoveEntry = DefaultRemoveEntry;
	}

	auto FFileDeletionOperation::ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities, bool OwnedOnly, std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (!Plan) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::InvalidData, "The deletion plan is unavailable."});
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
		for (const FFileDeletionFingerprint& Entry : Plan->Entries)
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

		for (const FFileDeletionRoot& Root : Plan->MaximalRoots)
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

	auto FFileDeletionOperation::DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult
	{
		bStarted = true;
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
		auto Entries = Plan->Entries;
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
		for (const auto& Entry : Plan->Entries)
		{
			std::error_code Error;
			const auto State = std::filesystem::symlink_status(Entry.PhysicalPath, Error);
			if ((!Error || Error == std::errc::no_such_file_or_directory) && State.type() == std::filesystem::file_type::not_found) RemovedPaths.push_back(Normalize(Entry.PhysicalPath));
		}
		if (Status.Error) Status.Effect = bUncertain ? EAssetWriteEffect::ContentUncertain
			: Removed.empty() ? EAssetWriteEffect::None : EAssetWriteEffect::PartiallyWritten;
		return Status;
	}

	auto FFileDeletionOperation::ValidateScope(std::optional<UnifiedError::FError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		if (bStarted || !Plan || !Plan->CanExecute()) return Reject(OutError, UnifiedError::FError{AssetWriteErrors::InvalidData, "Deletion is blocked or consumed."});
		if (const bool Native = AssetOperation.Validate(OutError); !Native) return Native;
		FAssetDeletionFileIdentities Identities;
		return ValidatePhysicalState(Identities, false, OutError);
	}

	auto FFileDeletionOperation::Execute(FFileDeletionHooks InHooks) -> bool
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
