#include "AssetTools/ContentFolderOperation.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/ContentFilesystem.h"
#include "AssetRegistry/ContentChangeScopes.h"

namespace Durin::ContentStorage
{
	using ContentStorage::FindReparsePointInPath;
	using ContentStorage::AreSamePath;
	namespace
	{
		using ContentStorage::Filesystem::NormalizePath;

		auto Failure(UnifiedError::FErrorId Error, std::string Message) -> FAssetWriteResult
		{ return {.Error = UnifiedError::FError{Error, std::move(Message)}}; }
		auto SetQueryError(std::string* OutError, std::string_view Message) -> bool
		{ if (OutError) *OutError = Message; return false; }

	} // namespace

	auto FContentFolderOperation::ValidateFolder(
		std::string_view PhysicalDirectory, std::string_view Name, std::string& OutNewItem) const -> FAssetWriteResult
	{
		std::string AdmissionError;
		if (!CanModify(&AdmissionError)) return Failure(AssetWriteErrors::ReadOnlyMode, AdmissionError);
		if (Name.empty() || Name == "." || Name == ".."
			|| Name.find_first_of("/\\:*?\"<>|") != std::string_view::npos
			|| Name.back() == '.' || Name.back() == ' '
			|| std::ranges::any_of(Name, [](unsigned char Character) { return Character < 32; }))
			return Failure(AssetWriteErrors::InvalidPath, "Enter a valid folder name without path separators.");
		const std::string Directory = NormalizePath(PhysicalDirectory);
		const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Directory);
		if (!Mount)
			return Failure(AssetWriteErrors::InvalidPath, "Folders require an automatically scanned content mount.");
		if (!Mount.Mount->bContentWritable)
			return Failure(AssetWriteErrors::ReadOnlyMode, "This content mount is read-only.");
		std::error_code Error;
		std::filesystem::path Reparse;
		if (!std::filesystem::is_directory(Directory, Error) || Error
			|| FindReparsePointInPath(Mount.Mount->GetContentDir().generic_string(), Directory, Reparse, Error) || Error)
			return Failure(AssetWriteErrors::InvalidPath, "The target must be an existing directory without reparse points.");
		const auto Path = (std::filesystem::path(Directory) / Name).generic_string();
		const auto Destination = ContentStorage::FContentMountPaths::ResolveMountPath(Path);
		if (!Destination || Destination.Mount != Mount.Mount)
			return Failure(AssetWriteErrors::InvalidPath, "The new folder would leave its content mount.");
		const auto Probe = ContentStorage::Filesystem::Probe(Path);
		if (Probe.Error) return Failure(AssetWriteErrors::IoError, "Could not inspect the new folder path: " + Probe.Error.message());
		if (Probe.Exists()) return Failure(AssetWriteErrors::AlreadyExists, "An item with this name already exists.");
		OutNewItem = NormalizePath(Path);
		return {};
	}
	auto FContentFolderOperation::CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* OutErrorMsg) const -> bool
	{
		if (OutErrorMsg) OutErrorMsg->clear();
		std::string Candidate;
		const auto Result = ValidateFolder(Directory, Name, Candidate);
		return Result ? true : SetQueryError(OutErrorMsg, (Result.Error ? Result.Error->ToString() : std::string{}));
	}
	auto FContentFolderOperation::CreateFolder(std::string_view Directory, std::string_view Name, std::string& OutNewItem) -> bool
	{
		OutNewItem = {};
		FContentChangeBatch Changes;
		std::string Candidate;
		const auto Status = ValidateFolder(Directory, Name, Candidate);
		if (!Status) return Complete(Status);
		std::error_code Error;
		if (!std::filesystem::create_directory(Candidate, Error) || Error)
			return Complete(Failure(AssetWriteErrors::IoError, "Could not create folder: " + Error.message()));
		OutNewItem = std::move(Candidate);
		Changes.Changes.push_back({EContentChangeKind::Added, {}, OutNewItem, {}, {}, true});
		return Complete(Status, std::move(Changes));
	}

}
