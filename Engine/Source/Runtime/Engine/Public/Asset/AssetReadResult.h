#pragma once

#include "EngineAPI.h"
#include "Asset/PackageResourceError.h"
#include "Asset/EditorBulkDataStorageError.h"
#include "DObject/ObjectValidation.h"
#include "AssetRegistry/RegistryResult.h"
#include "DObject/AssetPath.h"
#include "DObject/ObjectDiagnostic.h"

#include <expected>

namespace Durin
{
	// Classifies package reads, object loading, and resident-object access failures.
	enum class EAssetReadError : uint8
	{
		None,
		InvalidPath,
		AlreadyExists,
		NotFound,
		IoError,
		CorruptFile,
		UnsupportedVersion,
		UnknownClass,
		TypeMismatch,
		MissingDependency,
		CircularDependency,
		InvalidObjectGraph,
		UnsupportedProperty,
		InvalidPackageType,
		InUse,
		StaleData,
		ShuttingDown,
		Cancelled,
		ProjectionPending
	};

	// Failure payload for completed reads. Residency and load reports are caller-owned.
	struct FAssetReadError
	{
		EAssetReadError Code = EAssetReadError::InvalidObjectGraph;
		std::string Message;
		FObjectPath ResolvedPath;
		bool bRedirected = false;
		std::optional<FObjectError> ObjectCause;
		std::optional<FPackageResourceRegistrationError> ResourceCause;
		std::optional<FEditorBulkDataStorageError> StorageCause;
	};

	// Codec and pending graph status; not a completed object-load value.
	struct FAssetReadResult
	{
		EAssetReadError Error = EAssetReadError::None;
		std::string Message;
		std::optional<FPackageResourceRegistrationError> ResourceCause;
		std::optional<FEditorBulkDataStorageError> StorageCause;

		auto Succeeded() const -> bool { return Error == EAssetReadError::None; }
		explicit operator bool() const { return Succeeded(); }
	};

	[[nodiscard]] inline auto AssetReadErrorFromResult(const FAssetReadResult& Result) -> FAssetReadError
	{
		check(!Result);
		return {.Code = Result.Error, .Message = Result.Message,
			.ResourceCause = Result.ResourceCause, .StorageCause = Result.StorageCause};
	}

	[[nodiscard]] inline auto AssetReadResultFromError(const FAssetReadError& Error) -> FAssetReadResult
	{
		return {.Error = Error.Code, .Message = Error.Message,
			.ResourceCause = Error.ResourceCause, .StorageCause = Error.StorageCause};
	}
}
