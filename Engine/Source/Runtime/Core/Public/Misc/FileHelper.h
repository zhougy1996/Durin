#pragma once

#include "Misc/FileHandle.h"
#include "Hash/XxHash.h"

namespace Durin::FFileHelper
{
	// Silent, value-returning operations. Ordinary I/O never retries.
	// Allocation failures can throw; filesystem failures return FFileError.
	[[nodiscard]] CORE_API auto OpenRead(const FFilePath& Path)
		-> std::expected<std::unique_ptr<IFileHandle>, FFileError>;
	// Success(false) means missing. Test has_value() before inspecting the bool.
	[[nodiscard]] CORE_API auto FileExists(const FFilePath& Path)
		-> std::expected<bool, FFileError>;
	[[nodiscard]] CORE_API auto LoadFileToArray(const FFilePath& Path)
		-> std::expected<FByteBuffer, FFileError>;
	// Exact bytes, including embedded NULs; no encoding conversion.
	[[nodiscard]] CORE_API auto LoadFileToString(const FFilePath& Path)
		-> std::expected<std::string, FFileError>;
	[[nodiscard]] CORE_API auto HashFileXx128(const FFilePath& Path)
		-> std::expected<FXxHash128, FFileError>;
	// Creates parent directories and truncates existing files. Failure may leave
	// partial bytes. Use the atomic operation when complete replacement is needed.
	[[nodiscard]] CORE_API auto SaveArrayToFile(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileError>;
	// Exclusively creates, flushes and closes; failure never removes an existing file.
	[[nodiscard]] CORE_API auto SaveArrayToNewFile(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileError>;
	// Publishes complete bytes through a sibling temporary, with bounded retries.
	[[nodiscard]] CORE_API auto SaveArrayToFileAtomically(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileError>;
	[[nodiscard]] CORE_API auto CopyFileAtomically(const FFilePath& Source, const FFilePath& Destination)
		-> std::expected<void, FFileError>;
}
