#pragma once

#include "EngineAPI.h"
#include "Asset/PackageResourceError.h"
#include "Asset/EditorBulkDataStorageError.h"
#include "Diagnostics/Diagnostic.h"
#include "DObject/ObjectValidation.h"
#include "AssetRegistry/RegistryResult.h"

namespace Durin
{
	struct FCookDependencyGraphResult;
	struct FCookContributionResult;
	struct FCookInputFailure;
	// Describes mutation progress independently from the diagnostic error code.
	enum class EAssetResultDisposition : uint8
	{
		Default,
		ForwardPending,
		ContentCommittedProjectionPending,
		RecoveryRequired,
		PartiallyWritten,
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
		std::vector<std::filesystem::path> AffectedFiles;
		std::optional<FObjectValidationError> GraphValidationCause;
		std::optional<FAssetRegistryResult> RegistryCause;
		std::optional<FEditorBulkDataStorageError> BulkStorageCause;
		std::optional<FPackageResourceRegistrationError> ResourceRegistrationCause;
		std::shared_ptr<const FCookDependencyGraphResult> CookDependencyCause;
		std::shared_ptr<const FCookInputFailure> CookInputCause;
		std::shared_ptr<const FCookContributionResult> CookContributionCause;

		auto Succeeded() const -> bool { return Error == EAssetError::None; }
		explicit operator bool() const { return Succeeded(); }
		ENGINE_API auto GetDiagnostic() const -> FDiagnostic;
	};
}
