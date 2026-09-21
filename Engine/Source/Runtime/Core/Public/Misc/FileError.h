#pragma once

#include "CoreAPI.h"
#include "Misc/FilePath.h"

namespace Durin
{
	// Identifies the physical operation that failed.
	enum class EFileOperation : uint8
	{
		NormalizePath, Inspect, OpenRead, QuerySize, Read,
		CreateParentDirectories, OpenWrite, Write, Close,
		CreateTemporaryFile, WriteTemporaryFile, FlushTemporaryFile,
		CloseTemporaryFile, ReplaceDestination
	};

	// Owns the native cause and physical context; formatting never logs.
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

}
