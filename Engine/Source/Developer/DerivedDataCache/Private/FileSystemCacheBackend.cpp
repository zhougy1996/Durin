#include "FileSystemCacheBackend.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::DerivedData
{
	auto FFileSystemCacheBackend::GetBucketDirectory(const FCacheBucket& Bucket) const
		-> std::filesystem::path
	{
		return (std::filesystem::path(FPaths::DerivedDataCacheDir())
			/ std::string(Bucket.ToString())).lexically_normal();
	}

	auto FFileSystemCacheBackend::GetEntryPath(
		const FCacheBucket& Bucket, const FCacheKey& Key,
		std::filesystem::path& OutPath, std::string& OutError) const -> bool
	{
		if (!Bucket.IsValid() || !Key.IsValid())
		{
			OutError = "Cache bucket or key is invalid.";
			return false;
		}
		const std::filesystem::path Directory = GetBucketDirectory(Bucket);
		const std::string KeyText(Key.ToString());
		const std::filesystem::path Candidate =
			(Directory / KeyText.substr(0, 2) / (KeyText + ".bin")).lexically_normal();
		if (!FPaths::IsLexicalDescendantPath(Candidate, Directory, true))
		{
			OutError = "Cache entry path escapes its configured bucket.";
			return false;
		}
		OutPath = Candidate;
		OutError.clear();
		return true;
	}

	auto FFileSystemCacheBackend::Get(const FCacheGetRequest& Request) const
		-> FCacheGetResult
	{
		if (!Request.Bucket.IsValid() || !Request.Key.IsValid()
			|| Request.MaximumValueBytes == 0)
			return {ECacheGetStatus::InvalidRequest, {}, "Cache get request is invalid."};
		std::filesystem::path Path;
		std::string Error;
		if (!GetEntryPath(Request.Bucket, Request.Key, Path, Error))
			return {ECacheGetStatus::InvalidRequest, {}, std::move(Error)};

		std::error_code ErrorCode;
		const std::filesystem::file_status Status = std::filesystem::symlink_status(Path, ErrorCode);
		if (ErrorCode)
		{
			if (ErrorCode == std::errc::no_such_file_or_directory)
			{
				for (std::filesystem::path Ancestor = Path.parent_path(); !Ancestor.empty();
					Ancestor = Ancestor.parent_path())
				{
					std::error_code AncestorError;
					const std::filesystem::file_status AncestorStatus =
						std::filesystem::symlink_status(Ancestor, AncestorError);
					if (AncestorError == std::errc::no_such_file_or_directory
						|| (!AncestorError && !std::filesystem::exists(AncestorStatus)))
						continue;
					if (AncestorError)
						return {ECacheGetStatus::StorageFailure, {},
							std::format("Failed to inspect cache entry parent: {}", AncestorError.message())};
					if (!std::filesystem::is_directory(AncestorStatus))
						return {ECacheGetStatus::StorageFailure, {},
							"Cache entry parent is not a directory."};
					return {ECacheGetStatus::Miss, {}, "Cache entry is missing."};
				}
			}
			return {ECacheGetStatus::StorageFailure, {},
				std::format("Failed to inspect cache entry: {}", ErrorCode.message())};
		}
		if (!std::filesystem::exists(Status))
			return {ECacheGetStatus::Miss, {}, "Cache entry is missing."};
		if (!std::filesystem::is_regular_file(Status))
			return {ECacheGetStatus::StorageFailure, {}, "Cache entry is not a regular file."};
		std::filesystem::path ResolvedPath;
		if (!FPaths::TryResolveContainedPath(
			Path, GetBucketDirectory(Request.Bucket), ResolvedPath, ErrorCode))
			return {ECacheGetStatus::StorageFailure, {},
				"Cache entry resolves outside its configured bucket."};
		const uint64 FileSize = std::filesystem::file_size(ResolvedPath, ErrorCode);
		if (ErrorCode)
			return {ECacheGetStatus::StorageFailure, {}, "Failed to inspect cache entry size."};
		if (FileSize > Request.MaximumValueBytes)
			return {ECacheGetStatus::ValueTooLarge, {}, "Cache entry exceeds its configured size limit."};

		FByteArray Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, ResolvedPath))
			return {ECacheGetStatus::StorageFailure, {}, "Failed to read cache entry."};
		return {ECacheGetStatus::Hit, FSharedByteBuffer::Take(std::move(Bytes)), {}};
	}

	auto FFileSystemCacheBackend::Put(const FCachePutRequest& Request) const
		-> FCachePutResult
	{
		if (!Request.Bucket.IsValid() || !Request.Key.IsValid()
			|| Request.MaximumValueBytes == 0)
			return {ECachePutStatus::InvalidRequest, "Cache put request is invalid."};
		if (Request.Value.size() > Request.MaximumValueBytes)
			return {ECachePutStatus::ValueTooLarge, "Cache entry exceeds its configured size limit."};
		std::filesystem::path Path;
		std::string Error;
		if (!GetEntryPath(Request.Bucket, Request.Key, Path, Error))
			return {ECachePutStatus::InvalidRequest, std::move(Error)};

		std::error_code ErrorCode;
		std::filesystem::path ResolvedPath;
		const std::filesystem::path BucketDirectory = GetBucketDirectory(Request.Bucket);
		if (!FPaths::TryResolveContainedPath(Path, BucketDirectory, ResolvedPath, ErrorCode))
			return {ECachePutStatus::StorageFailure, ErrorCode
				? std::format("Failed to resolve cache entry path: {}", ErrorCode.message())
				: "Cache entry resolves outside its configured bucket."};
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		if (ErrorCode)
			return {ECachePutStatus::StorageFailure,
				std::format("Failed to create cache entry directory: {}", ErrorCode.message())};
		if (!FPaths::TryResolveContainedPath(Path, BucketDirectory, ResolvedPath, ErrorCode))
			return {ECachePutStatus::StorageFailure, ErrorCode
				? std::format("Failed to resolve cache entry path: {}", ErrorCode.message())
				: "Cache entry resolves outside its configured bucket."};
		FFileHelper::FAtomicFileError FileError;
		if (!FFileHelper::SaveArrayToFileAtomically(Request.Value, ResolvedPath, &FileError))
			return {ECachePutStatus::StorageFailure, FileError.ToString()};
		return {ECachePutStatus::Stored, {}};
	}

}
