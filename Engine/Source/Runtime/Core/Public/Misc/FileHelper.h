#pragma once

#include "Misc/CoreTypes.h"
#include "Containers/ContainersFwd.h"

#include "Misc/CoreStd.h"

#include "HAL/Platform.h"
#include "HAL/PlatformFile.h"
#include "Hash/XxHash.h"

namespace Durin::FFileHelper
{
	// Silent, value-returning operations. Ordinary I/O never retries.
	// Allocation failures can throw; filesystem failures return FFileSystemError.
	[[nodiscard]] CORE_API auto LoadFileToArray(const FFilePath& Path)
		-> std::expected<FByteBuffer, FFileSystemError>;
	struct FFileReadOptions
	{
		uint64 MaximumBytes = std::numeric_limits<uint64>::max();
		std::optional<uint64> ExpectedBytes;
	};
	// Checks the opened handle size before allocation. Size checks use file_too_large
	// for limits and message_size for an expected-size mismatch. No retry or content validation.
	[[nodiscard]] CORE_API auto LoadFileToArray(const FFilePath& Path, FFileReadOptions Options)
		-> std::expected<FByteBuffer, FFileSystemError>;
	// Exact bytes, including embedded NULs; no encoding conversion.
	[[nodiscard]] CORE_API auto LoadFileToString(const FFilePath& Path)
		-> std::expected<std::string, FFileSystemError>;
	[[nodiscard]] CORE_API auto HashFileXx128(const FFilePath& Path)
		-> std::expected<FXxHash128, FFileSystemError>;
	// Creates parent directories and truncates existing files. Failure may leave
	// partial bytes. Use the atomic operation when complete replacement is needed.
	[[nodiscard]] CORE_API auto SaveArrayToFile(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileSystemError>;
	// Exclusively creates, flushes and closes; failure never removes an existing file.
	[[nodiscard]] CORE_API auto SaveArrayToNewFile(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileSystemError>;
	// Publishes complete bytes through a sibling temporary, with bounded retries.
	[[nodiscard]] CORE_API auto SaveArrayToFileAtomically(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileSystemError>;
	[[nodiscard]] CORE_API auto CopyFileAtomically(const FFilePath& Source, const FFilePath& Destination)
		-> std::expected<void, FFileSystemError>;
}
