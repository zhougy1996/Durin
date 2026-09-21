#pragma once

#include "CoreAPI.h"
#include "Misc/FilePath.h"
#include "Hash/XxHash.h"

namespace Durin
{
	namespace FFileHelper
	{
		enum class EFileIoOperation : uint8
		{
			None,
			OpenRead,
			QuerySize,
			Read
		};

		struct FFileIoError
		{
			EFileIoOperation Operation = EFileIoOperation::None;
			std::error_code NativeError;
			FFilePath Path;
			uint64 Offset = 0;
			uint64 Size = 0;

			CORE_API auto ToString() const -> std::string;
		};

		// Uniquely owned synchronous random-read capability. ReadAt is exact: it
		// either fills Output or fails without exposing a shared stream cursor.
		class IFileHandle
		{
		public:
			virtual ~IFileHandle() = default;
			virtual auto GetSize() const -> uint64 = 0;
			virtual auto ReadAt(
				uint64 Offset,
				FMutableByteView Output,
				FFileIoError* OutError = nullptr) -> bool = 0;
		};

		CORE_API auto OpenRead(
			const FFilePath& FilePath,
			FFileIoError* OutError = nullptr) -> std::unique_ptr<IFileHandle>;

		// Identifies the filesystem operation that prevented atomic file publication.
		enum class EAtomicFileOperation : uint8
		{
			None,
			NormalizeDestination,
			CreateParentDirectories,
			CreateTemporaryFile,
			WriteTemporaryFile,
			FlushTemporaryFile,
			CloseTemporaryFile,
			ReplaceDestination
		};

		// Preserves the native cause and path metrics for an atomic publication failure.
		struct FAtomicFileError
		{
			EAtomicFileOperation Operation = EAtomicFileOperation::None;
			std::error_code NativeError;
			FFilePath Path;
			size_t PathLength = 0;
			size_t LongestComponentLength = 0;

			CORE_API auto ToString() const -> std::string;
		};

		CORE_API bool FileExists(std::string_view FileName);

		CORE_API bool LoadFileToArray(FByteBuffer& Result, const FFilePath& FilePath);

		CORE_API bool LoadFileToArray(std::vector<uint32>& Result, const FFilePath& FilePath);

		CORE_API bool LoadFileToString(std::string& Result, std::string_view FileName);

		// Hashes a file incrementally with bounded memory. The caller owns any
		// before/after metadata checks needed for a larger immutable snapshot.
		CORE_API auto HashFileXx128(
			const FFilePath& FilePath,
			FXxHash128& OutHash,
			std::error_code& OutError) -> bool;

		CORE_API bool SaveArrayToFile(const FByteView& Array, const FFilePath& FilePath);

		CORE_API bool SaveArrayToFile(const std::span<const uint32>& Array, const FFilePath& FilePath);

		// Exclusively creates FilePath, then writes, flushes and closes its bytes.
		// Existing paths are never overwritten or removed. Failure after creation
		// performs best-effort cleanup. Partial bytes are visible until completion;
		// the caller must keep this path unpublished while writing.
		CORE_API auto SaveArrayToNewFile(
			FByteView Array,
			const FFilePath& FilePath,
			FAtomicFileError* OutError = nullptr
		) -> bool;

		// Publishes complete bytes through a fixed-length sibling temporary file.
		// Concurrent publishers are last-writer-wins and never expose partial bytes.
		CORE_API auto SaveArrayToFileAtomically(
			FByteView Array,
			const FFilePath& FilePath,
			FAtomicFileError* OutError = nullptr
		) -> bool;

		// Publishes a bounded-memory copy through a sibling temporary file.
		CORE_API auto CopyFileAtomically(
			const FFilePath& SourcePath,
			const FFilePath& DestinationPath,
			FAtomicFileError* OutError = nullptr
		) -> bool;

	}
}
