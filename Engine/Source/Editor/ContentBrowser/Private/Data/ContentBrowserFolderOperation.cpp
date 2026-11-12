#include "Data/ContentBrowserFolderOperation.h"
#include "Operations/ContentBrowserPathInspection.h"
#include "Operations/ContentBrowserPaths.h"
#include "Panels/ContentBrowserFilesystem.h"
#include "Panels/ContentBrowserChanges.h"

namespace Durin::ContentBrowser::Private
{
	namespace
	{
		using ContentBrowserFilesystem::NormalizePath;

		auto Failure(EAssetWriteError Error, std::string Message) -> FAssetWriteResult
		{ return {Error, std::move(Message)}; }
		auto SetQueryError(std::string* OutError, std::string_view Message) -> bool
		{ if (OutError) *OutError = Message; return false; }
		auto SetNewItem(FContentBrowserNativeItem& Item, std::string Physical, std::string Virtual = {}) -> void
		{
			Item.PhysicalPath = std::move(Physical);
			Item.VirtualPath = std::move(Virtual);
			Item.Name = std::filesystem::path(Item.PhysicalPath).filename().generic_string();
			Item.Extension = std::filesystem::path(Item.PhysicalPath).extension().generic_string();
		}

	} // namespace

	auto FContentBrowserFolderOperation::ValidateFolder(
		std::string_view PhysicalDirectory, std::string_view Name, FContentBrowserNativeItem& OutNewItem) const -> FAssetWriteResult
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Failure(EAssetWriteError::ReadOnlyMode, AdmissionError);
		if (Name.empty() || Name == "." || Name == ".."
			|| Name.find_first_of("/\\:*?\"<>|") != std::string_view::npos
			|| Name.back() == '.' || Name.back() == ' '
			|| std::ranges::any_of(Name, [](unsigned char Character) { return Character < 32; }))
			return Failure(EAssetWriteError::InvalidPath, "Enter a valid folder name without path separators.");
		const std::string Directory = NormalizePath(PhysicalDirectory);
		const auto Mount = FContentBrowserPaths::ResolveMountPath(Directory);
		if (!Mount)
			return Failure(EAssetWriteError::InvalidPath, "Folders require an automatically scanned content mount.");
		if (!Mount.Mount->bContentWritable)
			return Failure(EAssetWriteError::ReadOnlyMode, "This content mount is read-only.");
		std::error_code Error;
		std::filesystem::path Reparse;
		if (!std::filesystem::is_directory(Directory, Error) || Error
			|| FindReparsePointInPath(Mount.Mount->GetContentDir().generic_string(), Directory, Reparse, Error) || Error)
			return Failure(EAssetWriteError::InvalidPath, "The target must be an existing directory without reparse points.");
		const auto Path = (std::filesystem::path(Directory) / Name).generic_string();
		const auto Destination = FContentBrowserPaths::ResolveMountPath(Path);
		if (!Destination || Destination.Mount != Mount.Mount)
			return Failure(EAssetWriteError::InvalidPath, "The new folder would leave its content mount.");
		const auto Probe = ContentBrowserFilesystem::Probe(Path);
		if (Probe.Error) return Failure(EAssetWriteError::IoError, "Could not inspect the new folder path: " + Probe.Error.message());
		if (Probe.Exists()) return Failure(EAssetWriteError::AlreadyExists, "An item with this name already exists.");
		OutNewItem = {};
		OutNewItem.Kind = EContentBrowserItemKind::Folder;
		SetNewItem(OutNewItem, NormalizePath(Path));
		return {};
	}
	auto FContentBrowserFolderOperation::PrepareFolder(
		std::string_view Directory, FContentBrowserNativeItem& OutNewItem) -> bool
	{
		OutNewItem = {};
		for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
		{
			const auto Name = Suffix == 0 ? std::string("New Folder") : std::format("New Folder ({})", Suffix + 1);
			const auto Status = ValidateFolder(Directory, Name, OutNewItem);
			if (Status.Error == EAssetWriteError::AlreadyExists) continue;
			return Complete(Status);
		}
		return Complete(Failure(EAssetWriteError::AlreadyExists, "Could not find a unique folder name in this directory."));
	}
	auto FContentBrowserFolderOperation::CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* OutErrorMsg) const -> bool
	{
		if (OutErrorMsg) OutErrorMsg->clear();
		FContentBrowserNativeItem Candidate;
		const auto Result = ValidateFolder(Directory, Name, Candidate);
		return Result ? true : SetQueryError(OutErrorMsg, Result.Message);
	}
	auto FContentBrowserFolderOperation::CreateFolder(std::string_view Directory, std::string_view Name, FContentBrowserNativeItem& OutNewItem) -> bool
	{
		OutNewItem = {};
		FContentChangeBatch Changes;
		FContentBrowserNativeItem Candidate;
		const auto Status = ValidateFolder(Directory, Name, Candidate);
		if (!Status) return Complete(Status);
		std::error_code Error;
		if (!std::filesystem::create_directory(Candidate.PhysicalPath, Error) || Error)
			return Complete(Failure(EAssetWriteError::IoError, "Could not create folder: " + Error.message()));
		OutNewItem = std::move(Candidate);
		Changes.Changes.push_back({EContentChangeKind::Added, {}, OutNewItem.PhysicalPath, {}, {}, true});
		return Complete(Status, std::move(Changes));
	}

}
