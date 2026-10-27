#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "DObject/SoftObjectPtr.h"

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
		std::optional<FSoftObjectError> ObjectCause;
	};

	// Codec and pending graph status; not a completed object-load value.
	struct FAssetReadResult
	{
		EAssetReadError Error = EAssetReadError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetReadError::None; }
		explicit operator bool() const { return Succeeded(); }
	};

	[[nodiscard]] inline auto AssetReadErrorFromResult(const FAssetReadResult& Result) -> FAssetReadError
	{
		check(!Result);
		return {.Code = Result.Error, .Message = Result.Message};
	}

	[[nodiscard]] inline auto AssetReadResultFromError(const FAssetReadError& Error) -> FAssetReadResult
	{
		return {.Error = Error.Code, .Message = Error.Message};
	}
}
