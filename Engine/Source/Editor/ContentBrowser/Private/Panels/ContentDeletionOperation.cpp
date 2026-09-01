#include "Panels/ContentBrowserOperations.h"

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

		auto IsSameOrDescendant(
			const std::filesystem::path& Path,
			const std::filesystem::path& Root) -> bool
		{
			const std::filesystem::path Relative = Path.lexically_relative(Root);
			return !Relative.empty()
				&& (Relative == "." || *Relative.begin() != "..");
		}
	}

	FContentDeletionOperation::FContentDeletionOperation(
		FContentDeletionPlanPtr InPlan,
		FContentDeletionHooks InHooks)
		: Plan(std::move(InPlan)), Hooks(std::move(InHooks))
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
		std::vector<std::filesystem::path> CompletedRoots;
		for (const FContentDeletionRoot& Root : Plan->MaximalRoots)
		{
			const std::filesystem::path Path = Normalize(Root.OriginalPath);
			std::error_code Error;
			if (!std::filesystem::exists(Path, Error) && !Error)
				CompletedRoots.push_back(Path);
			else if (Error)
				return Fail(std::format(
					"Could not inspect deletion source {}: {}",
					Path.generic_string(), Error.message()));
		}
		for (const FContentDeletionFingerprint& Entry : Plan->Entries)
		{
			const std::filesystem::path Path = Normalize(Entry.PhysicalPath);
			ExpectedPaths.insert(Path.generic_string());
			if (std::ranges::any_of(CompletedRoots,
				[&](const std::filesystem::path& Root) {
					return IsSameOrDescendant(Path, Root);
				}))
				continue;
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status))
				return Fail(std::format(
					"Deletion source changed: {}.", Path.generic_string()));
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
			if (Error || static_cast<int64>(WriteTime.time_since_epoch().count())
				!= Entry.LastWriteTimeTicks)
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
			const std::error_code Error = Hooks.RemoveAll(Path);
			if (Error)
				return {EAssetError::IoError, std::format(
					"Could not permanently delete {}: {}",
					Path.generic_string(), Error.message())};
		}
		return {};
	}

	auto FContentDeletionOperation::Execute() -> bool
	{
		Details.clear();
		if (!Plan || !Plan->CanExecute())
			return Fail("The deletion plan is blocked or unavailable.");
		if (!ValidatePhysicalState()) return false;
		const FAssetOperationResult Result = Plan->AssetOperation.Delete({
			.Delete = [this] { return DeletePhysicalRoots(); },
		});
		if (!Result) return Fail(Result.Message);
		Details = std::format(
			"Permanently deleted {} root(s).", Plan->MaximalRoots.size());
		return true;
	}
} // namespace Durin::Editor::ContentBrowser::Private
