#include "FileSystemCacheBackend.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::DerivedData
{
	auto FFileSystemCacheBackend::GetBucketDirectory(const FCacheBucket& Bucket) const
		-> FFilePath
	{
		return (FFilePath(FPaths::DerivedDataCacheDir())
			/ std::string(Bucket.ToString())).lexically_normal();
	}

	auto FFileSystemCacheBackend::GetEntryPath(const FCacheKey& Key) const
		-> std::expected<FFilePath, FCacheError>
	{
		if (!Key.IsValid())
			return std::unexpected(FCacheError{ECacheError::InvalidRequest,
				"Cache key is invalid."});
		const FCacheBucket& Bucket = Key.GetBucket();
		const FFilePath Directory = GetBucketDirectory(Bucket);
		const std::string KeyText = Key.ToString();
		// The bucket directory is already normalized. A sealed binary key produces
		// only hex path components; checking that alphabet proves containment without
		// repeatedly normalizing the same absolute directory on every cache hit.
		if (KeyText.empty() || !std::ranges::all_of(KeyText, [](char C) {
			return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
		}))
		{
			return std::unexpected(FCacheError{ECacheError::InvalidRequest,
				"Cache key has an invalid path representation."});
		}
		return Directory / KeyText.substr(0, 2) / (KeyText + ".bin");
	}

	auto FFileSystemCacheBackend::Get(const FCacheStorageGetRequest& Request) const
		-> FCacheStorageGetResult
	{
		if (Request.MaximumValueBytes == 0)
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache get request is invalid."});
		auto EntryPath = GetEntryPath(Request.Key);
		if (!EntryPath) return std::unexpected(std::move(EntryPath.error()));
		FFilePath Path = std::move(*EntryPath);

		std::error_code ErrorCode;
		const std::filesystem::file_status Status = std::filesystem::symlink_status(Path, ErrorCode);
		if (ErrorCode)
		{
			if (ErrorCode == std::errc::no_such_file_or_directory)
			{
				for (FFilePath Ancestor = Path.parent_path(); !Ancestor.empty();
					Ancestor = Ancestor.parent_path())
				{
					std::error_code AncestorError;
					const std::filesystem::file_status AncestorStatus =
						std::filesystem::symlink_status(Ancestor, AncestorError);
					if (AncestorError == std::errc::no_such_file_or_directory
						|| (!AncestorError && !std::filesystem::exists(AncestorStatus)))
						continue;
					if (AncestorError)
						return std::unexpected(FCacheError{ECacheError::StorageFailure,
							std::format("Failed to inspect cache entry parent: {}", AncestorError.message())});
					if (!std::filesystem::is_directory(AncestorStatus))
						return std::unexpected(FCacheError{ECacheError::StorageFailure,
							"Cache entry parent is not a directory."});
					return std::optional<FSharedByteBuffer>{};
				}
			}
			return std::unexpected(FCacheError{ECacheError::StorageFailure,
				std::format("Failed to inspect cache entry: {}", ErrorCode.message())});
		}
		if (!std::filesystem::exists(Status))
			return std::optional<FSharedByteBuffer>{};
		if (!std::filesystem::is_regular_file(Status))
			return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache entry is not a regular file."});
		FFilePath ResolvedPath;
		if (!FPaths::TryResolveContainedPath(
			Path, GetBucketDirectory(Request.Key.GetBucket()), ResolvedPath, ErrorCode))
			return std::unexpected(FCacheError{ECacheError::StorageFailure,
				"Cache entry resolves outside its configured bucket."});
		const uint64 FileSize = std::filesystem::file_size(ResolvedPath, ErrorCode);
		if (ErrorCode)
			return std::unexpected(FCacheError{ECacheError::StorageFailure, "Failed to inspect cache entry size."});
		if (FileSize > Request.MaximumValueBytes)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache entry exceeds its configured size limit."});

		// Bound the opened file before allocation as well as the earlier path query.
		auto Bytes = FFileHelper::LoadFileToArray(ResolvedPath,
			{.MaximumBytes = Request.MaximumValueBytes});
		if (!Bytes)
		{
			if (Bytes.error().NativeError == std::errc::file_too_large)
				return std::unexpected(FCacheError{ECacheError::ValueTooLarge,
					"Cache entry exceeds its configured size limit."});
			return std::unexpected(FCacheError{ECacheError::StorageFailure,
				std::format("Failed to read cache entry: {}", Bytes.error().ToString())});
		}
		// Record decoding owns integrity validation; storage returns the exact bytes.
		return std::optional<FSharedByteBuffer>{FSharedByteBuffer::Take(std::move(*Bytes))};
	}

	auto FFileSystemCacheBackend::Put(const FCacheStoragePutRequest& Request) const
		-> FCacheStoragePutResult
	{
		if (Request.MaximumValueBytes == 0)
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache put request is invalid."});
		if (Request.Value.size() > Request.MaximumValueBytes)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache entry exceeds its configured size limit."});
		auto EntryPath = GetEntryPath(Request.Key);
		if (!EntryPath) return std::unexpected(std::move(EntryPath.error()));
		FFilePath Path = std::move(*EntryPath);

		std::error_code ErrorCode;
		FFilePath ResolvedPath;
		const FFilePath BucketDirectory =
			GetBucketDirectory(Request.Key.GetBucket());
		if (!FPaths::TryResolveContainedPath(Path, BucketDirectory, ResolvedPath, ErrorCode))
			return std::unexpected(FCacheError{ECacheError::StorageFailure, ErrorCode
				? std::format("Failed to resolve cache entry path: {}", ErrorCode.message())
				: "Cache entry resolves outside its configured bucket."});
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		if (ErrorCode)
			return std::unexpected(FCacheError{ECacheError::StorageFailure,
				std::format("Failed to create cache entry directory: {}", ErrorCode.message())});
		if (!FPaths::TryResolveContainedPath(Path, BucketDirectory, ResolvedPath, ErrorCode))
			return std::unexpected(FCacheError{ECacheError::StorageFailure, ErrorCode
				? std::format("Failed to resolve cache entry path: {}", ErrorCode.message())
				: "Cache entry resolves outside its configured bucket."});
		// Persist the self-validating record without a second envelope or payload copy.
		auto Saved = FFileHelper::SaveArrayToFileAtomically(Request.Value, ResolvedPath);
		if (!Saved)
			return std::unexpected(FCacheError{ECacheError::StorageFailure,
				std::format("Failed to write cache entry: {}", Saved.error().ToString())});
		return {};
	}

}
