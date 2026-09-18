#pragma once

#include "Asset/AssetDefinitions.h"
#include "DObject/Archive.h"

namespace Durin
{
	struct FPackageObjectLoadError
	{
		EAssetError Code = EAssetError::None;
		std::string ObjectPath;
		std::string ArchivePath;
		std::string Subject;
		std::string DeclaringType;
		std::string ExpectedType;
		std::string ActualType;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 Offset = 0;
		uint64 Total = 0;
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::variant<std::monostate, FPropertyValueError, FReflectedMapKeyError,
			FObjectValidationError, FObjectError> Cause;
		std::shared_ptr<const FAssetResult> AssetCause;
	};

	struct FPackageObjectLoadResult
	{
		FPackageObjectLoadError Error;
		auto Succeeded() const -> bool { return Error.Code == EAssetError::None; }
		explicit operator bool() const { return Succeeded(); }
	};

	ENGINE_API auto FormatPackageObjectLoadError(const FPackageObjectLoadError& Error) -> std::string;
}
