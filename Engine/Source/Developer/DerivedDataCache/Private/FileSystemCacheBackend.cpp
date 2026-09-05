#include "FileSystemCacheBackend.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::DerivedData
{
	namespace
	{
		constexpr uint32 CacheEntryMagic = 0x43444444; // DDDC
		constexpr uint32 CacheEntrySchemaVersion = 1;
		constexpr uint32 CacheEntryFormatVersion = 1;
		constexpr uint64 CacheEntryHeaderBytes = 40;
	}

	auto FFileSystemCacheBackend::GetBucketDirectory(const FCacheBucket& Bucket) const
		-> std::filesystem::path
	{
		return (std::filesystem::path(FPaths::DerivedDataCacheDir())
			/ std::string(Bucket.ToString())).lexically_normal();
	}

	auto FFileSystemCacheBackend::GetEntryPath(
		const FCacheKey& Key,
		std::filesystem::path& OutPath, std::string& OutError) const -> bool
	{
		if (!Key.IsValid())
		{
			OutError = "Cache key is invalid.";
			return false;
		}
		const FCacheBucket& Bucket = Key.GetBucket();
		const std::filesystem::path Directory = GetBucketDirectory(Bucket);
		const std::string KeyText = Key.ToString();
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
		if (!Request.Key.IsValid() || Request.MaximumValueBytes == 0)
			return {ECacheGetStatus::InvalidRequest, {}, "Cache get request is invalid."};
		std::filesystem::path Path;
		std::string Error;
		if (!GetEntryPath(Request.Key, Path, Error))
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
			Path, GetBucketDirectory(Request.Key.GetBucket()), ResolvedPath, ErrorCode))
			return {ECacheGetStatus::StorageFailure, {},
				"Cache entry resolves outside its configured bucket."};
		const uint64 FileSize = std::filesystem::file_size(ResolvedPath, ErrorCode);
		if (ErrorCode)
			return {ECacheGetStatus::StorageFailure, {}, "Failed to inspect cache entry size."};
		if (Request.MaximumValueBytes > std::numeric_limits<uint64>::max()
			- CacheEntryHeaderBytes
			|| FileSize > Request.MaximumValueBytes + CacheEntryHeaderBytes)
			return {ECacheGetStatus::ValueTooLarge, {}, "Cache entry exceeds its configured size limit."};

		FByteBuffer Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, ResolvedPath))
			return {ECacheGetStatus::StorageFailure, {}, "Failed to read cache entry."};
		FBinaryReader Reader(Bytes, {
			.MaximumTotalBytes = Request.MaximumValueBytes + CacheEntryHeaderBytes,
			.MaximumFieldBytes = std::max<uint64>(Request.MaximumValueBytes, 16)});
		uint64 ValueSize = 0;
		FXxHash128 ExpectedHash;
		FByteView Value;
		if (!Reader.ReadAndValidateHeader(
			CacheEntryMagic, CacheEntrySchemaVersion, CacheEntryFormatVersion)
			|| !Reader.ReadU64(ValueSize)
			|| ValueSize > Request.MaximumValueBytes
			|| !Reader.ReadHash128(ExpectedHash)
			|| !Reader.ReadRegion(Value, ValueSize, Request.MaximumValueBytes)
			|| !Reader.IsAtEnd())
			return {ECacheGetStatus::Corrupt, {},
				"Cache entry envelope is unsupported, truncated, or malformed."};
		if (ExpectedHash.IsZero() || FXxHash128::HashBuffer(Value) != ExpectedHash)
			return {ECacheGetStatus::Corrupt, {},
				"Cache entry content hash validation failed."};
		FSharedByteBuffer StoredBytes = FSharedByteBuffer::Take(std::move(Bytes));
		return {ECacheGetStatus::Hit,
			StoredBytes.MakeView(CacheEntryHeaderBytes, ValueSize), {}};
	}

	auto FFileSystemCacheBackend::Put(const FCachePutRequest& Request) const
		-> FCachePutResult
	{
		if (!Request.Key.IsValid() || Request.MaximumValueBytes == 0)
			return {ECachePutStatus::InvalidRequest, "Cache put request is invalid."};
		if (Request.MaximumValueBytes > std::numeric_limits<uint64>::max()
			- CacheEntryHeaderBytes)
			return {ECachePutStatus::InvalidRequest,
				"Cache put request size limit is invalid."};
		if (Request.Value.size() > Request.MaximumValueBytes)
			return {ECachePutStatus::ValueTooLarge, "Cache entry exceeds its configured size limit."};
		std::filesystem::path Path;
		std::string Error;
		if (!GetEntryPath(Request.Key, Path, Error))
			return {ECachePutStatus::InvalidRequest, std::move(Error)};

		std::error_code ErrorCode;
		std::filesystem::path ResolvedPath;
		const std::filesystem::path BucketDirectory =
			GetBucketDirectory(Request.Key.GetBucket());
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
		FBinaryWriter Writer({
			.MaximumTotalBytes = Request.MaximumValueBytes + CacheEntryHeaderBytes,
			.MaximumFieldBytes = std::max<uint64>(Request.MaximumValueBytes, 16)});
		Writer.WriteHeader({CacheEntryMagic, CacheEntrySchemaVersion,
			CacheEntryFormatVersion});
		Writer.WriteU64(Request.Value.size());
		Writer.WriteHash128(FXxHash128::HashBuffer(Request.Value));
		Writer.WriteBytes(Request.Value);
		if (Writer.HasError())
			return {ECachePutStatus::StorageFailure,
				"Failed to encode the cache entry envelope."};
		FFileHelper::FAtomicFileError FileError;
		if (!FFileHelper::SaveArrayToFileAtomically(
			Writer.GetBytes(), ResolvedPath, &FileError))
			return {ECachePutStatus::StorageFailure, FileError.ToString()};
		return {ECachePutStatus::Stored, {}};
	}

}
