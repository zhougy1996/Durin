#include "Operations/ContentBrowserOperationService.h"

#include "Misc/FileHelper.h"

namespace Durin::Editor::ContentBrowser::Private
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

	auto FContentDeletionOperation::Fail(std::string Message) -> bool
	{
		Details = std::move(Message);
		return false;
	}

	auto FContentDeletionOperation::ValidatePhysicalState() -> bool
	{
		if (!Plan) return Fail("The deletion plan is unavailable.");
		std::unordered_set<std::string> ExpectedPaths;
		for (const FContentDeletionFingerprint& Entry : Plan->Entries)
		{
			const std::filesystem::path Path = Normalize(Entry.PhysicalPath);
			ExpectedPaths.insert(Path.generic_string());
			std::error_code Error;
			if (RemovedPaths.contains(Path.generic_string()))
			{
				const auto Status = std::filesystem::symlink_status(Path, Error);
				if ((!Error || Error == std::errc::no_such_file_or_directory)
					&& Status.type() == std::filesystem::file_type::not_found) continue;
				return Fail(std::format("Deleted path was replaced: {}.", Path.generic_string()));
			}
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status))
				return Fail(std::format(
					"Deletion source changed: {}.", Path.generic_string()));
#ifdef _WIN32
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				return Fail(std::format("Deletion source became a reparse point: {}.", Path.generic_string()));
#endif
			const bool bDirectory =
				Entry.Kind == EContentDeletionEntryKind::Directory;
			if (bDirectory != std::filesystem::is_directory(Status))
				return Fail(std::format(
					"Deletion source changed: {}.", Path.generic_string()));
			if (!bDirectory)
			{
				const uintmax_t Size = std::filesystem::file_size(Path, Error);
				if (Error || Size != Entry.FileSize)
					return Fail(std::format(
						"Deletion source changed: {}.", Path.generic_string()));
				FXxHash128 Identity;
				if (!FFileHelper::HashFileXx128(Path, Identity, Error)
					|| Identity != Entry.ByteIdentity)
					return Fail(std::format(
						"Deletion source bytes changed: {}.", Path.generic_string()));
			}
			const auto WriteTime = std::filesystem::last_write_time(Path, Error);
			if (Error || (!(bStarted && bDirectory)
				&& static_cast<int64>(WriteTime.time_since_epoch().count()) != Entry.LastWriteTimeTicks))
				return Fail(std::format(
					"Deletion source changed: {}.", Path.generic_string()));
		}

		for (const FContentDeletionRoot& Root : Plan->MaximalRoots)
		{
			const std::filesystem::path Path = Normalize(Root.OriginalPath);
			std::error_code Error;
			if (!std::filesystem::is_directory(Path, Error)) continue;
			for (std::filesystem::recursive_directory_iterator It(
					 Path, std::filesystem::directory_options::none, Error), End;
				 !Error && It != End; It.increment(Error))
				if (!ExpectedPaths.contains(Normalize(It->path()).generic_string()))
					return Fail(std::format(
						"Deletion source gained an unconfirmed entry: {}.",
						It->path().generic_string()));
			if (Error)
				return Fail(std::format(
					"Could not inspect deletion source {}: {}",
					Path.generic_string(), Error.message()));
		}
		return true;
	}

	auto FContentDeletionOperation::DeletePhysicalRoots() -> FAssetResult
	{
		for (const FContentDeletionRoot& Root : Plan->MaximalRoots)
		{
			const std::filesystem::path Path = Normalize(Root.OriginalPath);
			if (RemovedPaths.contains(Path.generic_string())) continue;
			bStarted = true;
			const std::error_code Error = Hooks.RemoveAll(Path);
			for (const auto& Entry : Plan->Entries)
			{
				std::error_code ProbeError;
				const auto Status = std::filesystem::symlink_status(Entry.PhysicalPath, ProbeError);
				if ((!ProbeError || ProbeError == std::errc::no_such_file_or_directory)
					&& Status.type() == std::filesystem::file_type::not_found)
					RemovedPaths.insert(Normalize(Entry.PhysicalPath).generic_string());
			}
			if (Error)
				return {EAssetError::IoError, std::format(
					"Could not permanently delete {}: {}",
					Path.generic_string(), Error.message())};
		}
		return {};
	}

	auto FContentDeletionOperation::Execute(FContentDeletionHooks InHooks) -> FAssetOperationResult
	{
		if (InHooks.RemoveAll) Hooks = std::move(InHooks);
		Details.clear();
		if (!Plan || !Plan->CanExecute())
			return {.Kind = EAssetOperationKind::Delete, .State = EAssetOperationTerminalState::Rejected, .Message = "Deletion is blocked or unavailable."};
		if (!ValidatePhysicalState())
		{
			FAssetOperationResult Failure{.Kind = EAssetOperationKind::Delete, .State = EAssetOperationTerminalState::Rejected, .Message = Details};
			if (bStarted) Failure.State = EAssetOperationTerminalState::ForwardPending;
			return Failure;
		}
		Result = AssetOperation.Delete({.Delete = [this] { return DeletePhysicalRoots(); }});
		Details = Result.Message;
		return Result;
	}
} // namespace Durin::Editor::ContentBrowser::Private
