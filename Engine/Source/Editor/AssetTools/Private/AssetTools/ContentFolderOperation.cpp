#include "AssetTools/ContentFolderOperation.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/ContentFilesystem.h"
#include "Misc/FileError.h"
#include "Asset/AssetWriteResult.h"
#include "AssetRegistry/ContentChangeScopes.h"

namespace Durin::ContentStorage
{
	namespace
	{
		using ContentStorage::Filesystem::NormalizePath;

		auto Reject(std::optional<UnifiedError::FError>* OutError, UnifiedError::FError Error) -> bool
		{ if (OutError) *OutError = std::move(Error); return false; }
	} // namespace

	auto FContentFolderOperation::ValidateFolder(
		std::string_view PhysicalDirectory, std::string_view Name, std::string& OutNewItem, std::optional<UnifiedError::FError>* OutError) const -> bool
	{
		if (OutError) OutError->reset();
		OutNewItem = {};
		std::string AdmissionError;
		if (!CanModifyContent(Policy, &AdmissionError)) return Reject(OutError, UnifiedError::FError(AssetWriteErrors::ReadOnlyMode, AdmissionError));
		if (Name.empty() || Name == "." || Name == ".."
			|| Name.find_first_of("/\\:*?\"<>|") != std::string_view::npos
			|| Name.back() == '.' || Name.back() == ' '
			|| std::ranges::any_of(Name, [](unsigned char Character) { return Character < 32; }))
			return Reject(OutError, UnifiedError::FError(AssetWriteErrors::InvalidPath, "Enter a valid folder name without path separators."));
		const std::string Directory = NormalizePath(PhysicalDirectory);
		const auto Mount = FContentMountPaths::ResolveMountPath(Directory);
		if (!Mount)
			return Reject(OutError, UnifiedError::FError(AssetWriteErrors::InvalidPath, "Folders require an automatically scanned content mount."));
		if (!Mount.Mount->bContentWritable)
			return Reject(OutError, UnifiedError::FError(AssetWriteErrors::ReadOnlyMode, "This content mount is read-only."));
		std::error_code Error;
		std::filesystem::path Reparse;
		const bool DirectoryExists = std::filesystem::is_directory(Directory, Error);
		if (Error) return Reject(OutError, ToError(FFileError{.Operation = EFileOperation::Inspect, .NativeError = Error, .Path = Directory}));
		const bool HasReparse = DirectoryExists && FindReparsePointInPath(Mount.Mount->GetContentDir(), Directory, Reparse, Error);
		if (Error) return Reject(OutError, ToError(FFileError{.Operation = EFileOperation::Inspect, .NativeError = Error, .Path = Directory}));
		if (!DirectoryExists || HasReparse)
			return Reject(OutError, UnifiedError::FError(AssetWriteErrors::InvalidPath, "The target must be an existing directory without reparse points."));
		const auto Path = (std::filesystem::path(Directory) / Name).generic_string();
		const auto Destination = FContentMountPaths::ResolveMountPath(Path);
		if (!Destination || Destination.Mount != Mount.Mount)
			return Reject(OutError, UnifiedError::FError(AssetWriteErrors::InvalidPath, "The new folder would leave its content mount."));
		const auto Probe = ContentStorage::Filesystem::Probe(Path);
		if (Probe.Error) return Reject(OutError, ToError(FFileError{.Operation = EFileOperation::Inspect, .NativeError = Probe.Error, .Path = Path}));
		if (Probe.Exists()) return Reject(OutError, UnifiedError::FError(AssetWriteErrors::AlreadyExists, "An item with this name already exists."));
		OutNewItem = NormalizePath(Path);
		return true;
	}
	auto FContentFolderOperation::CanCreateFolder(std::string_view Directory, std::string_view Name, std::string* OutErrorMsg) const -> bool
	{
		if (OutErrorMsg) OutErrorMsg->clear();
		std::string Candidate;
		std::optional<UnifiedError::FError> Error;
		const bool Valid = ValidateFolder(Directory, Name, Candidate, OutErrorMsg ? &Error : nullptr);
		if (!Valid && OutErrorMsg && Error) *OutErrorMsg = Error->ToString();
		return Valid;
	}
	auto FContentFolderOperation::CreateFolder(std::string_view Directory, std::string_view Name, std::string& OutNewItem) -> bool
	{
		OutNewItem = {};
		std::string Candidate;
		std::optional<UnifiedError::FError> Error;
		if (!ValidateFolder(Directory, Name, Candidate, &Error)) return ReportContentMutationFailure(Policy, *Error);
		std::error_code Ec;
		const bool Created = std::filesystem::create_directory(Candidate, Ec);
		if (Ec) return ReportContentMutationFailure(Policy, ToError(FFileError{.Operation = EFileOperation::CreateParentDirectories, .NativeError = Ec, .Path = Candidate}));
		if (!Created) return ReportContentMutationFailure(Policy, UnifiedError::FError{AssetWriteErrors::AlreadyExists, "An item with this name already exists."});
		OutNewItem = std::move(Candidate);
		FContentChangeBatch Changes;
		Changes.Changes.push_back({EContentChangeKind::Added, {}, OutNewItem, {}, {}, true});
		NotifyContentMutation(Policy, std::move(Changes));
		return true;
	}
}
