#pragma once

#include "EngineAPI.h"
#include "Diagnostics/Diagnostic.h"

namespace Durin
{
	// Describes mutation progress independently from the diagnostic error code.
	enum class EAssetResultDisposition : uint8
	{
		Default,
		ForwardPending,
		ContentCommittedProjectionPending,
		RecoveryRequired,
	};

	// Classifies Engine-owned asset loading, storage, Cook, and mutation failures.
	enum class EAssetError : uint8
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
		ReadOnlyMode,
		ShuttingDown
	};

	struct FAssetResult
	{
		EAssetError Error = EAssetError::None;
		std::string Message;
		EAssetResultDisposition Disposition = EAssetResultDisposition::Default;
		std::string OperationId;
		std::string DesiredDirection;
		std::string FailedParticipant;
		std::filesystem::path RecoveryLocation;

		auto Succeeded() const -> bool { return Error == EAssetError::None; }
		explicit operator bool() const { return Succeeded(); }
		ENGINE_API auto GetDiagnostic() const -> FDiagnostic;
	};
}
