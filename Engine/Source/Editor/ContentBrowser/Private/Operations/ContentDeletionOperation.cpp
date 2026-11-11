#include "Operations/ContentDeletionOperation.h"

#include "Misc/FileHelper.h"

namespace Durin::ContentBrowser::Private
{
	namespace
	{
		auto Normalize(const std::filesystem::path& Path) -> std::filesystem::path
		{
			return std::filesystem::absolute(Path).lexically_normal();
		}

		auto DefaultRemoveAll(const std::filesystem::path& Path) -> std::error_code
		{
			std::error_code Error;
			std::filesystem::remove_all(Path, Error);
			return Error;
		}

	}

	FContentDeletionOperation::FContentDeletionOperation(
		FContentDeletionPlanPtr InPlan,
		FAssetDeletionOperation InAssetOperation)
		: Plan(std::move(InPlan)), AssetOperation(std::move(InAssetOperation))
	{
		if (!Hooks.RemoveAll) Hooks.RemoveAll = DefaultRemoveAll;
	}

	auto FContentDeletionOperation::ValidatePhysicalState(FAssetDeletionFileIdentities& OutIdentities) -> FAssetWriteResult
	{
		if (!Plan) return {EAssetWriteError::InvalidData, "The deletion plan is unavailable."};
		const auto InspectionFailure = [](const std::filesystem::path& Path, const std::error_code& Error) -> FAssetWriteResult {
			const bool bMissing = Error == std::errc::no_such_file_or_directory
				|| Error == std::errc::not_a_directory;
			return {bMissing ? EAssetWriteError::StaleData : EAssetWriteError::IoError,
				std::format("Could not inspect deletion source {}: {}", Path.generic_string(), Error.message())};
		};
		const auto Changed = [](const std::filesystem::path& Path) -> FAssetWriteResult {
			return {EAssetWriteError::StaleData,
				std::format("Deletion source changed: {}.", Path.generic_string())};
		};
		FAssetDeletionFileIdentities VerifiedFiles;
		std::unordered_set<std::string> ExpectedPaths;
		for (const FContentDeletionFingerprint& Entry : Plan->Entries)
		{
			const std::filesystem::path Path = Normalize(Entry.PhysicalPath);
			ExpectedPaths.insert(Path.generic_string());
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
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
					Failure.Message = Identity.error().ToString();
					return Failure;
				}
				if (*Identity != Entry.ByteIdentity)
					return {EAssetWriteError::StaleData,
						std::format("Deletion source bytes changed: {}.", Path.generic_string())};
				VerifiedFiles.emplace(Path.generic_string(), *Identity);
			}
			const auto WriteTime = std::filesystem::last_write_time(Path, Error);
			if (Error) return InspectionFailure(Path, Error);
			if (static_cast<int64>(WriteTime.time_since_epoch().count()) != Entry.LastWriteTimeTicks) return Changed(Path);
		}

		for (const FContentDeletionRoot& Root : Plan->MaximalRoots)
		{
			const std::filesystem::path Path = Normalize(Root.OriginalPath);
			std::error_code Error;
			const bool bDirectory = std::filesystem::is_directory(Path, Error);
			if (Error) return InspectionFailure(Path, Error);
			if (!bDirectory) continue;
			for (std::filesystem::recursive_directory_iterator It(
					 Path, std::filesystem::directory_options::none, Error), End;
				 !Error && It != End; It.increment(Error))
				if (!ExpectedPaths.contains(Normalize(It->path()).generic_string()))
					return {EAssetWriteError::StaleData, std::format(
						"Deletion source gained an unconfirmed entry: {}.", It->path().generic_string())};
			if (Error) return InspectionFailure(Path, Error);
		}
		OutIdentities = std::move(VerifiedFiles);
		return {};
	}

	auto FContentDeletionOperation::DeletePhysicalRoots(std::vector<std::filesystem::path>& RemovedPaths) -> FAssetWriteResult
	{
		FAssetWriteResult Status;
		for (const FContentDeletionRoot& Root : Plan->MaximalRoots)
		{
			const std::filesystem::path Path = Normalize(Root.OriginalPath);
			bStarted = true;
			const std::error_code Error = Hooks.RemoveAll(Path);
			if (Error)
			{
				Status = {.Error = EAssetWriteError::IoError, .Message = std::format(
					"Could not permanently delete {}: {}",
					Path.generic_string(), Error.message()),
					.Effect = EAssetWriteEffect::PartiallyWritten};
				break;
			}
		}
		// Capture the final physical outcome once, including a partially removed root.
		RemovedPaths.clear();
		for (const auto& Entry : Plan->Entries)
		{
			std::error_code ProbeError;
			const auto FileStatus = std::filesystem::symlink_status(Entry.PhysicalPath, ProbeError);
			if ((!ProbeError || ProbeError == std::errc::no_such_file_or_directory)
				&& FileStatus.type() == std::filesystem::file_type::not_found)
				RemovedPaths.push_back(Normalize(Entry.PhysicalPath));
		}
		return Status;
	}

	auto FContentDeletionOperation::Execute(FContentDeletionHooks InHooks) -> bool
	{
		if (InHooks.RemoveAll) Hooks = std::move(InHooks);
		Result = {};
		if (bStarted || !Plan || !Plan->CanExecute())
		{
			Result = {EAssetWriteError::InvalidData, "Deletion is blocked or unavailable."};
			return false;
		}
		const bool bSucceeded = AssetOperation.Delete({
			.Delete = [this](auto& RemovedPaths) { return DeletePhysicalRoots(RemovedPaths); },
			.ValidateFiles = [this](FAssetDeletionFileIdentities& Identities) -> FAssetWriteResult {
				return ValidatePhysicalState(Identities);
			}});
		Result = AssetOperation.GetWriteResult();
		return bSucceeded;
	}
} // namespace Durin::ContentBrowser::Private
