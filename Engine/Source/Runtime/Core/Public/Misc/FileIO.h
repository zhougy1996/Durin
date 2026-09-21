#pragma once

#include "CoreAPI.h"
#include "Hash/XxHash.h"
#include "Misc/FilePath.h"
#include <expected>

namespace Durin::FFileIO
{
	enum class EFileOperation : uint8
	{
		NormalizePath, Inspect, OpenRead, QuerySize, Read,
		CreateParentDirectories, OpenWrite, Write, Close,
		CreateTemporaryFile, WriteTemporaryFile, FlushTemporaryFile,
		CloseTemporaryFile, ReplaceDestination
	};

	struct FFileError
	{
		EFileOperation Operation;
		std::error_code NativeError;
		FFilePath Path;
		// Source or destination associated with a multi-path operation, if any.
		FFilePath RelatedPath;
		struct FRange { uint64 Offset; uint64 Size; };
		std::optional<FRange> Range;
		CORE_API auto ToString() const -> std::string;
	};

	// All operations are silent. Ordinary I/O never retries. Allocation failures
	// can throw; expected filesystem failures are returned as FFileError.
	class IFileHandle
	{
	public:
		virtual ~IFileHandle() = default;
		[[nodiscard]] virtual auto GetSize() const -> uint64 = 0;
		// Exact read, serialized by the caller. Failure can modify part of Output.
		[[nodiscard]] virtual auto ReadAt(uint64 Offset, FMutableByteView Output)
			-> std::expected<void, FFileError> = 0;
	};

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
	[[nodiscard]] CORE_API auto SaveArrayToNewFile(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileError>;
	// Uses the existing publication machinery, including its bounded retries.
	[[nodiscard]] CORE_API auto SaveArrayToFileAtomically(FByteView Bytes, const FFilePath& Path)
		-> std::expected<void, FFileError>;
	[[nodiscard]] CORE_API auto CopyFileAtomically(const FFilePath& Source, const FFilePath& Destination)
		-> std::expected<void, FFileError>;
}
