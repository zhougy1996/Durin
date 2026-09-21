#pragma once

#include "EngineAPI.h"
#include "Asset/PackageResourceError.h"
#include "Asset/EditorBulkDataStorageError.h"
#include "DObject/ObjectValidation.h"
#include "AssetRegistry/RegistryResult.h"

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

	struct FAssetReadResult
	{
		EAssetReadError Error = EAssetReadError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetReadError::None; }
		explicit operator bool() const { return Succeeded(); }
	};
}
