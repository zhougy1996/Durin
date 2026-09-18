#pragma once

#include "EngineAPI.h"

namespace Durin
{
	enum class EArchiveFailureCode : uint8;
	enum class EEditorBulkDataStorageError : uint8
	{
		None, DepthLimit, UnsupportedVersion, InvalidDescriptor, InvalidStructHeader,
		FieldLimit, TruncatedField, FileSystem,
	};
	struct FEditorBulkDataStorageError
	{
		EEditorBulkDataStorageError Code = EEditorBulkDataStorageError::None;
		uint64 ObjectId = 0;
		std::string ObjectPath;
		std::vector<std::string> FieldRoute;
		std::string StructName;
		uint32 Depth = 0;
		uint32 SourceFormatVersion = 0;
		uint64 Index = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		std::filesystem::path Path;
		std::error_code SystemError;
	};
	struct FEditorBulkDataStorageResult
	{
		FEditorBulkDataStorageError Error;
		auto Succeeded() const -> bool { return Error.Code == EEditorBulkDataStorageError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
	ENGINE_API auto FormatEditorBulkDataStorageError(const FEditorBulkDataStorageError& Error) -> std::string;
}
